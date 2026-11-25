#include <winsock2.h>
#include <windows.h>
#include "net.h"

// Unit 4 host side. One UDP socket, one rx thread, newest-packet-wins.
// No protocol machinery beyond a sequence number: possession applies the
// freshest input and falls back to AI when nothing arrives for a second.
// Client side is the same mailbox pattern in reverse: the game-thread relay
// offers ctl snapshots, a sender thread owns the socket and paces the wire:
// for input, sendto never runs on the game thread, a winsock hitch must not
// stall a frame. Packet layout lives in net.h, single definition.
// Unit 5 adds the reverse arrow: the host replies WC3S wingman state to
// the freshest input source on the rx socket, the client parses it on its
// tx socket into an arrival-wins mailbox (g_cstate below for why not seq).
// The WC3S sendto runs on the host game relay thread by design, see
// wc3net_state_send.

#define REMOTE_TIMEOUT_MS 1000

static SOCKET g_sock = INVALID_SOCKET;
static HANDLE g_thread;

// crude log mirroring hook.c log_line, kept local to avoid header churn
extern void wc3net_log(const char *s);
extern void wc3net_log_crit(const char *s);
static volatile LONG g_rx_count;

void wc3net_rx_stats(LONG *total)
{
    *total = g_rx_count;
}
static struct {
    CRITICAL_SECTION lock;
    wc3net_packet pkt;
    DWORD rx_tick;
    int   fresh;
    DWORD src_ip;    // sender of the stored packet, seq is per-sender
    WORD  src_port;
} g_state;

// Unit 6: Out-of-mission gameflow & save-file mailboxes (WC3G).
static struct {
    CRITICAL_SECTION lock;
    int              init;
    wc3g_state_pkt   pkt;
    DWORD            rx_tick;
    int              have;
} g_gf_state;

// Client savegame reassembly buffer (one transfer at a time, max WC3G_SAVE_MAX).
#define SAVE_READY_QUEUE_MAX 8
static struct {
    CRITICAL_SECTION lock;
    int              init;
    unsigned long    xfer_id;
    unsigned long    total_size;
    unsigned char    chunk_count;
    unsigned long    chunk_mask;   // bit i set when chunk i received (up to 32 chunks = 28 KB)
    char             filename[24];
    unsigned char    buf[WC3G_SAVE_MAX];
    // Completed files queued for client game/writer thread to flush to disk
    int              q_head;
    int              q_tail;
    int              q_count;
    unsigned long    last_completed_xfer;
    struct {
        char          name[24];
        unsigned long size;
        unsigned char buf[WC3G_SAVE_MAX];
    } q[SAVE_READY_QUEUE_MAX];
} g_save_rx;

static volatile unsigned long g_save_ack_xfer;
static volatile DWORD g_gf_client_tick;
static volatile DWORD g_gf_host_tick;

static void gf_init_locks(void)
{
    if (!g_gf_state.init) {
        InitializeCriticalSection(&g_gf_state.lock);
        g_gf_state.init = 1;
    }
    if (!g_save_rx.init) {
        InitializeCriticalSection(&g_save_rx.lock);
        g_save_rx.init = 1;
    }
}

static DWORD WINAPI rx_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        unsigned char raw[1600];
        struct sockaddr_in from;
        int fromlen = sizeof(from);
        int n = recvfrom(g_sock, (char *)raw, sizeof(raw), 0,
                         (struct sockaddr *)&from, &fromlen);
        if (n < 4) continue;
        {
            unsigned long magic;
            CopyMemory(&magic, raw, 4);
            if (magic == WC3G_MAGIC) {
                // Client HELLO or SAVE_ACK on host socket
                if (n >= (int)sizeof(wc3g_hello_pkt) && raw[4] == WC3G_MSG_HELLO) {
                    EnterCriticalSection(&g_state.lock);
                    if (!g_state.fresh ||
                        from.sin_addr.s_addr != g_state.src_ip ||
                        ntohs(from.sin_port) != g_state.src_port) {
                        g_state.src_ip = from.sin_addr.s_addr;
                        g_state.src_port = ntohs(from.sin_port);
                        g_state.fresh = 1;
                        {
                            char hbuf[96];
                            wsprintfA(hbuf, "gf client hello from %u.%u.%u.%u:%u\r\n",
                                      (unsigned)(ntohl(from.sin_addr.s_addr) >> 24) & 0xff,
                                      (unsigned)(ntohl(from.sin_addr.s_addr) >> 16) & 0xff,
                                      (unsigned)(ntohl(from.sin_addr.s_addr) >> 8) & 0xff,
                                      (unsigned)ntohl(from.sin_addr.s_addr) & 0xff,
                                      ntohs(from.sin_port));
                            wc3net_log_crit(hbuf);
                        }
                    }
                    LeaveCriticalSection(&g_state.lock);
                    {
                        DWORD now = GetTickCount();
                        g_gf_client_tick = now ? now : 1;
                    }
                } else if (n >= 16 && raw[4] == WC3G_MSG_SAVE_ACK) {
                    wc3g_save_pkt ack;
                    ZeroMemory(&ack, sizeof(ack));
                    CopyMemory(&ack, raw, n < (int)sizeof(ack) ? n : (int)sizeof(ack));
                    g_save_ack_xfer = ack.xfer_id;
                }
                continue;
            }
            if (magic != 0x4E334357 /* WC3N */ || n != (int)sizeof(wc3net_packet))
                continue;
        }
        wc3net_packet p;
        CopyMemory(&p, raw, sizeof(p));
        InterlockedIncrement(&g_rx_count);
        if (g_rx_count <= 10 || (g_rx_count % 60) == 0) {
            char buf[160];
            wsprintfA(buf, "rx#%ld from %u.%u.%u.%u:%u seq=%lu p=%d l=%d r=%d t=%d b=%lu\r\n",
                      g_rx_count,
                      (unsigned)(ntohl(from.sin_addr.s_addr) >> 24) & 0xff,
                      (unsigned)(ntohl(from.sin_addr.s_addr) >> 16) & 0xff,
                      (unsigned)(ntohl(from.sin_addr.s_addr) >> 8) & 0xff,
                      (unsigned)ntohl(from.sin_addr.s_addr) & 0xff,
                      ntohs(from.sin_port), p.seq, p.pitch, p.lateral,
                      p.rudder, p.throttle, p.buttons);
            wc3net_log(buf);
        }
        EnterCriticalSection(&g_state.lock);
        // seq is a per-sender counter that restarts at 0 with every sender
        // process, so a bare seq comparison freezes the mailbox after any
        // client restart: the 2026-11-18 leg, a stick stream ran to seq
        // 13734, then two fresh sim senders were rejected as stale for
        // minutes. A changed source address means a new stream and always
        // wins, regardless of its seq.
        if ((int)(p.seq - g_state.pkt.seq) > 0 || !g_state.fresh ||
            from.sin_addr.s_addr != g_state.src_ip ||
            ntohs(from.sin_port) != g_state.src_port) {
            int switched = g_state.fresh &&
                (from.sin_addr.s_addr != g_state.src_ip ||
                 ntohs(from.sin_port) != g_state.src_port);
            if (switched) {
                char sbuf[64];
                wsprintfA(sbuf, "sender switch to %u.%u.%u.%u:%u\r\n",
                          (unsigned)(ntohl(from.sin_addr.s_addr) >> 24) & 0xff,
                          (unsigned)(ntohl(from.sin_addr.s_addr) >> 16) & 0xff,
                          (unsigned)(ntohl(from.sin_addr.s_addr) >> 8) & 0xff,
                          (unsigned)ntohl(from.sin_addr.s_addr) & 0xff,
                          ntohs(from.sin_port));
                wc3net_log_crit(sbuf);
            }
            g_state.pkt = p;
            g_state.rx_tick = GetTickCount();
            g_state.fresh = 1;
            g_state.src_ip = from.sin_addr.s_addr;
            g_state.src_port = ntohs(from.sin_port);
        }
        LeaveCriticalSection(&g_state.lock);
    }
    return 0;
}

int wc3net_remote_input(int *pitch, int *lateral, int *rudder,
                        int *throttle, DWORD *buttons)
{
    int ok = 0;
    if (g_sock == INVALID_SOCKET) return 0;
    EnterCriticalSection(&g_state.lock);
    if (g_state.fresh && GetTickCount() - g_state.rx_tick < REMOTE_TIMEOUT_MS) {
        *pitch    = g_state.pkt.pitch;
        *lateral  = g_state.pkt.lateral;
        *rudder   = g_state.pkt.rudder;
        *throttle = g_state.pkt.throttle;
        *buttons  = g_state.pkt.buttons;   // full dword, the relay picks its bytes
        ok = 1;
    }
    LeaveCriticalSection(&g_state.lock);
    return ok;
}

int wc3net_net_start(int port)
{
    WSADATA wsa;
    struct sockaddr_in addr;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return 0;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);
    if (bind(g_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        g_sock = INVALID_SOCKET;
        return 0;
    }
    InitializeCriticalSection(&g_state.lock);
    gf_init_locks();
    g_thread = CreateThread(NULL, 0, rx_thread, NULL, 0, NULL);
    return g_thread != NULL;
}

// WC3S state sender, host side. The reply rides the rx socket back to the
// freshest input source: input presence is client presence, no client
// address exists anywhere else to send to. Concurrent sendto with
// rx_thread's recvfrom on one datagram socket is safe, only the address
// copy needs the mailbox lock. Unlike the client input path this sendto
// runs on the state pump thread, accepted because one sub-MTU UDP send per
// frame has no connect or retry path to hitch on. Variable wire length,
// header + obj_count entries, one datagram.
#include <stddef.h>
typedef char wc3s_hdr_size_check[(offsetof(wc3s_packet, objects) == WC3S_HDR) ? 1 : -1];

int wc3net_state_send(const int pos[3], const int orient[9], const int vel[3],
                      int throttle, const wc3s_object *objs, int nobj,
                      int idx_blair, int idx_wing, unsigned long wing_voice,
                      unsigned char hdr_flags, const wc3s_wing_health *wh,
                      unsigned long long part_dead_mask,
                      unsigned char nav_idx)
{
    static DWORD seq;
    static DWORD fail_streak;
    struct sockaddr_in to;
    wc3s_packet p;
    int have;
    int len;

    if (g_sock == INVALID_SOCKET) return 0;
    if (nobj < 0) nobj = 0;
    if (nobj > WC3S_OBJ_MAX) nobj = WC3S_OBJ_MAX;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    EnterCriticalSection(&g_state.lock);
    have = g_state.fresh;              // set by the first input, never cleared
    to.sin_addr.s_addr = g_state.src_ip;
    to.sin_port = htons(g_state.src_port);
    LeaveCriticalSection(&g_state.lock);
    if (!have) return 0;

    ZeroMemory(&p, sizeof(p));
    p.magic = 0x53334357;   // WC3S
    p.seq = ++seq;
    CopyMemory(p.pos, pos, sizeof(p.pos));
    CopyMemory(p.orient, orient, sizeof(p.orient));
    CopyMemory(p.vel, vel, sizeof(p.vel));
    p.throttle = throttle;
    p.wing_voice = wing_voice;
    p.obj_count = (unsigned char)nobj;
    p.idx_blair = (unsigned char)idx_blair;
    p.idx_wing = (unsigned char)idx_wing;
    p.hdr_flags = hdr_flags;
    if (wh) {
        CopyMemory(p.wing_shld, wh->shld, 4);
        CopyMemory(p.wing_armor, wh->armor, 4);
        p.wing_hull = wh->hull;
        p.wing_flags = wh->flags;
    }
    p.part_dead_mask = part_dead_mask;
    p.nav_idx = nav_idx;
    if (nobj) CopyMemory(p.objects, objs, (DWORD)nobj * sizeof(wc3s_object));
    len = WC3S_HDR + nobj * (int)sizeof(wc3s_object);
    if (sendto(g_sock, (char *)&p, len, 0,
               (struct sockaddr *)&to, sizeof(to)) == SOCKET_ERROR) {
        fail_streak++;
        // first failure plus roughly 1 Hz while it persists: a dead client
        // must not flood the log at 60 Hz
        if (fail_streak == 1 || (fail_streak % 60) == 0) {
            char buf[96];
            wsprintfA(buf, "state sendto failed %lu, streak %lu\r\n",
                      WSAGetLastError(), fail_streak);
            wc3net_log_crit(buf);
        }
        return 0;
    }
    fail_streak = 0;
    return 1;
}

// Client sender. The relay offers from the game thread, this thread is the
// only socket writer, so the two never contend past the mailbox. Host and
// client roles never coexist in one process, separate sockets from the rx
// side above.
static SOCKET g_tx_sock = INVALID_SOCKET;
static struct sockaddr_in g_tx_dest;
static HANDLE g_tx_thread;
static struct {
    CRITICAL_SECTION lock;
    wc3net_ctl_snap snap;
    DWORD offer_tick;
    int   have;
} g_tx;

void wc3net_client_offer(const wc3net_ctl_snap *snap)
{
    if (g_tx_sock == INVALID_SOCKET) return;   // sender never started, drop here
    // newest snapshot wins, mirroring the rx side
    EnterCriticalSection(&g_tx.lock);
    g_tx.snap = *snap;
    g_tx.offer_tick = GetTickCount();
    g_tx.have = 1;
    LeaveCriticalSection(&g_tx.lock);
}

static DWORD WINAPI tx_thread(LPVOID unused)
{
    LONG seq = 0;
    DWORD fail_streak = 0;
    (void)unused;
    for (;;) {
        wc3net_ctl_snap snap;
        int fresh;
        Sleep(1000 / 60);
        EnterCriticalSection(&g_tx.lock);
        // go quiet a second after the last offer: a paused or hung client
        // game then stops feeding the host, whose own 1 s timeout hands the
        // wingman back to AI
        fresh = g_tx.have && GetTickCount() - g_tx.offer_tick < REMOTE_TIMEOUT_MS;
        snap = g_tx.snap;
        LeaveCriticalSection(&g_tx.lock);
        if (!fresh) continue;
        {
            wc3net_packet p;
            p.magic = 0x4E334357;   // WC3N
            p.seq = (DWORD)++seq;
            p.pitch = snap.pitch;
            p.lateral = snap.lateral;
            p.rudder = snap.rudder;
            p.throttle = snap.throttle;
            p.buttons = ((DWORD)snap.buttons & 0xffff00ffUL) |
                        (((DWORD)snap.flags & 0xff) << 8);
            if (sendto(g_tx_sock, (char *)&p, sizeof(p), 0,
                       (struct sockaddr *)&g_tx_dest, sizeof(g_tx_dest)) == SOCKET_ERROR) {
                fail_streak++;
                // first failure plus roughly 1 Hz while it persists: a dead
                // host must not flood the log at 60 Hz
                if (fail_streak == 1 || (fail_streak % 60) == 0) {
                    char buf[96];
                    wsprintfA(buf, "tx sendto failed %lu, streak %lu\r\n",
                              WSAGetLastError(), fail_streak);
                    wc3net_log_crit(buf);
                }
            } else {
                fail_streak = 0;
            }
        }
    }
    return 0;
}

int wc3net_client_start(const char *host, int port)
{
    WSADATA wsa;
    struct sockaddr_in addr;
    unsigned long ip;
    SOCKET sock;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    ip = inet_addr(host);   // the ini carries an ip, no name lookup by design
    if (ip == INADDR_NONE) {
        wc3net_log_crit("client host is not an ip, not sending\r\n");
        return 0;
    }
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return 0;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip;
    addr.sin_port = htons((u_short)port);
    g_tx_dest = addr;
    InitializeCriticalSection(&g_tx.lock);
    gf_init_locks();
    g_tx_thread = CreateThread(NULL, 0, tx_thread, NULL, 0, NULL);
    if (!g_tx_thread) return 0;
    // visible to offer() only now, after the lock exists: a relay call
    // racing this start can never enter an uninitialized critical section
    g_tx_sock = sock;
    return 1;
}

// Client state mailbox, WC3S receive side. ARRIVAL-WINS, deliberately not
// the input mailbox's seq comparison: every WC3S is a full absolute
// snapshot from one host, so a reordered datagram costs one 16 ms frame of
// older state that the next packet overwrites, invisible in a cockpit.
// Ranking by seq would reintroduce rx_thread's ghost-seq freeze (a
// restarted sender restarts at 0 and loses forever) for zero visible gain.
static struct {
    CRITICAL_SECTION lock;
    wc3s_packet pkt;
    DWORD rx_tick;
    int   have;
} g_cstate;
static volatile int g_cstate_up;   // set after the lock exists, fetch gate
static HANDLE g_cstate_thread;

static DWORD WINAPI state_rx_thread(LPVOID unused)
{
    static DWORD stale_hint_tick;
    (void)unused;
    for (;;) {
        unsigned char raw[1600];
        int n = recvfrom(g_tx_sock, (char *)raw, sizeof(raw), 0, NULL, NULL);
        if (n < 4) continue;
        {
            unsigned long magic;
            CopyMemory(&magic, raw, 4);
            if (magic == WC3G_MAGIC) {
                if (n == (int)sizeof(wc3g_state_pkt) && raw[4] == WC3G_MSG_STATE) {
                    EnterCriticalSection(&g_gf_state.lock);
                    CopyMemory(&g_gf_state.pkt, raw, sizeof(wc3g_state_pkt));
                    g_gf_state.rx_tick = GetTickCount();
                    g_gf_state.have = 1;
                    LeaveCriticalSection(&g_gf_state.lock);
                    {
                        DWORD now = GetTickCount();
                        g_gf_host_tick = now ? now : 1;
                    }
                } else if (n >= 38 && raw[4] == WC3G_MSG_SAVE_DAT) {
                    wc3g_save_pkt sp;
                    ZeroMemory(&sp, sizeof(sp));
                    CopyMemory(&sp, raw, n < (int)sizeof(sp) ? n : (int)sizeof(sp));
                    if (sp.total_size > 0 && sp.total_size <= WC3G_SAVE_MAX &&
                        sp.chunk_count > 0 && sp.chunk_count <= 32 &&
                        sp.chunk_idx < sp.chunk_count &&
                        sp.data_len <= WC3G_CHUNK_MAX) {
                        unsigned long off = (unsigned long)sp.chunk_idx * WC3G_CHUNK_MAX;
                        if (off + sp.data_len <= sp.total_size) {
                            int complete = 0;
                            EnterCriticalSection(&g_save_rx.lock);
                            if (sp.xfer_id != g_save_rx.xfer_id) {
                                g_save_rx.xfer_id = sp.xfer_id;
                                g_save_rx.total_size = sp.total_size;
                                g_save_rx.chunk_count = sp.chunk_count;
                                g_save_rx.chunk_mask = 0;
                                CopyMemory(g_save_rx.filename, sp.filename, 22);
                                g_save_rx.filename[22] = 0;
                            }
                            CopyMemory(g_save_rx.buf + off, sp.data, sp.data_len);
                            g_save_rx.chunk_mask |= (1UL << sp.chunk_idx);
                            {
                                unsigned long full = (sp.chunk_count == 32)
                                    ? 0xffffffffUL : ((1UL << sp.chunk_count) - 1UL);
                                if ((g_save_rx.chunk_mask & full) == full) {
                                    if (sp.xfer_id != g_save_rx.last_completed_xfer &&
                                        g_save_rx.q_count < SAVE_READY_QUEUE_MAX) {
                                        int idx = g_save_rx.q_tail;
                                        CopyMemory(g_save_rx.q[idx].name, g_save_rx.filename, 24);
                                        CopyMemory(g_save_rx.q[idx].buf, g_save_rx.buf, g_save_rx.total_size);
                                        g_save_rx.q[idx].size = g_save_rx.total_size;
                                        g_save_rx.q_tail = (idx + 1) % SAVE_READY_QUEUE_MAX;
                                        g_save_rx.q_count++;
                                        g_save_rx.last_completed_xfer = sp.xfer_id;
                                    }
                                    complete = 1;
                                }
                            }
                            LeaveCriticalSection(&g_save_rx.lock);
                            if (complete) {
                                wc3g_save_pkt ack;
                                ZeroMemory(&ack, sizeof(ack));
                                ack.magic = WC3G_MAGIC;
                                ack.msg_type = WC3G_MSG_SAVE_ACK;
                                ack.xfer_id = sp.xfer_id;
                                sendto(g_tx_sock, (char *)&ack, 16, 0,
                                       (struct sockaddr *)&g_tx_dest, sizeof(g_tx_dest));
                            }
                        }
                    }
                }
                continue;
            }
            if (magic != 0x53334357 /* WC3S */) continue;
        }
        wc3s_packet p;
        if (n > (int)sizeof(p)) continue;
        CopyMemory(&p, raw, n);
        // a pre-v0.10 host sends exactly 72 B; hint once so a
        // mixed-version pair diagnoses itself from the log
        if (n == 72) {
            if (GetTickCount() - stale_hint_tick > 10000) {
                stale_hint_tick = GetTickCount();
                wc3net_log_crit("72 B WC3S from a pre-v0.10 host, upgrade both sides\r\n");
            }
            continue;
        }
        // variable length: header + obj_count entries, nothing else
        if (n < WC3S_HDR || p.obj_count > WC3S_OBJ_MAX ||
            n != WC3S_HDR + p.obj_count * (int)sizeof(wc3s_object)) continue;
        EnterCriticalSection(&g_cstate.lock);
        g_cstate.pkt = p;
        g_cstate.rx_tick = GetTickCount();
        g_cstate.have = 1;
        LeaveCriticalSection(&g_cstate.lock);
    }
    return 0;
}

// The host replies to the tx socket's ephemeral source port, so WC3S
// arrives where input leaves: one rx thread on g_tx_sock, no new socket.
int wc3net_state_start_rx(void)
{
    if (g_tx_sock == INVALID_SOCKET) return 0;
    InitializeCriticalSection(&g_cstate.lock);
    g_cstate_thread = CreateThread(NULL, 0, state_rx_thread, NULL, 0, NULL);
    if (!g_cstate_thread) return 0;
    g_cstate_up = 1;
    return 1;
}

int wc3net_state_fetch(wc3s_packet *out)
{
    int ok = 0;
    if (!g_cstate_up) return 0;
    EnterCriticalSection(&g_cstate.lock);
    if (g_cstate.have && GetTickCount() - g_cstate.rx_tick < REMOTE_TIMEOUT_MS) {
        *out = g_cstate.pkt;
        ok = 1;
    }
    LeaveCriticalSection(&g_cstate.lock);
    return ok;
}

// ---- Unit 6: WC3G gameflow & savegame transport ----

int wc3net_gf_client_present(void)
{
    DWORD t = g_gf_client_tick;
    return (t != 0 && (GetTickCount() - t) < 5000);
}

int wc3net_gf_host_present(void)
{
    DWORD t = g_gf_host_tick;
    return (t != 0 && (GetTickCount() - t) < 5000);
}

int wc3net_gf_send_state(const wc3g_state_pkt *pkt)
{
    struct sockaddr_in to;
    int have;
    if (g_sock == INVALID_SOCKET || !pkt) return 0;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    EnterCriticalSection(&g_state.lock);
    have = g_state.fresh;
    to.sin_addr.s_addr = g_state.src_ip;
    to.sin_port = htons(g_state.src_port);
    LeaveCriticalSection(&g_state.lock);
    if (!have) return 0;
    return sendto(g_sock, (const char *)pkt, (int)sizeof(*pkt), 0,
                  (struct sockaddr *)&to, sizeof(to)) != SOCKET_ERROR;
}

int wc3net_gf_fetch_state(wc3g_state_pkt *out)
{
    int ok = 0;
    if (!g_gf_state.init || !out) return 0;
    EnterCriticalSection(&g_gf_state.lock);
    if (g_gf_state.have && (GetTickCount() - g_gf_state.rx_tick) < 5000) {
        *out = g_gf_state.pkt;
        ok = 1;
    }
    LeaveCriticalSection(&g_gf_state.lock);
    return ok;
}

int wc3net_gf_send_hello(int act, int scene, int room)
{
    static unsigned long hseq;
    wc3g_hello_pkt h;
    if (g_tx_sock == INVALID_SOCKET) return 0;
    ZeroMemory(&h, sizeof(h));
    h.magic = WC3G_MAGIC;
    h.msg_type = WC3G_MSG_HELLO;
    h.client_act = (unsigned char)act;
    h.seq = ++hseq;
    h.client_scene = scene;
    h.client_room = room;
    return sendto(g_tx_sock, (const char *)&h, (int)sizeof(h), 0,
                  (struct sockaddr *)&g_tx_dest, sizeof(g_tx_dest)) != SOCKET_ERROR;
}

int wc3net_gf_send_save_file(const char *filename, const unsigned char *data, unsigned long size)
{
    static unsigned long xfer_seq;
    struct sockaddr_in to;
    int have, attempt, nchunks, i;
    unsigned long xid;

    if (g_sock == INVALID_SOCKET || !filename || !data || size == 0 || size > WC3G_SAVE_MAX)
        return 0;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    EnterCriticalSection(&g_state.lock);
    have = g_state.fresh;
    to.sin_addr.s_addr = g_state.src_ip;
    to.sin_port = htons(g_state.src_port);
    LeaveCriticalSection(&g_state.lock);
    if (!have) return 0;

    nchunks = (int)((size + WC3G_CHUNK_MAX - 1) / WC3G_CHUNK_MAX);
    if (nchunks > 32) return 0;
    xid = ++xfer_seq;

    // Send up to 4 rounds (100 ms apart) until client ACKs this xfer_id
    for (attempt = 0; attempt < 4; attempt++) {
        for (i = 0; i < nchunks; i++) {
            wc3g_save_pkt sp;
            unsigned long off = (unsigned long)i * WC3G_CHUNK_MAX;
            unsigned long rem = size - off;
            unsigned short clen = (unsigned short)(rem > WC3G_CHUNK_MAX ? WC3G_CHUNK_MAX : rem);
            int pkt_len;
            ZeroMemory(&sp, sizeof(sp));
            sp.magic = WC3G_MAGIC;
            sp.msg_type = WC3G_MSG_SAVE_DAT;
            sp.chunk_idx = (unsigned char)i;
            sp.chunk_count = (unsigned char)nchunks;
            sp.xfer_id = xid;
            sp.total_size = size;
            sp.data_len = clen;
            lstrcpynA(sp.filename, filename, 22);
            CopyMemory(sp.data, data + off, clen);
            pkt_len = (int)offsetof(wc3g_save_pkt, data) + (int)clen;
            sendto(g_sock, (const char *)&sp, pkt_len, 0,
                   (struct sockaddr *)&to, sizeof(to));
        }
        Sleep(100);
        if (g_save_ack_xfer == xid) return 1;
    }
    return (g_save_ack_xfer == xid);
}

int wc3net_gf_pop_save_file(char *filename_out, unsigned char *data_out, unsigned long *size_out)
{
    int ok = 0;
    if (!g_save_rx.init || !filename_out || !data_out || !size_out) return 0;
    EnterCriticalSection(&g_save_rx.lock);
    if (g_save_rx.q_count > 0) {
        int idx = g_save_rx.q_head;
        CopyMemory(filename_out, g_save_rx.q[idx].name, 24);
        CopyMemory(data_out, g_save_rx.q[idx].buf, g_save_rx.q[idx].size);
        *size_out = g_save_rx.q[idx].size;
        g_save_rx.q_head = (idx + 1) % SAVE_READY_QUEUE_MAX;
        g_save_rx.q_count--;
        ok = 1;
    }
    LeaveCriticalSection(&g_save_rx.lock);
    return ok;
}

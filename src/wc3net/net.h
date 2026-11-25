#ifndef WC3NET_NET_H
#define WC3NET_NET_H

// WC3N UDP packet, the single layout definition, shared by host rx and
// client tx (and notes/design-client-input.md). buttons packs two bytes:
// bits 0-7 carry ctl+4 one-shot buttons, bits 8-15 carry ctl+5 flags, of
// which the host applies only bit4 afterburner and bit7 eject. ctl+5 bit6
// (locked) mirrors host-local object state the client does not own.
// Senders that leave the high byte 0 behave exactly as before.
typedef struct wc3net_packet {
    unsigned long magic;      // 'WC3N'
    unsigned long seq;
    int  pitch;               // ctl+0x10
    int  lateral;             // ctl+0xc
    int  rudder;              // ctl+0x14
    int  throttle;            // ctl+8, 0..100
    unsigned long buttons;    // ctl+4 | ctl+5 << 8
} wc3net_packet;

#define WC3N_BTN_CLIENT_EJECT 0x00010000UL  // bit 16 of wc3net_packet.buttons: client pressed Alt+E eject

// Fresh player control block snapshot, offered by the client relay after
// the game's own input pipeline filled it. Field offsets per
// notes/unit4_ctl/findings.md.
typedef struct wc3net_ctl_snap {
    int buttons;   // ctl+4
    int flags;     // ctl+5
    int throttle;  // ctl+8, 0..100
    int lateral;   // ctl+0xc
    int pitch;     // ctl+0x10
    int rudder;    // ctl+0x14
} wc3net_ctl_snap;

// WC3S state snapshot, host -> client. Own-ship block per
// notes/unit5/v1-design.md: the wingman physics block copied raw
// (phys = *(ship+4)): pos[3] at phys+0x28..+0x30, orient[9] at
// phys+0x34..+0x57 (3x3 24.8 fixed point, identity diagonal 0x100), vel[3]
// at phys+0x58..+0x60. throttle is ctl+8 (ctl = *(ship+0x7c8) on the host),
// 0..100. The client writes the same offsets on its own PLYR, so no scale
// conversion exists anywhere. Packed so later BYTE/WORD HUD additions
// cannot silently pad the wire layout.
//
// Unit 5 increment (a) appends the world objects[] block per
// notes/unit5/v2-objects-design.md: every live ship in the host mission,
// Blair included, streamed so the client stomps its whole world each frame.
// Variable wire length: header + obj_count * 56 B, one datagram, 976 B at
// the 16-object cap, under the 1500 B MTU so UDP never fragments.

// WC3S header flags (wc3s_packet.hdr_flags, formerly reserved0).
#define WC3S_FLAG_AUTOPILOT   0x01  // host is in autopilot/landing cutscene (DAT_004a3524)
#define WC3S_FLAG_MISSION_END 0x02  // host mission ended; throttle field carries DAT_004a41c8 exit status
#define WC3S_FLAG_NAVMAP      0x04  // host is inside the 3D Nav Map (CAM_MODE_VA == 10)

// One streamed ship. Fields read straight off the registry node (the node
// IS the phys object, FUN_0044f180 iterates them at stride 0x7e): pos at
// +0x28, orient at +0x34. owner is the byte at node+0x70, dead at node+0x7d.
// namehash is FNV-1a over the 8-char type name at (*(node+0x24))+0x2d,
// computed identically on both sides, so the client verifies every stomp
// target without names ever crossing the wire.
typedef struct __attribute__((packed)) wc3s_object {
    unsigned long namehash;
    int   pos[3];             // node+0x28
    int   orient[9];          // node+0x34
    unsigned char owner;      // node+0x70
    unsigned char flags;      // b0 dead (node+0x7d), b1 guns firing (ctl+4 bit0), b2 alt-fire/missile (ctl+4 bit2)
    unsigned char part_idx;   // participant table index (0..254, 0xff = unknown)
    unsigned char reserved1;  // zero
} wc3s_object;                // 56 B

// WC3S wing_flags bits
#define WC3S_WING_HEALTH_OK 0x01  // wing_shld/wing_armor/wing_hull valid
#define WC3S_WING_HIT       0x02  // wingman took a hit (trigger cockpit flash)
#define WC3S_WING_DEAD      0x04  // wingman destroyed on host

#define WC3S_OBJ_MAX 16
#define WC3S_HDR     100      // bytes before objects[]

typedef struct __attribute__((packed)) wc3s_packet {
    unsigned long magic;      // 'WC3S'
    unsigned long seq;
    int  pos[3];              // wingman phys+0x28
    int  orient[9];           // wingman phys+0x34
    int  vel[3];              // wingman phys+0x58
    int  throttle;            // wingman ctl+8, 0..100
    unsigned long wing_voice; // wingman phys+0x9c value, client finds its
                              // local wingman node by matching it, 0 = absent
    unsigned char obj_count;  // objects[] entries on the wire
    unsigned char idx_blair;  // objects[] slot of the host PLYR, 0xff absent
    unsigned char idx_wing;   // objects[] slot of the host wingman, 0xff absent
    unsigned char hdr_flags;  // WC3S_FLAG_* (b0 autopilot, b1 mission-end)
    unsigned char wing_shld[4];  // wingman shield faces 0..3 scaled 0..255
    unsigned char wing_armor[4]; // wingman armor faces 0..3 scaled 0..255
    unsigned char wing_hull;     // wingman hull scaled 0..255
    unsigned char wing_flags;    // WC3S_WING_*
    unsigned char nav_idx;       // host selected nav script index (simstate+0x9d, 0xff = unknown)
    unsigned char reserved2;     // zero
    unsigned long long part_dead_mask; // bit i = 1 iff host participant i is dead/destroyed
    wc3s_object objects[WC3S_OBJ_MAX];
} wc3s_packet;

typedef struct wc3s_wing_health {
    unsigned char shld[4];
    unsigned char armor[4];
    unsigned char hull;
    unsigned char flags;
} wc3s_wing_health;

int wc3net_net_start(int port);
int wc3net_remote_input(int *pitch, int *lateral, int *rudder,
                        int *throttle, unsigned long *buttons);
int wc3net_client_start(const char *host, int port);
void wc3net_client_offer(const wc3net_ctl_snap *snap);

// state mirror + world stream, WC3S host -> client
int wc3net_state_send(const int pos[3], const int orient[9], const int vel[3],
                      int throttle, const wc3s_object *objs, int nobj,
                      int idx_blair, int idx_wing, unsigned long wing_voice,
                      unsigned char hdr_flags, const wc3s_wing_health *wh,
                      unsigned long long part_dead_mask,
                      unsigned char nav_idx);
int wc3net_state_start_rx(void);
int wc3net_state_fetch(wc3s_packet *out);

// Unit 6: Out-of-mission gameflow/campaign sync & savegame mirroring (WC3G).
// Rides the same UDP 3443 socket as WC3N/WC3S, distinguished by 4-byte magic.
#define WC3G_MAGIC        0x47334357UL  // 'WC3G'

#define WC3G_MSG_HELLO    1  // client -> host keepalive / presence while out of mission
#define WC3G_MSG_STATE    2  // host -> client campaign/room/event heartbeat (~10 Hz)
#define WC3G_MSG_SAVE_HDR 3  // host -> client save file metadata (start transfer)
#define WC3G_MSG_SAVE_DAT 4  // host -> client save file chunk (<= 900 B)
#define WC3G_MSG_SAVE_ACK 5  // client -> host save transfer ACK

#define WC3G_EVT_NONE     0
#define WC3G_EVT_CLICK    1  // hotspot gump clicked (arg0 = gump_id)
#define WC3G_EVT_BRANCH   2  // dialogue branch chosen (arg0 = 0 or 1, arg1 = branch_id)
#define WC3G_EVT_LOAD     3  // savegame slot loaded (arg0 = slot)
#define WC3G_EVT_MISSION  4  // launching mission (arg0 = mission_id)
#define WC3G_EVT_SKIP     5  // host skipped current FMV movie segment
#define WC3G_EVT_REPLAY     6  // host chose Replay (0), Continue (1), or Quit (2) on death/eject screen
#define WC3G_EVT_DEBUG_MISN 7  // host debug menu mission launch (arg0 = mission_id, arg1 = need_cd)

// wc3g_state_pkt.reserved (host_flags) bits
#define WC3G_HFLAG_IN_MOVIE    0x0001  // host is inside FUN_0041c240 movie playback (DAT_004a24cc != 0)
#define WC3G_HFLAG_IN_BRANCH   0x0002  // host is inside dialogue branch choice modal (vtable 0x49b8a0)
#define WC3G_HFLAG_IN_HD_MOV   0x0004  // host is actively playing a wc3en LibVlc_Movie (node+0x3c != 0)
#define WC3G_HFLAG_DECK_ACTIVE 0x0008  // host is interactively standing in room_id (DAT_004a3578 != 0)
#define WC3G_HFLAG_IN_MISSION  0x0010  // host is inside an active campaign spaceflight mission
#define WC3G_HFLAG_IN_SIM      0x0020  // host is inside an active simulator spaceflight mission
// Redundant dialogue branch channel packed in upper bits of reserved (zero packet size change):
//   bits 8..11 = 4-bit branch choice counter (1..15, wraps 1..15; 0 = none yet)
//   bit 12     = latest logical branch choice (0 or 1)
// Survives even if host presses ESC immediately after picking a choice (overwriting evt_type).
#define WC3G_HFLAG_BSEQ_SHIFT  8
#define WC3G_HFLAG_BSEQ_MASK   0x0f00
#define WC3G_HFLAG_BCHOICE_BIT 0x1000

#define WC3G_CHUNK_MAX    900
#define WC3G_SAVE_MAX     16384

// Compact campaign state header + event + flags/regs snapshot.
// Fits in one UDP datagram (~1450 B) without fragmentation.
typedef struct __attribute__((packed)) wc3g_state_pkt {
    unsigned long magic;        // WC3G_MAGIC
    unsigned char msg_type;     // WC3G_MSG_STATE
    unsigned char evt_type;     // WC3G_EVT_*
    unsigned short reserved;
    unsigned long seq;          // monotonic packet seq
    unsigned long evt_seq;      // monotonic event counter (increments on click/branch/load)
    int  evt_arg0;              // gump_id / branch choice / slot
    int  evt_arg1;              // extra arg (e.g., branch_id / room)
    int  script_act;            // DAT_004a3580 (0=exit,1=prologue,2=new,3=sim,4=deck,5=mission)
    int  scene_id;              // DAT_004a3584
    int  mission_id;            // DAT_004a3588
    int  room_id;               // DAT_004ab84c
    int  flags[256];            // DAT_004aba08 (1024 B)
    int  regs[96];              // DAT_004ab840 (384 B)
} wc3g_state_pkt;               // 4+4+4+4+4+4+16+1024+384 = 1448 B (under 1472 B UDP MTU)

typedef struct __attribute__((packed)) wc3g_hello_pkt {
    unsigned long magic;        // WC3G_MAGIC
    unsigned char msg_type;     // WC3G_MSG_HELLO
    unsigned char client_act;   // client's DAT_004a3580
    unsigned short reserved;
    unsigned long seq;
    int  client_scene;          // client's DAT_004a3584
    int  client_room;           // client's DAT_004ab84c
} wc3g_hello_pkt;

typedef struct __attribute__((packed)) wc3g_save_pkt {
    unsigned long magic;        // WC3G_MAGIC
    unsigned char msg_type;     // WC3G_MSG_SAVE_DAT or WC3G_MSG_SAVE_ACK
    unsigned char chunk_idx;
    unsigned char chunk_count;
    unsigned char reserved;
    unsigned long xfer_id;      // monotonic transfer ID
    unsigned long total_size;
    unsigned short data_len;
    char filename[22];          // e.g. "00000000.WSG" or "WSG_NDX.WSG"
    unsigned char data[WC3G_CHUNK_MAX];
} wc3g_save_pkt;

int wc3net_gf_send_state(const wc3g_state_pkt *pkt);
int wc3net_gf_fetch_state(wc3g_state_pkt *out);
int wc3net_gf_send_hello(int act, int scene, int room);
int wc3net_gf_client_present(void);
int wc3net_gf_host_present(void);
int wc3net_gf_send_save_file(const char *filename, const unsigned char *data, unsigned long size);
int wc3net_gf_pop_save_file(char *filename_out, unsigned char *data_out, unsigned long *size_out);

#endif

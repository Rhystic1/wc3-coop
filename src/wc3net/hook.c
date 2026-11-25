#include <windows.h>
#include "net.h"
#include "missions.h"

// v0.10.5. Weapons fire mirror fix (weap_relay) + wingman shield/armor/hull
// sync (2026-11-19):
//   1) Weapons fire mirror root-cause fix: in v0.10.4, apply_world wrote
//      ctl+4 inside plyr_relay, then FUN_00463890 (FGTR update) ran
//      FUN_00410440 and wiped ctl+4 to 0 right before FUN_0040c370 (WEAP
//      slot 1) consumed it, so host Blair's guns/missiles never fired on
//      the client. Fixed by hooking fighter WEAP vtable slot 1 (0x49b524
//      -> FUN_0040c370) on both host and client:
//      - Host weap_relay latches ctl+4 guns (bit0, 65 ms) & missile (bit2,
//        120 ms) per fighter phys right as FUN_0040c370 runs, so state_pump
//        never misses a trigger or 1-frame missile press.
//      - Client weap_relay runs AFTER FUN_00410440 and RIGHT BEFORE
//        FUN_0040c370: leaves client's own PLYR untouched, clears local AI
//        fire on other fighters, and injects host's mirrored fire bits
//        (with energy/gun-bank/missile-ready guards) so host Blair's
//        cannons & missiles (and enemy fire) visibly launch on the client.
//   2) Wingman shield/armor/hull sync (WC3S_HDR 80 -> 92 B): host reads
//      g_wingman's SHLD (*(phys+0xa0), vtable 0x49d128: 4 shield faces
//      +0x20..+0x2c, 4 armor faces +0x40..+0x4c, maxes in *(shld+8)+8..24)
//      and DMGA (*(phys+0xa4): hull +0x40 / max +0x3c) scaled 0..255 plus
//      hit-flash flag; client stomps them onto its own PLYR so host
//      friendly fire and enemy damage show up on the client's cockpit HUD.

// v0.10.4. Unit 5 increments (d) mission-end sync + (e) enemy death sync
// & weapons fire mirror (2026-11-19):
//   1) Mission-end status sync: when host DAT_004a41c8 leaves 1 (e.g.
//      2 = landed/win, 3/4 = death/eject), state_pump sends a short burst
//      of WC3S_FLAG_MISSION_END packets carrying the host's DAT_004a41c8
//      in the throttle field before pausing. The client's plyr_relay
//      writes that status straight to its own DAT_004a41c8 so the client
//      exits the mission cleanly with the host's outcome instead of
//      watching the wingman drift/explode after teardown.
//   2) Enemy death sync: collect_world now includes dead ships
//      (phys[0x7d] != 0) so deaths propagate on the wire; when a client
//      local ship's participant entry was already spawned and is no longer
//      present in the host's active list (or arrives with dead flag set),
//      the client triggers the game's own kill sequence (phys[0x7d]=1,
//      phys+0x90=0x300 so FUN_0046ae10 explodes & removes it cleanly).
//   3) Weapons fire visual mirror: host reads ctl+4 fire bits (bit0 guns,
//      bit2 alt/missile, via FUN_0046c1d0's ship+0x650/0x7c8/0x6b8) into
//      wc3s_object.flags bits 1-2; client writes those bits into the
//      paired local ship's ctl+4 (including host Blair on local_wing_ship)
//      so the client sees host guns & missiles firing.

// v0.10.3. Unit 5 increments (b) lifecycle safety + (c) autopilot rules +
// participant-index pairing & log hygiene (2026-11-19):
//   1) Host-side mission & wingman validity gate: stop state_pump whenever
//      DAT_004a41ac == 0 (mission torn down), DAT_004a41c8 != 1 (not in
//      active flight), or g_wingman / its phys is invalid or dead; clear
//      g_wingman and g_wingman_flight on mission exit so Replay/landing
//      never streams dangling heap garbage to the client.
//   2) Client autopilot forbidden + host autopilot replication:
//      plyr_relay clears DAT_004a2da4 (local 'A' autopilot trigger) so
//      the client can never autopilot on its own; when the host enters
//      autopilot/landing (DAT_004a3524 != 0, carried in WC3S hdr_flags
//      bit0), the client triggers DAT_004a2da4 = 1 on the rising edge
//      so both instances perform the same sector/nav transition together
//      and spawn the same enemy waves.
//   3) Exact participant-table index pairing: wc3s_object.part_idx carries
//      the host participant index (0..254, identical on both sides from
//      the same mission file); client pairs by part_idx first, falling
//      back to owner+namehash.
//   4) Remove 10 Hz scan_registry log spam (keep collect_world's 1 Hz
//      walk line).

// v0.10.2. Registry walk root cause fixed (2026-11-19). Three chained
// static errors caused cnt=0 all session in v0.10 / v0.10.1 and zero
// scan lines in every earlier session:
//   1) The participant array header sits at simstate + 0x3b (not +0xec;
//      0xec = 0x3b * 4 was a Ghidra int*-scaled pointer-arithmetic
//      misread of FUN_0044ff40's (int *)(param_1 + 0x3b) and
//      FUN_00452bc0's (int *)((int)this + 0x3b)).
//   2) The 0x7e-byte array entries are mission participant descriptors,
//      NOT phys objects by value: entry+0x76 points to the live PHYS
//      object (proven by FUN_0044ea80 reading *(entry+0x76)+0x28..0x30,
//      FUN_0044f150 comparing *(entry+0x76) == phys, FUN_00452bc0
//      collecting *(entry+0x76) into the phys list for FUN_004342a0/4870).
//   3) The owning ship controller sits at *(phys + 0x94) (proven by
//      FUN_0046ae10's vcall on **(phys+0x94), matching the wingman's
//      ship+4 == phys). Ship class vtables are 0x49c188 (PLYR), 0x49d3b8
//      (FGTR), 0x49c1a8 (CPTL) -- 0x49c160/0x49d3a8 were the 4-byte CTRL
//      factory vtables, not ship instance vtables.
// Dead check is phys+0x7d (the exact byte FUN_00452bc0 and FUN_0046ae10
// test); both host and client now walk simstate+0x3b -> entry+0x76 ->
// phys -> *(phys+0x94) ship.

// v0.10.1. Two leg-2026-11-19 root causes fixed. 1) The walk filtered on
// the registry flag word bits (0x20 is-ship, 0x4 remove) exactly as
// FUN_00452bc0 does, but those bits are frame-phase-correlated state that
// flickers WITHIN one frame (the wingman's own phys+0x50 sampled during
// its update ran ffffff82, 00000000, 000000e6, ffffffff, ...) and an
// async 60 Hz sampler never caught a passing instant: ships=0 all
// session. The filter is now class-exact and phase-immune: a node is a
// live ship iff its node+0x76 backptr object's vtable is one of the
// three known ship classes and that ship's +0x7d dead byte is clear,
// both straight from FUN_00452bc0's own test. 2) DAT_004ab3c0 holds the
// player PHYS pointer directly (the dispatcher compares it against its
// own phys param), the Blair synth was chasing one indirection too many
// through *(ptr+4) and failed its guard every frame: blair=0 all
// session, so the client never received a Blair object and the local
// wingman flew unsynced local AI into the synced cockpit, the reported
// invisible-Hobbes collision-eject.

// v0.10. World objects, Unit 5 increment (a), per
// notes/unit5/v2-objects-design.md. The host state pump walks the sim
// registry every frame (FUN_0044f180's own iteration: nodes by value,
// stride 0x7e) and ships every live ship, Blair included, in the WC3S
// objects[] block. The client pairs each host object to a local node
// statelessly, by owner plus type-name hash in registry order, and stomps
// the transform per frame, so its local sim can never express divergent
// state. The client's own PLYR keeps carrying the wingman transform (the
// v1 cockpit), so the host's Blair object is swapped onto the client's
// local wingman node, making the host craft visible. Client-side range
// and teleport guards hold the host-teardown garbage stream (the Replay
// crash) away from every stomp. Data fields only, no game code bytes are
// ever written. STANDING RULE.

// v0.5. Own-ship mirror on the v0.4 seam, per notes/unit5/v1-design.md.
// mode=remote ships the wingman's phys pos/orient/vel plus ctl throttle
// back to the freshest input source (WC3S, net.c); mode=client stomps its
// own PLYR with the freshest WC3S, post-original, the proven one-frame-late
// possession ordering. The camera family follows the player-ship global,
// so overwriting the player transform moves the cockpit with no camera
// surgery. Data fields only, no game code bytes are ever written.
// STANDING RULE.

// v0.4. Scripted possession on top of the v0.3 relay. After the original
// FGTR update runs, the wingman's control block (ship+0x7c8) is overwritten
// with a scripted pattern per notes/unit4_ctl/findings.md. Fields are data
// only, no game code bytes are ever written. STANDING RULE.

// v0.3. Vtable-entry swap on the FGTR ship class, relay implemented as a
// plain fastcall C function. The vtable slot at WC3W.EXE+0x49d3b8 calls its
// entry with ECX = ship, matching GCC fastcall, so no hand-written stub
// bytes exist anywhere. No game code bytes are ever written. STANDING RULE.

#define GAME_BASE_EXPECTED   0x400000u
#define FGTR_UPDATE_VA       0x463890u
#define FGTR_VTABLE_VA       0x49d3b8u
#define FGTR_UPDATE_EXPECTED 0x00463890u
#define PLYR_UPDATE_VA       0x42a410u
#define PLYR_VTABLE_VA       0x49c188u
#define PLYR_UPDATE_EXPECTED 0x0042a410u
#define WEAP_UPDATE_VA       0x40c370u
#define WEAP_VTABLE_SLOT_VA  0x49b524u  // fighter WEAP (0x49b520) slot 1 -> FUN_0040c370
#define WEAP_UPDATE_EXPECTED 0x0040c370u
#define SHLD_CLASS_VA        0x49d128u  // fighter SHLD instance vtable (FUN_004582b0)
#define DMGA_FGTR_SLOT_VA    0x49d210u  // fighter DMGA (0x49d208) slot 2 -> FUN_0045bb30 (__thiscall, ret 8)
#define DMGA_FGTR_EXPECTED   0x0045bb30u
#define DMGA_CPTL_SLOT_VA    0x49be30u  // capital DMGA (0x49be28) slot 2 -> FUN_00424760 (__thiscall, ret 8)
#define DMGA_CPTL_EXPECTED   0x00424760u

// Unit 6: Out-of-mission Victory / gameflow / cutscene / savegame sync
// Static addresses verified via Ghidra (notes/unit6/decomp/*.c):
//   DAT_004a3580 = Script::act (0=exit,1=prologue,2=new,3=sim,4=deck,5=mission)
//   DAT_004a3584 = scene_id, DAT_004a3588 = mission_id, DAT_004ab84c = room_id
//   DAT_004a3594 = leave_scene flag, DAT_004a3590 = next_act
//   DAT_004aba08 = gameflow flags[256], DAT_004ab840 = gameflow regs[96]
//   DAT_004b0608 = room gump list (FUN_004689b0(list, id) finds gump by id)
//   DAT_004a7b4c / DAT_004b2000 = active modal gump stack (e.g. dialogue branch)
//   FUN_0045df30(ip, -1) = gameflow script VM (cdecl)
//   FUN_00408e70(slot)   = load savegame slot (cdecl)
//   FUN_004689b0(list,id)= find gump by ID in room (thiscall)
//   0x49ceb0 slot 0      = Screen gump per-frame tick (0x0044bf40, __thiscall, ret 0)
//   0x49b140 (0x401dc0), 0x49b6b0 (0x4133f0), 0x49b708/760/7b8/bb88 (0x413740)
//                        = hotspot gump click-release handlers (__thiscall, ret 4)
//   0x49b8c0 (0x414e70)  = dialogue branch choice handler (__thiscall, ret 4)
//   0x49b508 (0x40aa40)  = save/load dialog action handler (__thiscall, ret 4)
#define GF_SCRIPT_ACT_VA     0x4a3580u
#define GF_SCENE_ID_VA       0x4a3584u
#define GF_MISSION_ID_VA     0x4a3588u
#define GF_NEXT_ACT_VA       0x4a3590u
#define GF_LEAVE_SCENE_VA    0x4a3594u
#define GF_ROOM_ID_VA        0x4ab84cu
#define GF_CLICK_GUMP_VA     0x4ab858u
#define GF_FLAGS_VA          0x4aba08u
#define GF_REGS_VA           0x4ab840u
#define GF_IN_SIM_VA         0x4ab8d4u  // regs[37]: 1 while FUN_00435f20 simulator mission runs
#define GF_SIM_SCENE_VA      0x4ab8d8u  // regs[38]: scene to return to after simulator
#define GF_SIM_MISN_VA       0x4ab8dcu  // regs[39]: selected simulator mission ID
#define GF_ROOM_GUMPS_VA     0x4b0608u
#define GF_MODAL_TOP_VA      0x4a7b4cu
#define GF_MODAL_STACK_VA    0x4b2000u
#define GF_CAPTURED_GUMP_VA  0x4a7b58u  // DAT_004a7b58: gump holding keyboard/mouse capture (e.g. callsign input)
#define GF_CALLSIGN_DONE_VA  0x4ab900u  // DAT_004ab900: set to 1 when callsign input is confirmed

#define GF_EXEC_SCRIPT_VA    0x45df30u
#define GF_LOAD_SAVE_VA      0x408e70u
#define GF_FIND_GUMP_VA      0x4689b0u
#define GF_BRANCH_VT_VA      0x49b8a0u
#define GF_MOVIE_CTX_VA      0x4a24ccu  // non-NULL while FUN_0041c240 movie is active
#define GF_MOVIE_SKIP_VA     0x4a24c4u  // byte set to 1 to skip current movie segment (FUN_0041be00)
#define GF_MOVIE_BRANCH_VA   0x4aaa50u  // int: current movie branch list entry (-1 = end of sequence)
#define GF_DECK_ACTIVE_VA    0x4a3578u  // 1 while interactive Victory deck room loop is active
#define GF_KEYDOWN_ESC_VA    0x4b2181u  // DAT_004b2180[1]: live ESC key-down state (scancode 1)
#define GF_LOAD_OK_VA        0x49ff08u  // DAT_0049ff08: set to 1 by FUN_0040aa40 when load succeeds
#define GF_CD_SWITCH_VA      0x4042d0u  // FUN_004042d0(int cd): __cdecl mount CD1..4 TREs from WC3.CON
#define GF_CUR_CD_VA         0x49f72cu  // int DAT_0049f72c: currently mounted CD number (1..4)
#define GF_MUSIC_MGR_VA      0x4a9578u  // &DAT_004a9578: music manager object
#define GF_MUSIC_TRACK_VA    0x4b0968u  // BYTE DAT_004b0968: current music track index
#define GF_MUSIC_CD_VA       0x40f4a0u  // FUN_0040f4a0(void *this, unsigned int): __thiscall (ret 4)
#define GF_MUSIC_SYNC_VA     0x4397e0u  // FUN_004397e0(void): __cdecl (ret)
#define GF_PILOT_ALIVE_VA    0x4a9b38u  // int[12] DAT_004a9b38: squadron pilot alive/skill state (>0 = alive)
#define UI_MOUSE_RAW_VA      0x4a7e58u  // WORD buttons (b0=LMB,b1=RMB), WORD x, WORD y
#define KEY_STATE_TBL_VA     0x4b2180u  // BYTE[128] live keyboard scancode state table

// wc3en.dll (Enhancement Patch) data-pointer offsets for HD movie playback
// (FUN_10032d30 / FUN_10032a10 / FUN_100324a0) and input pump (FUN_1003da90).
// Hooked strictly via wc3en's own mutable data pointers (zero code bytes touched).
#define WC3EN_POLL_INPUT_OFF 0x54abcu   // function pointer DAT_10054abc -> 0x00482920
#define WC3EN_HD_MOVIE_OFF   0x549b8u   // pointer DAT_100549b8: active LibVlc_Movie object
#define WC3EN_ESC_DOWN_OFF   0x54541u   // byte DAT_10054541: wc3en keyboard ESC down state
#define WC3EN_POLL_INPUT_EXP 0x00482920u

#define SCREEN_TICK_SLOT_VA  0x49ceb0u
#define SCREEN_TICK_EXP      0x0044bf40u
#define SCREEN_SLAM_SLOT_VA  0x49ceb4u  // Screen vtable slot 1 -> FUN_00482740 (SLAM/present, __fastcall, ret 0)
#define SCREEN_SLAM_EXP      0x00482740u
#define UI_SCREEN_OBJ_VA     0x4a7dfcu  // DAT_004a7dfc: active Screen object ptr
#define UI_VIEWPORT_VA       0x49f97cu  // DAT_0049f97c: viewport struct { surf*, x0, y0, x1, y1 }
#define UI_FONT_TABLE_VA     0x4a95d0u  // DAT_004a95d0: font pointer array (+0x18 large, +0x10 medium, +0x08 small)
#define UI_SMALL_FONT_VA     0x4a1158u  // DAT_004a1158: small fallback font ptr
#define UI_BG_COLOR_VA       0x4a9f88u  // DAT_004a9f88: dark box fill palette index
#define UI_COLOR_TBL_VA      0x4aa650u  // DAT_004aa650: 256-byte bright UI text color table
#define UI_FILL_RECT_VA      0x421080u  // __cdecl void(x1, y1, x2, y2, color)
#define UI_DRAW_LINE_VA      0x470bd9u  // __cdecl uint(vp, x1, y1, x2, y2, mode, color)
#define UI_DRAW_TEXT_VA      0x4753e1u  // __cdecl void(vp, x, y, font, str, color_tbl)
#define UI_FONT_HEIGHT_VA    0x475218u  // __cdecl int(font)
#define UI_FONT_CHAR_W_VA    0x47522bu  // __cdecl int(font, ch)
#define GUMP_REL1_SLOT_VA    0x49b140u
#define GUMP_REL1_EXP        0x00401dc0u
#define GUMP_REL2_SLOT_VA    0x49b6b0u
#define GUMP_REL2_EXP        0x004133f0u
#define GUMP_REL3A_SLOT_VA   0x49b708u
#define GUMP_REL3B_SLOT_VA   0x49b760u
#define GUMP_REL3C_SLOT_VA   0x49b7b8u
#define GUMP_REL3D_SLOT_VA   0x49bb88u
#define GUMP_REL3_EXP        0x00413740u
#define GUMP_REL4_SLOT_VA    0x49b2c8u  // button/door gump (0x402a40) -> 0x402d70 (pressed +0x90)
#define GUMP_REL4_EXP        0x00402d70u
#define GUMP_REL5_SLOT_VA    0x49b660u  // hotspot gump (0x412e00) -> 0x412fe0 (pressed +0x90)
#define GUMP_REL5_EXP        0x00412fe0u
#define GUMP_REL6_SLOT_VA    0x49ba48u  // UI gump (0x415f90) -> 0x416120 (pressed +0x90)
#define GUMP_REL6_EXP        0x00416120u
#define GUMP_KEY5_SLOT_VA    0x49b668u  // hotspot gump keyboard shortcut handler -> 0x4130b0
#define GUMP_KEY5_EXP        0x004130b0u
#define DECK_KEY_SLOT_VA     0x49b620u  // Victory deck hotkey handler (Alt+X exit) -> 0x4121c0
#define DECK_KEY_EXP         0x004121c0u
#define BRANCH_MOVE_SLOT     0x49b8bcu  // dialogue branch mouse-move highlight -> 0x414e30
#define BRANCH_MOVE_EXP      0x00414e30u
#define BRANCH_CHOOSE_SLOT   0x49b8c0u
#define BRANCH_CHOOSE_EXP    0x00414e70u
#define BRANCH_KEY_SLOT      0x49b8c8u  // dialogue branch keyboard choice -> 0x414cf0
#define BRANCH_KEY_EXP       0x00414cf0u
#define MOVIE_FRAME_SLOT_VA  0x49bc58u  // FMV movie per-frame render & skip poll -> 0x41b740
#define MOVIE_FRAME_EXP      0x0041b740u
#define SAVELOAD_ACT_SLOT    0x49b508u
#define SAVELOAD_ACT_EXP     0x0040aa40u
#define SIMLIST_CLICK_SLOT   0x49bad0u  // simulator listbox (0x49bac0) slot 4 click -> 0x417920 (ret 4)
#define SIMLIST_CLICK_EXP    0x00417920u
#define SIMLIST_CMD_SLOT     0x49bb10u  // simulator listbox (0x49bac0) slot 20 cmd -> 0x417cc0 (ret 4)
#define SIMLIST_CMD_EXP      0x00417cc0u

// Fighter WEAP update FUN_0040c370 returns non-zero in AL (CONCAT31(...,1));
// propagate exact return value in every path (same vanish lesson).
typedef unsigned int (__attribute__((fastcall)) *weap_fn)(void *);
static weap_fn g_weap_original;

// Fighter/Capital DMGA apply_damage (FUN_0045bb30 / FUN_00424760):
// __thiscall taking (void *dmga, int section, int dmg), verified ret 8 (c2 08 00),
// returns 1 when ship is destroyed, 0 while alive.
typedef unsigned int (__attribute__((thiscall)) *dmga_fn)(void *, int, int);
static dmga_fn g_dmga_fgtr_orig;
static dmga_fn g_dmga_cptl_orig;

// The original update returns a status byte (ship+0x716) that the dispatcher
// reads at 0x46b28b. The relay MUST propagate it in every path: returning a
// stale AL told the dispatcher the wingman was removable, and the ship was
// silently despawned one frame after the first corrupted return. Found via
// legs 3-13 bisect, 2026-11-12.
typedef unsigned char (__attribute__((fastcall)) *fgtr_fn)(void *);
static fgtr_fn g_original;
static volatile LONG g_calls;

// PLYR player update FUN_0042a410 returns int (value 1 in both paths) and
// has no static callers, dispatch is vtable-only. The relay matches int and
// propagates the original return in every path: same encoded vanish lesson
// as the FGTR status byte above.
typedef int (__attribute__((fastcall)) *plyr_fn)(void *);
static plyr_fn g_plyr_original;
static volatile LONG g_plyr_calls;

// Wingman flight model. The dispatcher consumes ctl axes through the object
// at node+0x98, vtable slot 1, every frame at 60 Hz, and its own per-frame
// path also clears ctl, so a tick-rate apply (17 Hz) left the axes live only
// ~a quarter of the time: guns instant, turns mush, measured 2026-11-18
// night. This relay refreshes the wingman's ctl from the freshest input
// immediately before the flight model reads it, the same frame-locked
// guarantee the player's own mapper enjoys. The vtable is class-shared, so
// the relay filters for the wingman's own flight object and every other
// ship passes through untouched.
typedef void (__attribute__((fastcall)) *flight_fn)(void *);
static flight_fn g_flight_original;
static void *g_wingman_flight;
static volatile LONG g_flight_calls;

enum {
    LOG_MODE_OFF      = 0,
    LOG_MODE_CRITICAL = 1,
    LOG_MODE_DEBUG    = 2
};

static volatile int g_log_mode = LOG_MODE_DEBUG;
static char g_log_path[MAX_PATH];

static void log_line(const char *s);
static void log_crit(const char *s);
void wc3net_log(const char *s) { log_line(s); }
void wc3net_log_crit(const char *s) { log_crit(s); }

static CRITICAL_SECTION g_log_lock;
static volatile LONG g_log_lock_state; // 0=uninit, 1=initing, 2=ready

static void log_lock_ensure(void)
{
    if (g_log_lock_state == 2) return;
    if (InterlockedCompareExchange(&g_log_lock_state, 1, 0) == 0) {
        HMODULE self = NULL;
        char *slash = NULL;
        char *p;
        InitializeCriticalSection(&g_log_lock);
        g_log_path[0] = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&log_lock_ensure, &self) && self &&
            GetModuleFileNameA(self, g_log_path, sizeof(g_log_path))) {
            for (p = g_log_path; *p; p++) {
                if (*p == '\\' || *p == '/') slash = p;
            }
            if (slash) {
                slash++;
                lstrcpynA(slash, "wc3net.log", (int)(sizeof(g_log_path) - (slash - g_log_path)));
            } else {
                lstrcpynA(g_log_path, "wc3net.log", sizeof(g_log_path));
            }
        } else {
            lstrcpynA(g_log_path, "wc3net.log", sizeof(g_log_path));
        }
        g_log_lock_state = 2;
    } else {
        while (g_log_lock_state != 2) Sleep(0);
    }
}

static void log_line_raw(const char *s)
{
    const char *path;
    HANDLE h;
    if (g_log_mode == LOG_MODE_OFF) return;
    path = g_log_path[0] ? g_log_path : "wc3net.log";
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD n = 0;
    WriteFile(h, s, lstrlenA(s), &n, NULL);
    CloseHandle(h);
}

// Retain at most the last 2 sessions in wc3net.log (the most recent previous
// session plus the new session starting now). On the 3rd session, the oldest
// session is removed before writing the new session header.
static void log_rotate_sessions(void)
{
    static const char marker[] = "wc3net v0 attach ";
    const int mlen = (int)(sizeof(marker) - 1);
    const char *path;
    HANDLE h;
    DWORD sz, rd = 0;
    char *buf;

    if (g_log_mode == LOG_MODE_OFF) return;
    log_lock_ensure();
    EnterCriticalSection(&g_log_lock);
    path = g_log_path[0] ? g_log_path : "wc3net.log";
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_log_lock);
        return;
    }
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0) {
        CloseHandle(h);
        LeaveCriticalSection(&g_log_lock);
        return;
    }
    if (sz > (16u * 1024u * 1024u)) {
        CloseHandle(h);
        h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        LeaveCriticalSection(&g_log_lock);
        return;
    }
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, sz + 1);
    if (!buf) {
        CloseHandle(h);
        LeaveCriticalSection(&g_log_lock);
        return;
    }
    if (!ReadFile(h, buf, sz, &rd, NULL) || rd == 0) {
        CloseHandle(h);
        HeapFree(GetProcessHeap(), 0, buf);
        LeaveCriticalSection(&g_log_lock);
        return;
    }
    CloseHandle(h);
    {
        DWORD i, last_off = 0;
        int count = 0;
        for (i = 0; i + (DWORD)mlen <= rd; i++) {
            if (i == 0 || buf[i - 1] == '\n') {
                int k, match = 1;
                for (k = 0; k < mlen; k++) {
                    if (buf[i + (DWORD)k] != marker[k]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    last_off = i;
                    count++;
                }
            }
        }
        // If 2 or more previous sessions exist in wc3net.log, trim everything
        // before the most recent previous session (last_off) so that appending
        // the current session leaves exactly 2 sessions in wc3net.log.
        if (count >= 2 && last_off > 0) {
            HANDLE hw = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hw != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(hw, buf + last_off, rd - last_off, &wr, NULL);
                CloseHandle(hw);
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
    LeaveCriticalSection(&g_log_lock);
}

static void log_crit(const char *s)
{
    if (g_log_mode < LOG_MODE_CRITICAL) return;
    log_lock_ensure();
    EnterCriticalSection(&g_log_lock);
    log_line_raw(s);
    LeaveCriticalSection(&g_log_lock);
}

static void log_ts_crit(const char *tag, const char *s)
{
    char buf[224];
    if (g_log_mode < LOG_MODE_CRITICAL) return;
    wsprintfA(buf, "[%lu] %s %s", GetTickCount(), tag, s);
    log_crit(buf);
}

// timestamped line for timeline reconstruction, ms since boot
static void log_ts(const char *tag, const char *s)
{
    char buf[224];
    if (g_log_mode < LOG_MODE_DEBUG) return;
    wsprintfA(buf, "[%lu] %s %s", GetTickCount(), tag, s);
    log_line(buf);
}

static void log_line(const char *s)
{
    if (g_log_mode < LOG_MODE_DEBUG) return;
    log_lock_ensure();
    EnterCriticalSection(&g_log_lock);
    log_line_raw(s);
    LeaveCriticalSection(&g_log_lock);
}

static volatile int g_host_in_navmap;
static volatile int g_client_in_navmap;

static int crash_ptr_readable(const void *p, SIZE_T sz)
{
    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR start = (ULONG_PTR)p;
    ULONG_PTR end = start + sz;
    ULONG_PTR base, region_end;
    if (!p || sz == 0 || end < start) return 0;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    base = (ULONG_PTR)mbi.BaseAddress;
    region_end = base + mbi.RegionSize;
    if (start < base || end > region_end) return 0;
    return 1;
}

static void crash_module_base_name(void *addr, char *mod_out, int cap, ULONG_PTR *off_out)
{
    HMODULE mod = NULL;
    char path[MAX_PATH];
    const char *base_name = NULL;
    const char *p;
    mod_out[0] = 0;
    if (off_out) *off_out = 0;
    if (!addr) return;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &mod) || !mod)
        return;
    if (!GetModuleFileNameA(mod, path, sizeof(path))) return;
    base_name = path;
    for (p = path; *p; p++) {
        if (*p == '\\' || *p == '/') base_name = p + 1;
    }
    lstrcpynA(mod_out, base_name, cap);
    if (off_out) *off_out = (ULONG_PTR)addr - (ULONG_PTR)mod;
}

static void crash_fmt_addr(void *addr, char *out, int cap)
{
    char mname[64];
    ULONG_PTR off = 0;
    crash_module_base_name(addr, mname, sizeof(mname), &off);
    if (mname[0]) {
        char tmp[96];
        wsprintfA(tmp, "%s+0x%lx", mname, (unsigned long)off);
        lstrcpynA(out, tmp, cap);
    } else {
        lstrcpynA(out, "unknown", cap);
    }
}

static void crash_dump_exception(const char *src, EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er;
    CONTEXT *ctx;
    char buf[256];
    char aloc[96];
    DWORD code, flags, tid;
    void *addr;
    int cam_mode = -1;
    DWORD sim = 0, smode = 0;

    if (!ep || !ep->ExceptionRecord) return;
    er = ep->ExceptionRecord;
    ctx = ep->ContextRecord;
    code = er->ExceptionCode;
    flags = er->ExceptionFlags;
    addr = er->ExceptionAddress;
    tid = GetCurrentThreadId();
    crash_fmt_addr(addr, aloc, sizeof(aloc));

    wsprintfA(buf, "[%lu] CRASH(%s) code=0x%08lx flags=0x%08lx addr=%p (%s) tid=%lu\r\n",
              GetTickCount(), src ? src : "?", (unsigned long)code,
              (unsigned long)flags, addr, aloc, (unsigned long)tid);
    log_line_raw(buf);

    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        wsprintfA(buf, "  AV op=%lu target=%p\r\n",
                  (unsigned long)er->ExceptionInformation[0],
                  (void *)er->ExceptionInformation[1]);
        log_line_raw(buf);
    }

    if (ctx) {
        wsprintfA(buf, "  EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx ESI=%08lx EDI=%08lx\r\n",
                  (unsigned long)ctx->Eax, (unsigned long)ctx->Ebx,
                  (unsigned long)ctx->Ecx, (unsigned long)ctx->Edx,
                  (unsigned long)ctx->Esi, (unsigned long)ctx->Edi);
        log_line_raw(buf);
        wsprintfA(buf, "  EBP=%08lx ESP=%08lx EIP=%08lx EFLAGS=%08lx\r\n",
                  (unsigned long)ctx->Ebp, (unsigned long)ctx->Esp,
                  (unsigned long)ctx->Eip, (unsigned long)ctx->EFlags);
        log_line_raw(buf);
    }

    if (crash_ptr_readable((const void *)0x4b183cu, 2))
        cam_mode = (int)*(const short *)0x4b183cu;
    if (crash_ptr_readable((const void *)0x4a41acu, 4))
        sim = *(const DWORD *)0x4a41acu;
    if (crash_ptr_readable((const void *)0x4a41c8u, 4))
        smode = *(const DWORD *)0x4a41c8u;
    wsprintfA(buf, "  state host_nav=%d cli_nav=%d cam_mode=%d sim=%08lx smode=%lu\r\n",
              (int)g_host_in_navmap, (int)g_client_in_navmap, cam_mode,
              (unsigned long)sim, (unsigned long)smode);
    log_line_raw(buf);

    if (ctx) {
        DWORD esp = ctx->Esp;
        DWORD ebp = ctx->Ebp;
        int row, col, f;
        for (row = 0; row < 4; row++) {
            char vals[4][12];
            for (col = 0; col < 4; col++) {
                DWORD va = esp + (DWORD)((row * 4 + col) * 4);
                if (crash_ptr_readable((const void *)va, 4))
                    wsprintfA(vals[col], "%08lx", (unsigned long)*(const DWORD *)va);
                else
                    lstrcpynA(vals[col], "????????", sizeof(vals[col]));
            }
            wsprintfA(buf, "  ESP+%02x: %s %s %s %s\r\n",
                      (unsigned)(row * 16), vals[0], vals[1], vals[2], vals[3]);
            log_line_raw(buf);
        }
        for (f = 0; f < 10; f++) {
            DWORD next_ebp, ret;
            char rloc[96];
            if (!crash_ptr_readable((const void *)ebp, 8)) break;
            next_ebp = *(const DWORD *)ebp;
            ret = *(const DWORD *)(ebp + 4);
            crash_fmt_addr((void *)ret, rloc, sizeof(rloc));
            wsprintfA(buf, "  BT#%d ret=%08lx (%s) ebp=%08lx\r\n",
                      f, (unsigned long)ret, rloc, (unsigned long)ebp);
            log_line_raw(buf);
            if (next_ebp <= ebp || (next_ebp - ebp) > 0x100000UL) break;
            ebp = next_ebp;
        }
    }
}

static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_uef;

static LONG WINAPI wc3net_crash_filter(EXCEPTION_POINTERS *ep)
{
    static volatile LONG uef_dumped;
    if (InterlockedExchange(&uef_dumped, 1) == 0)
        crash_dump_exception("UEF", ep);
    if (g_prev_uef)
        return g_prev_uef(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI wc3net_veh(EXCEPTION_POINTERS *ep)
{
    static volatile LONG veh_dumps;
    DWORD code;
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED &&
        code != 0xC0000409UL) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        char mname[64];
        crash_module_base_name(ep->ExceptionRecord->ExceptionAddress, mname, sizeof(mname), NULL);
        if (lstrcmpiA(mname, "kernel32.dll") == 0 ||
            lstrcmpiA(mname, "kernelbase.dll") == 0 ||
            lstrcmpiA(mname, "ntdll.dll") == 0) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    if (InterlockedIncrement(&veh_dumps) <= 3)
        crash_dump_exception("VEH", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void read_name(void *ship, char *name)
{
    void *phys = NULL, *obj = NULL;
    name[0] = 0;
    if (ship && !IsBadReadPtr(ship, 8))
        phys = *(void **)((BYTE *)ship + 4);
    if (phys && !IsBadReadPtr(phys, 0x100))
        obj = *(void **)((BYTE *)phys + 0x24);
    if (obj && !IsBadReadPtr((BYTE *)obj + 0x2d, 8)) {
        CopyMemory(name, (BYTE *)obj + 0x2d, 8);
        name[8] = 0;
    }
}

// fields per notes/unit2/findings.md
static void log_ship_state(void *ship, LONG n, const char *tag)
{
    char buf[224];
    char name[12];
    void *phys = NULL, *ctl = NULL;
    DWORD voice = 0, owner = 0, order = 0, skill = 0, flags = 0;
    int ctl4 = -1, ctl5 = -1, ctl8 = -1;

    read_name(ship, name);
    if (ship && !IsBadReadPtr(ship, 8)) phys = *(void **)((BYTE *)ship + 4);
    if (phys && !IsBadReadPtr(phys, 0xa8)) {
        voice = *(DWORD *)((BYTE *)phys + 0x9c);
        owner = (BYTE) *(DWORD *)((BYTE *)phys + 0x70);
        flags = *(DWORD *)((BYTE *)phys + 0x50);
    }
    if (ship && !IsBadReadPtr((BYTE *)ship + 0x7c8, 4))
        ctl = *(void **)((BYTE *)ship + 0x7c8);
    if (ship && !IsBadReadPtr((BYTE *)ship + 0x6ec, 4))
        order = *(DWORD *)((BYTE *)ship + 0x6ec);
    if (ship && !IsBadReadPtr((BYTE *)ship + 0x631, 4))
        skill = *(DWORD *)((BYTE *)ship + 0x631);
    if (ctl && !IsBadReadPtr((BYTE *)ctl + 0x20, 1)) {
        ctl4 = ((BYTE *)ctl)[4];
        ctl5 = ((BYTE *)ctl)[5];
        ctl8 = *(int *)((BYTE *)ctl + 8);
    }
    wsprintfA(buf,
        "%s#%ld ship=%p name=%s voice=%p owner=%lu flags=%08lx order=%lu skill=%08lx ctl=%p c4=%d c5=%d c8=%d\r\n",
        tag, n, ship, name, (void *)voice, owner, flags, order, skill, ctl, ctl4, ctl5, ctl8);
    log_line(buf);
}

#define SEEN_MAX 24
static struct { void *ship; LONG count; } g_seen[SEEN_MAX];
#define SAMPLE_EVERY 60
#define LOG_TOTAL_MAX 400

static void *g_wingman;          // first FGTR with owner==1, captured at first sight
static volatile int g_wingman_dead;          // host: 1 once wingman dies/despawns while mission continues
static volatile int g_wingman_part_idx = -1; // host: participant index of captured wingman in simstate+0x3b
static volatile int g_client_spectating;     // client: 1 while st.wing_flags & WC3S_WING_DEAD
static DWORD g_client_eject_until;           // client: tick deadline to send WC3N_BTN_CLIENT_EJECT
static volatile int g_client_eject_anim;     // client: 1=eject (EJCT), 2=destroyed (DETH) before chase cam
static DWORD g_client_saved_p18;             // client: saved phys+0x18 during eject animation
static volatile int g_host_in_replay_prompt; // host: 1 while FUN_00430040 Replay/Continue screen is open
static int g_gf_pending_replay;              // client: 1 when host chose Replay/Continue/Quit
static int g_gf_pending_replay_choice;       // client: 0=Replay, 1=Continue, 2=Quit

#define SIMSTATE_VA   0x4a41acu
#define SIMSTATE_MODE_VA 0x4a41c8u   // 1 = active flight (FUN_00450690)
#define AUTOPILOT_REQ_VA    0x4a2da4u   // 'A' key autopilot/landing trigger (FUN_00450690)
#define AUTOPILOT_ACT_VA    0x4a3524u   // 1 while autopilot/landing cutscene runs (FUN_00434870/4342a0)
#define PLAYER_DEAD_VA      0x4a2d90u   // byte: player dead/ejecting trigger (FUN_0046ae10)
#define PLAYER_DEAD_PHASE2_VA 0x4a2d94u // byte: phase 2 external eject/death active
#define PLAYER_DEAD_DONE_VA 0x4a2d98u   // byte: player death/eject finished -> exits mission
#define PLAYER_EJECT_VA     0x4a2da0u   // byte: player ejected flag (Alt+E / Ctrl+E)
#define PLAYER_DEAD_SFX_VA  0x4a5810u   // byte: one-shot phase-2 trigger guard
#define PLAYER_DEAD_TIMER_VA 0x4ab3b4u  // int: eject/death countdown timer, init 0x500
#define FRAME_DT_VA         0x4ab280u   // int: per-frame dt
#define KEY_QUEUE_CNT_VA    0x4a7e28u   // DWORD: keyboard event queue count (0x4082dd)
#define KEY_QUEUE_BUF_VA    0x4b2208u   // DWORD[3]: {scancode, mods, state} (0x4082e7)
#define KEY_LALT_DOWN_VA    0x4b21b8u   // byte: Left Alt down state (0x4300a7)
#define CUR_KEY_SCANCODE_VA 0x4a9b80u   // DWORD: current frame scancode (0x31='N', 1=ESC, 0x1c=Enter, 0x19='P')
#define KEY_PENDING_CNT_VA  0x4a7e24u   // DWORD: pending key count copied into KEY_QUEUE_CNT_VA by FUN_00482920
#define KEY_PENDING_BUF_VA  0x4b2028u   // DWORD[3]: pending key event {scancode, mods, state}
#define CAM_MGR_VA          0x4b1830u   // &DAT_004b1830 camera manager
#define CAM_MODE_VA         0x4b183cu   // short: current camera mode; 4 = CHASE, 10 = NAVMAP
#define CAM_HUD_FLAG_VA     0x4a2c94u   // byte: HUD/view active flag
#define CAM_DIRTY_FLAG_VA   0x4a4c3cu   // byte: camera changed flag
#define CAM_FIND_VA         0x48cc10u   // __thiscall, ret 4
#define CAM_SWITCH_VA       0x48cdb0u   // __thiscall, ret 4
#define NAVMAP_ENTER_SLOT_VA 0x49d1ccu  // NAVMAP camera (0x49d1b8) slot +0x14 -> FUN_00430410 (__fastcall, ret 0)
#define NAVMAP_ENTER_EXP     0x00430410u
#define NAVMAP_TICK_SLOT_VA  0x49d1d0u  // NAVMAP camera (0x49d1b8) slot +0x18 -> FUN_0042eaf0 (__fastcall, ret 0)
#define NAVMAP_TICK_EXP      0x0042eaf0u
#define NAVMGR_OFF           0x95u      // simstate+0x95: +0 entries, +4 count, +8 cur_idx
#define NAVMGR_REFRESH_VA    0x468e20u  // FUN_00468e20(void *nav_mgr), __fastcall, ret 0
#define NAVUI_REFRESH_VA     0x446730u  // FUN_00446730(void *nav_ui), __fastcall, ret 0
#define COMM_QUEUE_CNT_VA    0x4a3340u  // int: active comm queue entries (0..8)
#define COMM_QUEUE_CUR_VA    0x4a3344u  // DWORD: pointer to active comm entry in DAT_004ab670
#define COMM_QUEUE_BUF_VA    0x4ab670u  // comm queue array, stride 0x24 (9 DWORDs)
#define COMM_MSG_READY_ATK   0x0bu      // "Just give the word, Sir." (FUN_00443d60:69)
#define PARTLIST_OFF  0x3bu
#define NODE_STRIDE   0x7e

typedef void *(__attribute__((thiscall)) *cam_find_fn)(void *mgr, const char *name);
typedef void (__attribute__((thiscall)) *cam_switch_fn)(void *mgr, void *cam);
typedef unsigned int (__attribute__((fastcall)) *navmap_enter_fn)(void *);
typedef void (__attribute__((fastcall)) *navmap_tick_fn)(void *);
typedef void (__attribute__((fastcall)) *nav_refresh_fn)(void *);
static navmap_enter_fn g_navmap_enter_orig;
static navmap_tick_fn  g_navmap_tick_orig;
static volatile int    g_host_in_navmap;
static volatile int    g_client_in_navmap;
static volatile int    g_client_navmap_exit_req;
static volatile LONG   g_host_nav_ticks;

// ---- World objects, Unit 5 increment (a) ----

#define PLAYER_GLOBAL_VA  0x4ab3c0u   // player PHYS ptr, dispatcher compares param_1 against it
#define PLYR_CLASS_VA     0x49c188u   // ship instance vtables (0x49c160/0x49d3a8 are CTRL factories)
#define FGTR_CLASS_VA     0x49d3b8u
#define CPTL_CLASS_VA     0x49c1a8u
#define WALK_NODE_LIMIT   512         // walk safety cap, span-guarded below
#define WORLD_ENT_MAX     24          // walk collection cap per frame
#define LOCAL_NODE_MAX    32          // client-side collection cap
#define POS_LIMIT   0x10000000        // per-component absolute position sanity
#define ORI_LIMIT   0x800             // 24.8 orientation entry sanity
#define VEL_LIMIT       0x10000000    // per-component velocity sanity
#define JUMP_LIMIT      0x02000000    // per-component one-frame teleport hold
#define CAND_NEAR_LIMIT 0x00100000    // candidate repeat tolerance for moving ships (~1M units)

// FNV-1a over the 8-char type name at (*(node+0x24))+0x2d, computed on
// both machines so stomp targets are verified without names on the wire.
// Zero means unreadable and never pairs.
static unsigned long name_hash(BYTE *node)
{
    unsigned long h = 0x811c9dc5UL;
    BYTE *obj = NULL;
    int i;
    if (!IsBadReadPtr(node + 0x24, 4)) obj = *(BYTE **)(node + 0x24);
    if (!obj || IsBadReadPtr(obj + 0x2d, 8)) return 0;
    for (i = 0; i < 8; i++) {
        h ^= obj[0x2d + i];
        h *= 0x01000193UL;
    }
    return h;
}

typedef struct {
    BYTE *node;              // live phys object (*(entry+0x76))
    BYTE *ship;              // owning ship controller (*(phys+0x94))
    unsigned char owner;     // phys+0x70
    unsigned char dead;      // phys+0x7d
    unsigned char fire;      // b1 = ctl+4 bit0 (guns), b2 = ctl+4 bit2 (missile/alt)
    unsigned char part_idx;  // index in participant table (0..254, 0xff = none)
    unsigned long hash;      // name_hash(phys), 0 = unreadable
    int  used;               // client-side pairing state, 0 = free
} world_ent;

// Read ship control block pointer per FUN_0046c1d0:
//   PLYR (0x49c188) -> ship+0x650
//   FGTR (0x49d3b8) -> ship+0x7c8
//   CPTL (0x49c1a8) -> ship+0x6b8
static BYTE *ship_ctl_ptr(BYTE *ship)
{
    DWORD vt, off;
    if (!ship || IsBadReadPtr(ship, 4)) return NULL;
    vt = *(DWORD *)ship;
    if (vt == PLYR_CLASS_VA) off = 0x650;
    else if (vt == FGTR_CLASS_VA) off = 0x7c8;
    else if (vt == CPTL_CLASS_VA) off = 0x6b8;
    else return NULL;
    if (IsBadReadPtr(ship + off, 4)) return NULL;
    return *(BYTE **)(ship + off);
}

#define FIRE_SLOTS 32

// Host-side per-fighter fire latch: written by weap_relay on the game thread
// right as FUN_0040c370 consumes ctl+4, read by state_pump at 60 Hz so short
// trigger pulses and 1-frame missile presses are never missed.
static struct {
    BYTE *phys;
    DWORD guns_until;
    DWORD msl_until;
} g_host_fire[FIRE_SLOTS];

// Client-side per-fighter mirrored fire table: written by apply_world inside
// plyr_relay, read by weap_relay right BEFORE FUN_0040c370 (AFTER
// FUN_00463890's FUN_00410440 has wiped ctl+4). Same game thread, no lock.
static struct {
    BYTE *phys;
    unsigned char fire;
    DWORD until;
} g_client_fire[FIRE_SLOTS];

static void host_fire_latch(BYTE *phys, unsigned char c4)
{
    DWORD now = GetTickCount();
    int i, slot = -1;
    if (!phys || !(c4 & (0x01 | 0x04))) return;
    for (i = 0; i < FIRE_SLOTS; i++) {
        if (g_host_fire[i].phys == phys) { slot = i; break; }
        if (slot == -1 && g_host_fire[i].phys == NULL) slot = i;
    }
    if (slot == -1) return;
    if (c4 & 0x01) g_host_fire[slot].guns_until = now + 65;
    if (c4 & 0x04) g_host_fire[slot].msl_until  = now + 120;
    g_host_fire[slot].phys = phys;
}

static unsigned char host_fire_query(BYTE *phys)
{
    DWORD now = GetTickCount();
    int i;
    unsigned char fl = 0;
    if (!phys) return 0;
    for (i = 0; i < FIRE_SLOTS; i++) {
        if (g_host_fire[i].phys == phys) {
            if ((int)(g_host_fire[i].guns_until - now) > 0) fl |= 0x02;
            if ((int)(g_host_fire[i].msl_until  - now) > 0) fl |= 0x04;
            break;
        }
    }
    return fl;
}

static void client_fire_set(BYTE *phys, unsigned char fire)
{
    DWORD now = GetTickCount();
    int i, slot = -1;
    if (!phys) return;
    for (i = 0; i < FIRE_SLOTS; i++) {
        if (g_client_fire[i].phys == phys) { slot = i; break; }
        if (slot == -1 && g_client_fire[i].phys == NULL) slot = i;
    }
    if (slot == -1) return;
    g_client_fire[slot].phys = phys;
    g_client_fire[slot].fire = fire;
    g_client_fire[slot].until = now + 250;
}

static unsigned char client_fire_query(BYTE *phys)
{
    DWORD now = GetTickCount();
    int i;
    if (!phys) return 0;
    for (i = 0; i < FIRE_SLOTS; i++) {
        if (g_client_fire[i].phys == phys) {
            if ((int)(g_client_fire[i].until - now) > 0)
                return g_client_fire[i].fire;
            return 0;
        }
    }
    return 0;
}

static unsigned char read_fire_flags(BYTE *phys, BYTE *ship)
{
    BYTE *ctl = ship_ctl_ptr(ship);
    unsigned char fl = host_fire_query(phys);
    if (ctl && !IsBadReadPtr(ctl + 4, 1)) {
        unsigned char c4 = ctl[4];
        if (c4 & 0x01) fl |= 0x02;   // guns
        if (c4 & 0x04) fl |= 0x04;   // alt-fire / missile
    }
    if (ctl && !IsBadReadPtr(ctl + 5, 1) && (ctl[5] & 0x10))
        fl |= 0x08;                  // afterburner
    return fl;
}

static unsigned char scale255(int cur, int mx)
{
    if (mx <= 0 || cur <= 0) return 0;
    if (cur >= mx) return 255;
    return (unsigned char)((cur * 255 + (mx / 2)) / mx);
}

// Host: read wingman shields (4 faces), armor (4 faces), hull, and hit flag
// from phys+0xa0 (SHLD, vtable 0x49d128) and phys+0xa4 (DMGA).
static void read_wing_health(BYTE *phys, wc3s_wing_health *out)
{
    static int prev_total;
    static int have_prev;
    static DWORD hit_until;
    DWORD now = GetTickCount();
    BYTE *shld = NULL, *shld_def = NULL, *dmga = NULL;
    int total = 0, ok = 0, f;

    ZeroMemory(out, sizeof(*out));
    if (!phys || IsBadReadPtr(phys + 0xa0, 8)) return;
    shld = *(BYTE **)(phys + 0xa0);
    dmga = *(BYTE **)(phys + 0xa4);
    if (shld && !IsBadReadPtr(shld, 0x50) && *(DWORD *)shld == SHLD_CLASS_VA) {
        shld_def = *(BYTE **)(shld + 8);
        if (shld_def && !IsBadReadPtr(shld_def + 8, 0x20)) {
            for (f = 0; f < 4; f++) {
                int max_s = *(int *)(shld_def + 8 + 4 * f);
                int cur_s = *(int *)(shld + 0x20 + 4 * f);
                int max_a = *(int *)(shld_def + 0x18 + 4 * f);
                int cur_a = *(int *)(shld + 0x40 + 4 * f);
                out->shld[f]  = scale255(cur_s, max_s);
                out->armor[f] = scale255(cur_a, max_a);
                total += (cur_s > 0 ? cur_s : 0) + (cur_a > 0 ? cur_a : 0);
            }
            ok = 1;
        }
    }
    if (dmga && !IsBadReadPtr(dmga + 0x3c, 8)) {
        // dmga+0x40 is accumulated system damage (0 = undamaged, max at +0x3c)
        int max_d = *(int *)(dmga + 0x3c);
        int cur_d = *(int *)(dmga + 0x40);
        out->hull = scale255(cur_d, max_d);
        total -= (cur_d > 0 ? cur_d : 0);
        ok = 1;
    }
    if (!ok) return;
    if ((have_prev && total < prev_total) ||
        (!IsBadReadPtr(phys + 0x7e, 1) && phys[0x7e] != 0)) {
        hit_until = now + 100;
    }
    prev_total = total;
    have_prev = 1;
    out->flags = WC3S_WING_HEALTH_OK;
    if ((int)(hit_until - now) > 0)
        out->flags |= WC3S_WING_HIT;
}

// Client: stomp host wingman shields, armor, and hull onto client's own PLYR
// so host friendly fire and enemy hits show up accurately on the cockpit HUD.
static void apply_wing_health(BYTE *phys, const wc3s_packet *st)
{
    static int prev_hit;
    BYTE *shld = NULL, *shld_def = NULL, *dmga = NULL;
    int hit, f;

    if (!phys || !(st->wing_flags & WC3S_WING_HEALTH_OK)) return;
    if (IsBadReadPtr(phys + 0xa0, 8)) return;
    shld = *(BYTE **)(phys + 0xa0);
    dmga = *(BYTE **)(phys + 0xa4);
    if (shld && !IsBadReadPtr(shld, 0x50) && *(DWORD *)shld == SHLD_CLASS_VA &&
        !IsBadWritePtr(shld + 0x20, 0x30)) {
        shld_def = *(BYTE **)(shld + 8);
        if (shld_def && !IsBadReadPtr(shld_def + 8, 0x20)) {
            for (f = 0; f < 4; f++) {
                int max_s = *(int *)(shld_def + 8 + 4 * f);
                int cur_s = (max_s > 0) ? ((max_s * (int)st->wing_shld[f] + 127) / 255) : 0;
                int max_a = *(int *)(shld_def + 0x18 + 4 * f);
                int cur_a = (max_a > 0) ? ((max_a * (int)st->wing_armor[f] + 127) / 255) : 0;
                if (cur_s > max_s) cur_s = max_s;
                if (cur_a > max_a) cur_a = max_a;
                *(int *)(shld + 0x20 + 4 * f) = cur_s;
                *(int *)(shld + 0x30 + 4 * f) = max_s - cur_s;
                *(int *)(shld + 0x40 + 4 * f) = cur_a;
            }
        }
    }
    if (dmga && !IsBadReadPtr(dmga + 0x3c, 8) && !IsBadWritePtr(dmga + 0x40, 4)) {
        int max_h = *(int *)(dmga + 0x3c);
        int cur_h = (max_h > 0) ? ((max_h * (int)st->wing_hull + 127) / 255) : 0;
        if (cur_h > max_h) cur_h = max_h;
        *(int *)(dmga + 0x40) = cur_h;
    }
    hit = ((st->wing_flags & WC3S_WING_HIT) != 0) &&
          ((st->wing_flags & WC3S_WING_DEAD) == 0);
    if (hit && !prev_hit && !IsBadWritePtr(phys + 0x7e, 1))
        phys[0x7e] = 1;
    prev_hit = hit;
}

// Walk the mission participant table: simstate DAT_004a41ac, header at
// simstate+0x3b (count +0x3b, array +0x3f, stride 0x7e, per FUN_0044ff40,
// FUN_0044f180, FUN_00452bc0). Each 0x7e-byte entry is a participant
// descriptor whose +0x76 dword points to the spawned live PHYS object
// (NULL when unspawned/despawned). Each live ship PHYS points to its
// ship controller at phys+0x94 (FUN_0046ae10), whose vtable is PLYR
// 0x49c188, FGTR 0x49d3b8, or CPTL 0x49c1a8, and whose dead byte is
// phys+0x7d (FUN_00452bc0). Filtering by ship controller vtable is
// class-exact and phase-immune for the async 60 Hz state pump. stats,
// when given, reports the once-per-second walk diag: [0] entries in the
// table (negative values mark bail stages), [1] with a live phys ptr,
// [2] passing the ship-class check, [3] alive.
static int collect_world(world_ent *out, int max, long *stats)
{
    DWORD sim = 0, count, array;
    int n = 0, i;

    if (stats) { stats[0] = -1; stats[1] = stats[2] = stats[3] = 0; }
    if (IsBadReadPtr((void *)SIMSTATE_VA, 4)) { if (stats) stats[0] = -2; return 0; }
    sim = *(DWORD *)SIMSTATE_VA;
    if (!sim || IsBadReadPtr((void *)(sim + PARTLIST_OFF), 8)) { if (stats) stats[0] = -3; return 0; }
    count = *(DWORD *)(sim + PARTLIST_OFF);
    array = *(DWORD *)(sim + PARTLIST_OFF + 4);
    if (stats) stats[0] = count > WALK_NODE_LIMIT ? -4 : (long)count;
    if (!array || count > WALK_NODE_LIMIT) return 0;
    if (count && IsBadReadPtr((void *)array, count * NODE_STRIDE)) { if (stats) stats[0] = -5; return 0; }
    for (i = 0; i < (int)count; i++) {
        BYTE *entry = (BYTE *)(array + (DWORD)i * NODE_STRIDE);
        BYTE *phys = *(BYTE **)(entry + 0x76);
        BYTE *ship;
        DWORD vt;
        if (!phys) continue;
        if (stats) stats[1]++;
        if (IsBadReadPtr(phys + 0x24, 0x74)) continue;
        ship = *(BYTE **)(phys + 0x94);
        if (!ship || IsBadReadPtr(ship, 4)) continue;
        vt = *(DWORD *)ship;
        if (vt != PLYR_CLASS_VA && vt != FGTR_CLASS_VA && vt != CPTL_CLASS_VA)
            continue;
        if (stats) stats[2]++;
        if (!phys[0x7d] && stats) stats[3]++;
        if (n < max) {
            out[n].node = phys;
            out[n].ship = ship;
            out[n].owner = phys[0x70];
            out[n].dead = phys[0x7d];
            out[n].fire = phys[0x7d] ? 0 : read_fire_flags(phys, ship);
            out[n].part_idx = (i < 255) ? (unsigned char)i : 0xff;
            out[n].hash = name_hash(phys);
            out[n].used = 0;
            n++;
        }
    }
    return n;
}

// Fill one wire object from a walk entry whose node span is verified.
static void fill_object_from_node(wc3s_object *o, const world_ent *e)
{
    o->namehash = e->hash;
    CopyMemory(o->pos, e->node + 0x28, sizeof(o->pos));
    CopyMemory(o->orient, e->node + 0x34, sizeof(o->orient));
    o->owner = e->owner;
    o->flags = (unsigned char)((e->dead ? 1 : 0) | e->fire);
    o->part_idx = e->part_idx;
    o->reserved1 = 0;
}

// Fill one wire object from a phys pointer chased off a ship object (the
// synthesis path, no walk span to lean on). One guard spans +0x24..+0x87:
// name object ptr, transform, owner, dead.
static int fill_object_from_phys(wc3s_object *o, BYTE *phys)
{
    unsigned char fire = 0;
    if (!phys || IsBadReadPtr(phys + 0x24, 0x74)) return 0;
    ZeroMemory(o, sizeof(*o));
    o->namehash = name_hash(phys);
    CopyMemory(o->pos, phys + 0x28, sizeof(o->pos));
    CopyMemory(o->orient, phys + 0x34, sizeof(o->orient));
    o->owner = phys[0x70];
    if (!phys[0x7d]) {
        BYTE *ship = *(BYTE **)(phys + 0x94);
        fire = read_fire_flags(phys, ship);
    }
    o->flags = (unsigned char)((phys[0x7d] ? 1 : 0) | fire);
    o->part_idx = 0xff;
    return 1;
}

// Host: build a 64-bit mask of every mission participant entry that is dead
// or destroyed on the host (entry+0x50 bit 1 set by FUN_0044e3b0, OR live
// phys has phys[0x7d] != 0). Streamed in every WC3S header so the client
// immediately kills any dead host enemy even across autopilot or packet loss.
static unsigned long long collect_part_dead_mask(void)
{
    DWORD sim = 0, count, array;
    unsigned long long mask = 0;
    int i, lim;

    if (IsBadReadPtr((void *)SIMSTATE_VA, 4)) return 0;
    sim = *(DWORD *)SIMSTATE_VA;
    if (!sim || IsBadReadPtr((void *)(sim + PARTLIST_OFF), 8)) return 0;
    count = *(DWORD *)(sim + PARTLIST_OFF);
    array = *(DWORD *)(sim + PARTLIST_OFF + 4);
    if (!array || count > WALK_NODE_LIMIT) return 0;
    if (count && IsBadReadPtr((void *)array, count * NODE_STRIDE)) return 0;
    lim = (count < 64) ? (int)count : 64;
    for (i = 0; i < lim; i++) {
        BYTE *entry = (BYTE *)(array + (DWORD)i * NODE_STRIDE);
        DWORD pflags = *(DWORD *)(entry + 0x50);
        BYTE *phys = *(BYTE **)(entry + 0x76);
        if ((pflags & 2u) != 0 ||
            (phys && !IsBadReadPtr(phys + 0x7d, 1) && phys[0x7d] != 0)) {
            mask |= (1ULL << i);
        }
    }
    return mask;
}

// Host: assemble the objects[] block. Slot 0 is always the host PLYR
// (Blair) synthesized from the player global, which holds the player's
// PHYS pointer directly (FUN_0046ae10 compares its own phys param
// against DAT_004ab3c0), slot 1 always the captured wingman; the idx
// bytes carry validity so the client never guesses. Both are skipped in
// the walk fill by node and by ship pointer (recording their exact
// part_idx onto slots 0 and 1), everything else goes out in registry
// order. The once-per-second diag decomposes the walk: cnt is the raw
// array count (-2 simstate unreadable, -3 header unreadable, -4 over
// the cap, -5 span unreadable), bp backptr hits, vt class-check passes,
// live after the dead check.
static int build_world(wc3s_packet *p)
{
    static DWORD last_diag;
    world_ent ents[WORLD_ENT_MAX];
    long st[4] = { 0, 0, 0, 0 };
    DWORD blair = 0, wing_node = 0;
    int i, n = 2, wn;

    p->idx_blair = 0xff;
    p->idx_wing = 0xff;
    p->wing_voice = 0;
    p->part_dead_mask = collect_part_dead_mask();

    if (!IsBadReadPtr((void *)PLAYER_GLOBAL_VA, 4))
        blair = *(DWORD *)PLAYER_GLOBAL_VA;
    if (blair && fill_object_from_phys(&p->objects[0], (BYTE *)blair))
        p->idx_blair = 0;
    if (g_wingman && !g_wingman_dead && !IsBadReadPtr((BYTE *)g_wingman + 4, 4)) {
        BYTE *phys = *(BYTE **)((BYTE *)g_wingman + 4);
        wing_node = (DWORD)(size_t)phys;
        if (fill_object_from_phys(&p->objects[1], phys)) {
            p->idx_wing = 1;
            if (!IsBadReadPtr(phys + 0x9c, 4))
                p->wing_voice = *(unsigned long *)(phys + 0x9c);
        }
    }
    wn = collect_world(ents, WORLD_ENT_MAX, st);
    for (i = 0; i < wn && n < WC3S_OBJ_MAX; i++) {
        if (ents[i].node && (DWORD)(size_t)ents[i].node == blair) {
            if (p->idx_blair == 0) p->objects[0].part_idx = ents[i].part_idx;
            continue;
        }
        if (!g_wingman_dead &&
            ((ents[i].node && (DWORD)(size_t)ents[i].node == wing_node) ||
             (ents[i].ship && ents[i].ship == (BYTE *)g_wingman))) {
            if (p->idx_wing == 1) p->objects[1].part_idx = ents[i].part_idx;
            continue;
        }
        fill_object_from_node(&p->objects[n], &ents[i]);
        n++;
    }
    p->obj_count = (unsigned char)n;

    if (GetTickCount() - last_diag >= 1000) {
        char buf[160];
        last_diag = GetTickCount();
        wsprintfA(buf,
            "cnt=%ld bp=%ld vt=%ld live=%ld sent=%d blair=%d wing=%d(%u) dm=%08lx h2=%08lx/%d\r\n",
            st[0], st[1], st[2], st[3], n, p->idx_blair != 0xff,
            p->idx_wing != 0xff, (unsigned)p->objects[1].part_idx,
            (unsigned long)(p->part_dead_mask & 0xffffffffUL),
            p->objects[2].namehash, (int)p->objects[2].owner);
        log_ts("walk", buf);
    }
    return n;
}

// Watchdog: sample the wingman at 10 Hz from our own thread, decoupled from
// the relay call rate. Near the flight deck the game updates the waiting
// wingman rarely, so relay-sampled logging goes blind exactly when the ship
// is removed (legs 3 and 4). This thread sees the transition regardless.
static DWORD WINAPI wing_watchdog(LPVOID unused)
{
    (void)unused;
    for (;;) {
        static DWORD last_flags, last_order, last_c4, last_c5, last_c8;
        static int have_last;
        void *ship = g_wingman;
        if (!g_wingman_dead && ship && !IsBadReadPtr((BYTE *)ship + 0x7d0, 1)) {
            void *phys = *(void **)((BYTE *)ship + 4);
            void *ctl = *(void **)((BYTE *)ship + 0x7c8);
            DWORD flags = 0, order = 0, c4 = 0, c5 = 0, c8 = 0;
            char buf[160];
            if (phys && !IsBadReadPtr((BYTE *)phys + 0x54, 1))
                flags = *(DWORD *)((BYTE *)phys + 0x50);
            if (!IsBadReadPtr((BYTE *)ship + 0x6f0, 1))
                order = *(DWORD *)((BYTE *)ship + 0x6ec);
            if (ctl && !IsBadReadPtr((BYTE *)ctl + 9, 1)) {
                c4 = ((BYTE *)ctl)[4];
                c5 = ((BYTE *)ctl)[5];
                c8 = *(DWORD *)((BYTE *)ctl + 8) & 0xff;
            }
            if (!have_last || flags != last_flags || order != last_order ||
                c4 != last_c4 || c5 != last_c5 || c8 != last_c8) {
                extern void wc3net_rx_stats(LONG *);
                LONG rx = 0;
                wc3net_rx_stats(&rx);
                wsprintfA(buf, "watch ship=%p flags=%08lx order=%lu c4=%lu c5=%lu c8=%lu rx=%ld\r\n",
                          ship, flags, order, c4, c5, c8, rx);
                log_ts("watch", buf);
                last_flags = flags; last_order = order;
                last_c4 = c4; last_c5 = c5; last_c8 = c8;
                have_last = 1;
            }
        }
        Sleep(100);
    }
    return 0;
}

static LONG g_possess_frames;    // remaining scripted frames, 0 = idle
static int g_possess_done;       // one-shot latch, never re-arm after first run
static int g_mode_remote;        // wc3net.ini mode=remote, network input replaces script
static int g_mode_client;        // wc3net.ini mode=client, ship local player input to the host
static int g_mode_observe;       // wc3net.ini mode=observe, no net, no writes ever
static int g_mode_observe_net;   // wc3net.ini mode=observenet, net up, still no writes
static int g_mode_script;        // wc3net.ini mode=script, local scripted wingman test
static int g_mode_off;           // wc3net.ini mode=off/singleplayer/vanilla (or fallback), pure pass-through
static int g_net_disabled;       // wc3net.ini net=off, listener and sender never start
static int g_auth;               // wc3net.ini auth=, remote axis multiplier, 1..4
static int g_ai_off;             // wingman AI disengaged under fresh client input
static volatile DWORD g_host_ap_grace_until; // post-autopilot AI/fire grace window on host
static volatile LONG g_threads_started;  // one-shot guard so mission restart doesn't double-spawn threads
static volatile LONG g_state_pump_started; // one-shot guard for state_pump thread
static char g_host[64];          // wc3net.ini host, client target ip
static int g_port;               // wc3net.ini port, shared by listener and client sender

static int host_real_autopilot(void)
{
    if (IsBadReadPtr((void *)AUTOPILOT_ACT_VA, 1) || *(BYTE *)AUTOPILOT_ACT_VA == 0)
        return 0;
    if (!IsBadReadPtr((void *)PLAYER_DEAD_VA, 1) && *(BYTE *)PLAYER_DEAD_VA != 0)
        return 0;
    return 1;
}

static int host_in_navmap(void)
{
    if (g_host_in_navmap != 0)
        return 1;
    if (!IsBadReadPtr((void *)CAM_MODE_VA, 2) && *(short *)CAM_MODE_VA == 10)
        return 1;
    return 0;
}

static int host_in_ap_or_grace(int *out_in_ap)
{
    DWORD now = GetTickCount();
    int in_ap = host_real_autopilot();
    int in_nav = host_in_navmap();
    if (in_ap || in_nav)
        g_host_ap_grace_until = now + 3000;
    if (out_in_ap) *out_in_ap = in_ap;
    return in_ap || in_nav || (g_host_ap_grace_until != 0 &&
                                (int)(g_host_ap_grace_until - now) > 0);
}

static void client_ensure_chase_cam(void);
static void client_reset_jump(void);

static unsigned char read_nav_idx(void)
{
    DWORD sim = 0;
    int cnt, idx;
    if (IsBadReadPtr((void *)SIMSTATE_VA, 4)) return 0xff;
    sim = *(DWORD *)SIMSTATE_VA;
    if (!sim || IsBadReadPtr((void *)(sim + NAVMGR_OFF), 12)) return 0xff;
    if (*(DWORD *)(sim + NAVMGR_OFF) == 0) return 0xff;
    cnt = *(int *)(sim + NAVMGR_OFF + 4);
    idx = *(int *)(sim + NAVMGR_OFF + 8);
    if (cnt > 0 && idx >= 0 && idx < cnt && idx < 255)
        return (unsigned char)idx;
    return 0xff;
}

static void client_sync_nav_idx(unsigned char target_idx, void *nav_ui)
{
    DWORD sim = 0;
    int cnt, cur;
    char buf[80];
    if (target_idx == 0xff) return;
    if (IsBadReadPtr((void *)SIMSTATE_VA, 4)) return;
    sim = *(DWORD *)SIMSTATE_VA;
    if (!sim || IsBadWritePtr((void *)(sim + NAVMGR_OFF), 0x1c)) return;
    if (*(DWORD *)(sim + NAVMGR_OFF) == 0) return;
    cnt = *(int *)(sim + NAVMGR_OFF + 4);
    cur = *(int *)(sim + NAVMGR_OFF + 8);
    if (cnt <= 0 || (int)target_idx >= cnt || cur == (int)target_idx) return;
    *(int *)(sim + NAVMGR_OFF + 8) = (int)target_idx;
    ((nav_refresh_fn)NAVMGR_REFRESH_VA)((void *)(sim + NAVMGR_OFF));
    if (nav_ui != NULL)
        ((nav_refresh_fn)NAVUI_REFRESH_VA)(nav_ui);
    wsprintfA(buf, "client navpoint synced: %d -> %u\r\n", cur, (unsigned)target_idx);
    log_ts("nav", buf);
}

static void client_offer_navmap_keepalive(int fallback_thr)
{
    wc3net_ctl_snap neutral;
    ZeroMemory(&neutral, sizeof(neutral));
    neutral.throttle = fallback_thr;
    if (!IsBadReadPtr((void *)PLAYER_GLOBAL_VA, 4)) {
        BYTE *phys = *(BYTE **)PLAYER_GLOBAL_VA;
        if (phys && !IsBadReadPtr(phys + 0x94, 4)) {
            BYTE *ctl = ship_ctl_ptr(*(BYTE **)(phys + 0x94));
            if (ctl && !IsBadReadPtr(ctl + 8, 4))
                neutral.throttle = *(int *)(ctl + 8);
        }
    }
    wc3net_client_offer(&neutral);
}

static void client_inject_navmap_exit(int pending_buf)
{
    DWORD buf_va = pending_buf ? KEY_PENDING_BUF_VA : KEY_QUEUE_BUF_VA;
    DWORD cnt_va = pending_buf ? KEY_PENDING_CNT_VA : KEY_QUEUE_CNT_VA;
    if (!IsBadWritePtr((void *)buf_va, 12) &&
        !IsBadWritePtr((void *)cnt_va, 4)) {
        ((DWORD *)buf_va)[0] = 1;
        ((DWORD *)buf_va)[1] = 0;
        ((DWORD *)buf_va)[2] = 1;
        *(DWORD *)cnt_va = 1;
    }
}

// Nav Map modal entry/tick relays (vtable 0x49d1b8 slots +0x14 / +0x18).
// FUN_00430410 enters FUN_00445060 -> FUN_0044ae40 (which scales every
// ship's phys+0x28..0x30 by 1/100 during the modal loop and restores them on
// exit). Track host/client Nav Map state, sync navpoint selection, and send
// client keepalives while inside the blocking loop.
static unsigned int __attribute__((fastcall)) navmap_enter_relay(void *nav_cam)
{
    unsigned int rc;
    char nbuf[64];
    if (!g_mode_client) {
        g_host_in_navmap = 1;
        g_host_nav_ticks = 0;
        g_host_ap_grace_until = GetTickCount() + 3000;
        wsprintfA(nbuf, "host opened navmap cam=%p\r\n", nav_cam);
        log_ts_crit("nav", nbuf);
        rc = g_navmap_enter_orig(nav_cam);
        g_host_ap_grace_until = GetTickCount() + 3000;
        g_host_in_navmap = 0;
        wsprintfA(nbuf, "host closed navmap rc=0x%x\r\n", rc);
        log_ts_crit("nav", nbuf);
        return rc;
    }
    g_client_in_navmap = 1;
    g_client_navmap_exit_req = 0;
    log_ts_crit("nav", "client opened navmap\r\n");
    rc = g_navmap_enter_orig(nav_cam);
    g_client_in_navmap = 0;
    g_client_navmap_exit_req = 0;
    client_reset_jump();
    log_ts_crit("nav", "client closed navmap\r\n");
    return rc;
}

static void __attribute__((fastcall)) navmap_tick_relay(void *nav_cam)
{
    if (!g_mode_client) {
        if (g_host_in_navmap) {
            LONG t = InterlockedIncrement(&g_host_nav_ticks);
            if (t == 1) {
                char nbuf[64];
                wsprintfA(nbuf, "host nav_tick#1 cam=%p\r\n", nav_cam);
                log_ts("nav", nbuf);
            }
        }
    } else if (g_client_in_navmap) {
        wc3s_packet st;
        int have_st = wc3net_state_fetch(&st);
        void *nav_ui = (nav_cam && !IsBadReadPtr((BYTE *)nav_cam + 0xec, 4))
                       ? *(void **)((BYTE *)nav_cam + 0xec) : NULL;
        if (have_st)
            client_sync_nav_idx(st.nav_idx, nav_ui);
        client_offer_navmap_keepalive(have_st ? st.throttle : 0);
        if (!have_st || !(st.hdr_flags & WC3S_FLAG_NAVMAP) ||
            (st.hdr_flags & (WC3S_FLAG_AUTOPILOT | WC3S_FLAG_MISSION_END))) {
            g_client_navmap_exit_req = 1;
            client_inject_navmap_exit(1);
        }
    }
    g_navmap_tick_orig(nav_cam);
}

// Fighter weapons relay (vtable 0x49b520 slot 1 -> FUN_0040c370).
// Runs in FUN_0046ae10 right AFTER FGTR's FUN_00463890 (which clears ctl+4
// via FUN_00410440) and right BEFORE FUN_0040c370 consumes ctl+4.
//   Host: latches ctl+4 guns/missile bits per fighter phys so state_pump
//         never misses a short trigger or 1-frame missile press.
//   Client: leaves the client's own PLYR untouched, clears local AI fire on
//         other fighters, and injects host's mirrored fire bits so host
//         Blair's cannons & missiles (and enemy weapons) fire visibly.
static unsigned int __attribute__((fastcall)) weap_relay(void *weap)
{
    if (weap && !IsBadReadPtr(weap, 0x60)) {
        BYTE *phys = *(BYTE **)((BYTE *)weap + 4);
        BYTE *ctl  = *(BYTE **)((BYTE *)weap + 0xc);
        if (phys && ctl) {
            if (g_mode_client) {
                BYTE *local_plyr_phys = NULL;
                if (!IsBadReadPtr((void *)PLAYER_GLOBAL_VA, 4))
                    local_plyr_phys = *(BYTE **)PLAYER_GLOBAL_VA;
                if (phys == local_plyr_phys && g_client_eject_anim) {
                    wc3s_packet st;
                    int have_st = wc3net_state_fetch(&st);
                    if (!IsBadWritePtr(ctl + 4, 1))
                        ctl[4] &= (BYTE)~(0x01 | 0x04);
                    if (!IsBadWritePtr((void *)PLAYER_DEAD_DONE_VA, 1))
                        *(BYTE *)PLAYER_DEAD_DONE_VA = 0;
                    if (!IsBadWritePtr((void *)PLAYER_EJECT_VA, 1))
                        *(BYTE *)PLAYER_EJECT_VA = (g_client_eject_anim == 1) ? 1 : 0;
                    if (have_st && (st.wing_flags & WC3S_WING_DEAD) != 0)
                        g_client_eject_until = 0;
                    if (g_client_eject_anim == 1 && g_client_eject_until != 0 &&
                        (int)(g_client_eject_until - GetTickCount()) > 0) {
                        wc3net_ctl_snap ej;
                        ZeroMemory(&ej, sizeof(ej));
                        if (!IsBadReadPtr(ctl + 8, 4))
                            ej.throttle = *(int *)(ctl + 8);
                        ej.buttons = (int)WC3N_BTN_CLIENT_EJECT;
                        wc3net_client_offer(&ej);
                    }
                    {
                        int phase2 = (!IsBadReadPtr((void *)PLAYER_DEAD_PHASE2_VA, 1) &&
                                      *(BYTE *)PLAYER_DEAD_PHASE2_VA != 0);
                        int timer  = (!IsBadReadPtr((void *)PLAYER_DEAD_TIMER_VA, 4))
                                     ? *(int *)PLAYER_DEAD_TIMER_VA : 0x500;
                        int dt     = (!IsBadReadPtr((void *)FRAME_DT_VA, 4))
                                     ? *(int *)FRAME_DT_VA : 4;
                        int host_end, host_ap;
                        if (dt < 4) dt = 4;
                        host_end = (have_st && (st.hdr_flags & WC3S_FLAG_MISSION_END) != 0);
                        host_ap  = (have_st && (st.hdr_flags & WC3S_FLAG_AUTOPILOT) != 0);
                        if ((phase2 && timer <= dt + 4) || host_end || host_ap) {
                            int anim_mode = g_client_eject_anim;
                            if (!IsBadWritePtr((void *)PLAYER_DEAD_VA, 1)) *(BYTE *)PLAYER_DEAD_VA = 0;
                            if (!IsBadWritePtr((void *)PLAYER_EJECT_VA, 1)) *(BYTE *)PLAYER_EJECT_VA = 0;
                            if (!IsBadWritePtr((void *)PLAYER_DEAD_PHASE2_VA, 1)) *(BYTE *)PLAYER_DEAD_PHASE2_VA = 0;
                            if (!IsBadWritePtr((void *)PLAYER_DEAD_SFX_VA, 1)) *(BYTE *)PLAYER_DEAD_SFX_VA = 0;
                            if (!IsBadWritePtr((void *)PLAYER_DEAD_DONE_VA, 1)) *(BYTE *)PLAYER_DEAD_DONE_VA = 0;
                            if (!IsBadWritePtr((void *)AUTOPILOT_ACT_VA, 1)) *(BYTE *)AUTOPILOT_ACT_VA = 0;
                            if (!IsBadWritePtr((void *)PLAYER_DEAD_TIMER_VA, 4)) *(int *)PLAYER_DEAD_TIMER_VA = 0x500;
                            if (g_client_saved_p18 != 0 && !IsBadWritePtr(phys + 0x18, 4))
                                *(DWORD *)(phys + 0x18) = g_client_saved_p18;
                            g_client_eject_anim = 0;
                            g_client_spectating = 1;
                            client_reset_jump();
                            if (!host_end && !host_ap)
                                client_ensure_chase_cam();
                            log_ts_crit("life", anim_mode == 1
                                ? "client eject animation finished, switching to Blair external chase cam\r\n"
                                : "client death animation finished, switching to Blair external chase cam\r\n");
                        }
                    }
                } else if ((phys != local_plyr_phys || g_client_spectating) &&
                           !IsBadWritePtr(ctl + 4, 1)) {
                    unsigned char fire = client_fire_query(phys);
                    int cloaked = (!IsBadReadPtr(phys + 0xc5, 1) && phys[0xc5] != 0);
                    // Suppress unsynced local AI weapon fire on mirrored ships
                    // (and client player triggers while spectating host Blair).
                    ctl[4] &= (BYTE)~(0x01 | 0x04);
                    if (!cloaked && (fire & 0x02)) {
                        BYTE *def = *(BYTE **)((BYTE *)weap + 8);
                        ctl[4] |= 0x01;
                        // Full guns bank + sufficient energy so visual fire never stalls
                        if (def && !IsBadReadPtr(def + 0x24, 4) &&
                            !IsBadWritePtr((BYTE *)weap + 0x58, 4))
                            *(int *)((BYTE *)weap + 0x58) = *(int *)(def + 0x24);
                        if (!IsBadWritePtr((BYTE *)weap + 0x4c, 4) &&
                            *(int *)((BYTE *)weap + 0x4c) < 200)
                            *(int *)((BYTE *)weap + 0x4c) = 200;
                    }
                    if (!cloaked && (fire & 0x04)) {
                        ctl[4] |= 0x04;
                    }
                }
            } else if (g_mode_remote) {
                // Prevent wingman from firing stray missiles/guns during autopilot
                // or during the brief post-autopilot grace window before fresh client
                // input resumes.
                if (!g_wingman_dead && g_wingman &&
                    !IsBadReadPtr((BYTE *)g_wingman + 4, 4) &&
                    *(BYTE **)((BYTE *)g_wingman + 4) == phys) {
                    int in_ap = 0;
                    int in_grace = host_in_ap_or_grace(&in_ap);
                    if (in_ap || (in_grace && g_ai_off)) {
                        int p2, l2, r2, t2;
                        DWORD b2;
                        if (in_ap || !wc3net_remote_input(&p2, &l2, &r2, &t2, &b2)) {
                            if (!IsBadWritePtr(ctl + 4, 1))
                                ctl[4] &= (BYTE)~(0x01 | 0x04);
                        }
                    }
                }
                if (!IsBadReadPtr(ctl + 4, 1)) {
                    unsigned char c4 = ctl[4];
                    if (c4 & (0x01 | 0x04))
                        host_fire_latch(phys, c4);
                }
            }
        }
    }
    return g_weap_original(weap);
}

// Client-only DMGA damage relays (0x49d210 -> FUN_0045bb30 fighter,
// 0x49be30 -> FUN_00424760 capital ship). On the client, host is authoritative
// for all ship deaths and wingman hull/shields; blocking local lethal hull
// accumulation prevents divergent local projectile hits from prematurely
// killing enemies or host Blair on the client while still alive on the host.
// SHLD::take_hit (0x4585a0 / 0x41df00) still runs normally so shield flashes
// and armor hit sparks remain visually intact.
static unsigned int __attribute__((thiscall)) dmga_fgtr_relay(void *dmga, int section, int dmg)
{
    if (g_mode_client) return 0;
    return g_dmga_fgtr_orig(dmga, section, dmg);
}

static unsigned int __attribute__((thiscall)) dmga_cptl_relay(void *dmga, int section, int dmg)
{
    if (g_mode_client) return 0;
    return g_dmga_cptl_orig(dmga, section, dmg);
}

// Host mission-active check: simstate non-null and FUN_00450690 flight loop
// state DAT_004a41c8 == 1. When the host lands, dies, or hits Replay,
// DAT_004a41c8 leaves 1 and FUN_00451140 zeroes DAT_004a41ac; gating on both
// stops state_pump before g_wingman can dangle into freed heap memory.
static int host_mission_active(void)
{
    DWORD sim = 0, mode = 0;
    if (IsBadReadPtr((void *)SIMSTATE_VA, 4)) return 0;
    sim = *(DWORD *)SIMSTATE_VA;
    if (!sim) return 0;
    if (IsBadReadPtr((void *)SIMSTATE_MODE_VA, 4)) return 0;
    mode = *(DWORD *)SIMSTATE_MODE_VA;
    return mode == 1;
}

static int g_wingman_valid(void *ship, BYTE **out_phys)
{
    BYTE *phys;
    if (!ship || IsBadReadPtr(ship, 8)) return 0;
    if (*(DWORD *)ship != FGTR_CLASS_VA) return 0;
    phys = *(BYTE **)((BYTE *)ship + 4);
    if (!phys || IsBadReadPtr(phys + 0x24, 0x74)) return 0;
    if (*(BYTE **)(phys + 0x94) != (BYTE *)ship) return 0;
    if (phys[0x7d]) return 0;
    *out_phys = phys;
    return 1;
}

static int find_phys_part_idx(BYTE *target_phys)
{
    DWORD sim, count, array;
    int i;
    if (!target_phys || IsBadReadPtr((void *)SIMSTATE_VA, 4)) return -1;
    sim = *(DWORD *)SIMSTATE_VA;
    if (!sim || IsBadReadPtr((void *)(sim + PARTLIST_OFF), 8)) return -1;
    count = *(DWORD *)(sim + PARTLIST_OFF);
    array = *(DWORD *)(sim + PARTLIST_OFF + 4);
    if (!array || count > WALK_NODE_LIMIT) return -1;
    if (count && IsBadReadPtr((void *)array, count * NODE_STRIDE)) return -1;
    for (i = 0; i < (int)count; i++) {
        BYTE *entry = (BYTE *)(array + (DWORD)i * NODE_STRIDE);
        if (*(BYTE **)(entry + 0x76) == target_phys)
            return i;
    }
    return -1;
}

static void *find_live_wingman_ship(int preferred_part_idx, int *out_part_idx)
{
    world_ent ents[WORLD_ENT_MAX];
    int n = collect_world(ents, WORLD_ENT_MAX, NULL);
    int i;
    void *fallback = NULL;
    int fallback_idx = -1;
    for (i = 0; i < n; i++) {
        if (ents[i].owner != 1 || ents[i].dead) continue;
        if (!ents[i].ship || IsBadReadPtr(ents[i].ship, 4)) continue;
        if (*(DWORD *)ents[i].ship != FGTR_CLASS_VA) continue;
        if (preferred_part_idx >= 0 && (int)ents[i].part_idx == preferred_part_idx) {
            if (out_part_idx) *out_part_idx = (int)ents[i].part_idx;
            return ents[i].ship;
        }
        if (!fallback) {
            fallback = ents[i].ship;
            fallback_idx = (int)ents[i].part_idx;
        }
    }
    if (out_part_idx) *out_part_idx = fallback_idx;
    return fallback;
}

static void gf_push_state_now(void);
static void gf_record_host_event(unsigned char type, int arg0, int arg1);
static int gf_read_int(DWORD va, int def);
static void gf_write_int(DWORD va, int v);
static void gf_dbg_snap_arm(void);
static int  gf_dbg_snap_is_valid(void);
static void gf_dbg_snap_restore_mem(void);

// State pump. The wingman's own update cadence is 17-19 Hz, the engine
// staggers AI ship updates (measured 2026-11-18 by file-interleave in the
// session log), so a mirror riding the relay starved the client view to
// that rate. This thread reads the transform from stable memory on its own
// 60 Hz clock, the same guarded cross-thread reads the watchdog performs.
// A tear mid-transform costs one warped frame at worst, accepted until a
// double-buffer exists.
static void state_pump_send_frame(BYTE *pos_phys, int thr, int is_spec,
                                  int last_pos[3], int last_ori[9], int last_vel[3],
                                  int *last_thr, int *have_last, DWORD *sent)
{
    unsigned char hdr_flags = 0;
    unsigned char nav_idx = read_nav_idx();
    wc3s_wing_health wh;

    if (host_real_autopilot()) {
        hdr_flags |= WC3S_FLAG_AUTOPILOT;
        g_host_ap_grace_until = GetTickCount() + 3000;
    }
    if (host_in_navmap()) {
        hdr_flags |= WC3S_FLAG_NAVMAP;
        g_host_ap_grace_until = GetTickCount() + 3000;
        ZeroMemory(&wh, sizeof(wh));
        if (is_spec) wh.flags |= WC3S_WING_DEAD;
        if (wc3net_state_send(last_pos, last_ori, last_vel, *last_thr,
                              NULL, 0, 0xff, 0xff, 0, hdr_flags, &wh,
                              collect_part_dead_mask(), nav_idx))
            (*sent)++;
        return;
    }
    {
        int pos[3], orient[9], vel[3];
        wc3s_packet p;
        CopyMemory(pos, pos_phys + 0x28, sizeof(pos));
        CopyMemory(orient, pos_phys + 0x34, sizeof(orient));
        CopyMemory(vel, pos_phys + 0x58, sizeof(vel));
        ZeroMemory(&p, sizeof(p));
        build_world(&p);
        read_wing_health(pos_phys, &wh);
        if (is_spec) wh.flags |= WC3S_WING_DEAD;
        if (host_in_navmap()) {
            hdr_flags |= WC3S_FLAG_NAVMAP;
            g_host_ap_grace_until = GetTickCount() + 3000;
            ZeroMemory(&wh, sizeof(wh));
            if (is_spec) wh.flags |= WC3S_WING_DEAD;
            if (wc3net_state_send(last_pos, last_ori, last_vel, *last_thr,
                                  NULL, 0, 0xff, 0xff, 0, hdr_flags, &wh,
                                  collect_part_dead_mask(), nav_idx))
                (*sent)++;
            return;
        }
        CopyMemory(last_pos, pos, sizeof(pos));
        CopyMemory(last_ori, orient, sizeof(orient));
        CopyMemory(last_vel, vel, sizeof(vel));
        *last_thr = thr;
        *have_last = 1;
        if (wc3net_state_send(pos, orient, vel, thr, p.objects,
                              p.obj_count, p.idx_blair, p.idx_wing,
                              p.wing_voice, hdr_flags, &wh,
                              p.part_dead_mask, nav_idx))
            (*sent)++;
        if (*sent && (*sent % 60) == 0) {
            char buf[128];
            unsigned bfire = (p.idx_blair != 0xff) ? (unsigned)p.objects[p.idx_blair].flags : 0u;
            wsprintfA(buf,
                      is_spec ? "x=%d y=%d z=%d t=%d nobj=%d fl=%02x sh=%u/%u d=%u bf=%u spec=1\r\n"
                              : "x=%d y=%d z=%d t=%d nobj=%d fl=%02x sh=%u/%u d=%u bf=%u\r\n",
                      pos[0], pos[1], pos[2], thr, p.obj_count, hdr_flags,
                      (unsigned)wh.shld[0], (unsigned)wh.shld[2], (unsigned)wh.hull, bfire);
            log_ts("mirror", buf);
        }
    }
}

static DWORD WINAPI state_pump(LPVOID unused)
{
    static int last_pos[3], last_ori[9], last_vel[3], last_thr, have_last;
    static DWORD solo_wait_start;
    static DWORD wing_missing_since;
    DWORD sent = 0;
    int was_active = 0;
    (void)unused;
    for (;;) {
        void *ship = g_wingman;
        if (!host_mission_active()) {
            solo_wait_start = 0;
            wing_missing_since = 0;
            if (was_active) {
                int end_mode = 2;
                int zeros_pos[3] = {0}, zeros_ori[9] = {0}, zeros_vel[3] = {0};
                int k, w, is_replay_modal;
                if (!IsBadReadPtr((void *)SIMSTATE_MODE_VA, 4)) {
                    DWORD m = *(DWORD *)SIMSTATE_MODE_VA;
                    if (m >= 2 && m <= 10) end_mode = (int)m;
                }
                is_replay_modal = (gf_read_int(GF_IN_SIM_VA, 0) == 0 &&
                                   (end_mode == 3 || end_mode == 4));
                if (is_replay_modal) {
                    // Host is opening FUN_00430040 (Replay / Continue / Quit);
                    // SIMSTATE_VA stays non-zero until the host chooses.
                    g_host_in_replay_prompt = 1;
                } else {
                    // Wait up to 200 ms for FUN_00450690 / FUN_00424c50 to finish
                    // finalizing regs[] outcome and tearing down SIMSTATE_VA.
                    for (w = 0; w < 20; w++) {
                        if (IsBadReadPtr((void *)SIMSTATE_VA, 4) ||
                            *(DWORD *)SIMSTATE_VA == 0)
                            break;
                        Sleep(10);
                    }
                    if (!IsBadReadPtr((void *)SIMSTATE_MODE_VA, 4)) {
                        DWORD m = *(DWORD *)SIMSTATE_MODE_VA;
                        if (m >= 2 && m <= 10) end_mode = (int)m;
                    }
                    if (gf_dbg_snap_is_valid())
                        gf_dbg_snap_restore_mem();
                }
                // Push finalized host flags[256] + regs[96] BEFORE sending
                // MISSION_END so client captures the exact mission outcome.
                gf_push_state_now();
                Sleep(20);
                gf_push_state_now();
                // Send a short burst of MISSION_END packets so UDP loss
                // cannot leave the client stranded in the ended mission.
                for (k = 0; k < 10; k++) {
                    wc3net_state_send(zeros_pos, zeros_ori, zeros_vel,
                                      end_mode, NULL, 0, 0xff, 0xff, 0,
                                      WC3S_FLAG_MISSION_END, NULL, 0ULL, 0xff);
                    Sleep(20);
                }
                // Immediately flush a WC3G state packet so WC3G_HFLAG_IN_MISSION /
                // WC3G_HFLAG_IN_SIM clear on the client without waiting 100 ms.
                gf_push_state_now();
                was_active = 0;
                have_last = 0;
                g_host_in_navmap = 0;
                g_wingman = NULL;
                g_wingman_flight = NULL;
                g_wingman_dead = 0;
                g_wingman_part_idx = -1;
                g_ai_off = 0;
                g_host_ap_grace_until = 0;
                ZeroMemory(g_seen, sizeof(g_seen));
                ZeroMemory(g_host_fire, sizeof(g_host_fire));
                {
                    char lbuf[96];
                    wsprintfA(lbuf, "host mission ended (mode=%d), sent end burst and cleared wingman\r\n",
                              end_mode);
                    log_ts_crit("life", lbuf);
                }
            }
            if (g_host_in_replay_prompt) {
                DWORD sim = (!IsBadReadPtr((void *)SIMSTATE_VA, 4)) ? *(DWORD *)SIMSTATE_VA : 0;
                DWORD m   = (!IsBadReadPtr((void *)SIMSTATE_MODE_VA, 4)) ? *(DWORD *)SIMSTATE_MODE_VA : 0;
                if (m == 7) {
                    g_host_in_replay_prompt = 0;
                    gf_record_host_event(WC3G_EVT_REPLAY, 0, 0);
                } else if (m == 2) {
                    g_host_in_replay_prompt = 0;
                    gf_record_host_event(WC3G_EVT_REPLAY, 2, 0);
                } else if (sim == 0) {
                    g_host_in_replay_prompt = 0;
                    if (gf_dbg_snap_is_valid())
                        gf_dbg_snap_restore_mem();
                    gf_record_host_event(WC3G_EVT_REPLAY, 1, 0);
                }
            }
            Sleep(1000 / 60);
            continue;
        }
        gf_dbg_snap_arm();
        if (g_host_in_replay_prompt) {
            g_host_in_replay_prompt = 0;
            gf_record_host_event(WC3G_EVT_REPLAY, 0, 0);
        }
        if (g_mode_remote && ship) {
            BYTE *phys = NULL;
            solo_wait_start = 0;
            if (!g_wingman_dead && g_wingman_valid(ship, &phys) &&
                !IsBadReadPtr(phys + 0x28, 0x3c)) {
                int thr = 0;
                was_active = 1;
                wing_missing_since = 0;
                if (g_wingman_part_idx < 0)
                    g_wingman_part_idx = find_phys_part_idx(phys);
                if (!IsBadReadPtr((BYTE *)ship + 0x7c8, 4)) {
                    void *ctl = *(void **)((BYTE *)ship + 0x7c8);
                    if (ctl && !IsBadReadPtr((BYTE *)ctl + 8, 4))
                        thr = *(int *)((BYTE *)ctl + 8);
                }
                state_pump_send_frame(phys, thr, 0, last_pos, last_ori, last_vel,
                                      &last_thr, &have_last, &sent);
            } else if (was_active) {
                BYTE *blair_phys = NULL;
                int thr = 0;
                if (!IsBadReadPtr((void *)PLAYER_GLOBAL_VA, 4))
                    blair_phys = *(BYTE **)PLAYER_GLOBAL_VA;
                if (blair_phys && !IsBadReadPtr(blair_phys + 0x94, 4)) {
                    BYTE *ctl = ship_ctl_ptr(*(BYTE **)(blair_phys + 0x94));
                    if (ctl && !IsBadReadPtr(ctl + 8, 4))
                        thr = *(int *)(ctl + 8);
                }
                if (!g_wingman_dead) {
                    if (host_real_autopilot() || host_in_navmap()) {
                        // FUN_00434870 temporarily despawns non-player ships
                        // during sector-jump autopilot and respawns them at the
                        // destination navpoint. Keep streaming with is_spec=0
                        // so WC3S_FLAG_AUTOPILOT reaches the client without
                        // falsely declaring WC3S_WING_DEAD.
                        wing_missing_since = 0;
                        if (blair_phys && !IsBadReadPtr(blair_phys + 0x28, 0x3c)) {
                            state_pump_send_frame(blair_phys, thr, 0, last_pos, last_ori, last_vel,
                                                  &last_thr, &have_last, &sent);
                        }
                        Sleep(1000 / 60);
                        continue;
                    }
                    {
                        int new_pi = -1;
                        void *respawned = find_live_wingman_ship(g_wingman_part_idx, &new_pi);
                        if (respawned && g_wingman_valid(respawned, &phys) &&
                            !IsBadReadPtr(phys + 0x28, 0x3c)) {
                            int rthr = 0;
                            g_wingman = respawned;
                            g_wingman_flight = NULL;
                            if (new_pi >= 0) g_wingman_part_idx = new_pi;
                            wing_missing_since = 0;
                            log_ts_crit("life", "re-captured respawned wingman after sector jump\r\n");
                            if (!IsBadReadPtr((BYTE *)respawned + 0x7c8, 4)) {
                                void *ctl = *(void **)((BYTE *)respawned + 0x7c8);
                                if (ctl && !IsBadReadPtr((BYTE *)ctl + 8, 4))
                                    rthr = *(int *)((BYTE *)ctl + 8);
                            }
                            state_pump_send_frame(phys, rthr, 0, last_pos, last_ori, last_vel,
                                                  &last_thr, &have_last, &sent);
                            Sleep(1000 / 60);
                            continue;
                        }
                    }
                    {
                        int confirmed_dead = 0;
                        if (g_wingman_part_idx >= 0 && g_wingman_part_idx < 64 &&
                            (collect_part_dead_mask() & (1ULL << g_wingman_part_idx)) != 0) {
                            confirmed_dead = 1;
                        } else if (ship && !IsBadReadPtr(ship, 8) &&
                                   *(DWORD *)ship == FGTR_CLASS_VA) {
                            BYTE *op = *(BYTE **)((BYTE *)ship + 4);
                            if (op && !IsBadReadPtr(op + 0x24, 0x74) &&
                                *(BYTE **)(op + 0x94) == (BYTE *)ship && op[0x7d] != 0) {
                                confirmed_dead = 1;
                            }
                        }
                        if (!confirmed_dead) {
                            if (!host_in_ap_or_grace(NULL)) {
                                if (wing_missing_since == 0)
                                    wing_missing_since = GetTickCount();
                                else if ((GetTickCount() - wing_missing_since) >= 1000)
                                    confirmed_dead = 1;
                            } else {
                                wing_missing_since = 0;
                            }
                        }
                        if (!confirmed_dead) {
                            if (blair_phys && !IsBadReadPtr(blair_phys + 0x28, 0x3c)) {
                                state_pump_send_frame(blair_phys, thr, 0, last_pos, last_ori, last_vel,
                                                      &last_thr, &have_last, &sent);
                            }
                            Sleep(1000 / 60);
                            continue;
                        }
                        g_wingman_dead = 1;
                        g_wingman_flight = NULL;
                        g_ai_off = 0;
                        wing_missing_since = 0;
                        log_ts_crit("life", "host wingman destroyed, switching client to host spectator mode\r\n");
                    }
                }
                if (blair_phys && !IsBadReadPtr(blair_phys + 0x28, 0x3c)) {
                    state_pump_send_frame(blair_phys, thr, 1, last_pos, last_ori, last_vel,
                                          &last_thr, &have_last, &sent);
                }
            }
        } else if (g_mode_remote && !ship) {
            if (solo_wait_start == 0) {
                solo_wait_start = GetTickCount();
            } else if ((GetTickCount() - solo_wait_start) >= 400) {
                BYTE *blair_phys = NULL;
                if (!was_active) {
                    was_active = 1;
                    g_wingman_dead = 1;
                    g_wingman_flight = NULL;
                    g_ai_off = 0;
                    log_ts_crit("life", "solo mission (no wingman), streaming Blair spectator mode\r\n");
                }
                if (!IsBadReadPtr((void *)PLAYER_GLOBAL_VA, 4))
                    blair_phys = *(BYTE **)PLAYER_GLOBAL_VA;
                if (blair_phys && !IsBadReadPtr(blair_phys + 0x28, 0x3c)) {
                    int thr = 0;
                    if (!IsBadReadPtr(blair_phys + 0x94, 4)) {
                        BYTE *ctl = ship_ctl_ptr(*(BYTE **)(blair_phys + 0x94));
                        if (ctl && !IsBadReadPtr(ctl + 8, 4))
                            thr = *(int *)(ctl + 8);
                    }
                    state_pump_send_frame(blair_phys, thr, 1, last_pos, last_ori, last_vel,
                                          &last_thr, &have_last, &sent);
                }
            }
        }
        Sleep(1000 / 60);
    }
    return 0;
}

// Freshest client input onto the wingman ctl, auth-scaled. Called at frame
// rate from flight_relay as the primary apply and at tick rate from
// fgtr_relay as the fallback if the flight hook never landed.
static void apply_remote_ctl(void *ship)
{
    int pitch, lateral, rudder, throttle;
    DWORD buttons;
    void *ctl;
    BYTE *b;

    if (!wc3net_remote_input(&pitch, &lateral, &rudder, &throttle, &buttons))
        return;
    if ((buttons & WC3N_BTN_CLIENT_EJECT) && !g_wingman_dead &&
        ship && !IsBadReadPtr((BYTE *)ship + 4, 4)) {
        BYTE *phys = *(BYTE **)((BYTE *)ship + 4);
        if (phys && !IsBadWritePtr(phys + 0x7d, 1) && phys[0x7d] == 0) {
            phys[0x7d] = 1;
            if (!IsBadWritePtr(phys + 0x90, 4))
                *(int *)(phys + 0x90) = 0x300;
            log_ts_crit("life", "client eject received, destroying/ejecting host wingman\r\n");
        }
        return;
    }
    pitch *= g_auth; lateral *= g_auth; rudder *= g_auth;
    if (pitch > 512) pitch = 512; else if (pitch < -512) pitch = -512;
    if (lateral > 512) lateral = 512; else if (lateral < -512) lateral = -512;
    if (rudder > 512) rudder = 512; else if (rudder < -512) rudder = -512;
    if (IsBadReadPtr((BYTE *)ship + 0x7c8, 4)) return;
    ctl = *(void **)((BYTE *)ship + 0x7c8);
    if (!ctl || IsBadWritePtr((BYTE *)ctl + 0x20, 1)) return;
    b = (BYTE *)ctl;
    *(int *)(b + 8) = throttle;
    *(int *)(b + 0xc) = lateral;
    *(int *)(b + 0x10) = pitch;
    *(int *)(b + 0x14) = rudder;
    // FUN_00427310 branches on ship==player: the player consumes +0xc/+0x10/
    // +0x14, every other ship consumes +0x18/+0x1c/+0x20 as rate-LIMITED
    // target turn rates (x16 to match the player branch's effective units).
    // The wingman is not the player, so the AI fields are the ones that fly.
    *(int *)(b + 0x18) = lateral * 16;
    *(int *)(b + 0x1c) = pitch * 16;
    *(int *)(b + 0x20) = rudder * 16;
    if (!IsBadReadPtr((void *)AUTOPILOT_ACT_VA, 1) &&
        *(BYTE *)AUTOPILOT_ACT_VA != 0) {
        // Never apply fire/button bits during autopilot cutscene
        b[4] = 0;
    } else {
        b[4] = (BYTE)buttons;
        // high byte carries the client's ctl+5 flags; only afterburner (0x10)
        // and eject (0x80) cross the wire, the remaining flag bits stay
        // host-local
        b[5] |= (BYTE)((buttons >> 8) & (0x10 | 0x80));
    }
}

// Flight-model hook. The dispatcher's param_1 IS the phys object (the
// "node+0x94/0x98/0x9c" fields in unit4 notes are phys offsets), so the
// wingman's flight model is one chain: *(ship+4) -> phys, *(phys+0x98) ->
// flight object, slot 1 of its class vtable is the frame-rate consume call.
// First attempt walked the 0x7e registry array instead and bailed silently,
// leaving the tick-rate AI as the last ctl writer, the wrong-direction
// scripted turns of 2026-11-18 night. Every bail logs now.
static void __attribute__((fastcall)) flight_relay(void *obj);
static void hook_wingman_flight(void *ship)
{
    BYTE *phys, *obj, **vt, **slot;
    DWORD prot = 0;

    if (IsBadReadPtr((BYTE *)ship + 4, 4)) {
        log_line("flight hook bail, ship+4 unreadable\r\n");
        return;
    }
    phys = *(BYTE **)((BYTE *)ship + 4);
    if (!phys || IsBadReadPtr(phys + 0x9c, 4)) {
        log_line("flight hook bail, phys unreadable\r\n");
        return;
    }
    obj = *(BYTE **)(phys + 0x98);
    if (!obj || IsBadReadPtr(obj, 4)) {
        log_line("flight hook bail, flight object unreadable\r\n");
        return;
    }
    vt = *(BYTE ***)obj;
    if (!vt || IsBadReadPtr(vt + 1, 4)) {
        log_line("flight hook bail, vtable unreadable\r\n");
        return;
    }
    slot = (BYTE **)(vt + 1);
    g_wingman_flight = obj;
    if (*slot != (BYTE *)&flight_relay) {
        g_flight_original = (flight_fn)*slot;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &prot)) {
            log_line("flight vtable virtualprotect failed\r\n");
            return;
        }
        *slot = (BYTE *)&flight_relay;
        VirtualProtect(slot, 4, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), slot, 4);
    }
    {
        char buf[96];
        wsprintfA(buf, "flight relay installed, obj=%p slot=%p original=%p\r\n",
                  obj, slot, (void *)g_flight_original);
        log_line(buf);
    }
}

static void __attribute__((fastcall)) flight_relay(void *obj)
{
    if (!g_wingman_dead && obj == g_wingman_flight &&
        (g_mode_remote || g_mode_observe_net) && !g_mode_observe) {
        LONG n = InterlockedIncrement(&g_flight_calls);
        void *ship = g_wingman;
        if (ship) apply_remote_ctl(ship);
        if ((n % 600) == 0) {
            char buf[64];
            wsprintfA(buf, "flight#%ld frame-locked apply live\r\n", n);
            log_line(buf);
        }
    }
    g_flight_original(obj);
}

#define NET_PORT_DEFAULT 3443

static void ini_path(char *out, int len)
{
    // Bare filenames make GetPrivateProfile* search the Windows directory,
    // not the game dir. Resolve against our own DLL location instead.
    HMODULE self;
    char *slash;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ini_path, &self);
    if (!GetModuleFileNameA(self, out, len)) { lstrcpynA(out, "wc3net.ini", len); return; }
    slash = out + lstrlenA(out);
    while (slash > out && *slash != '\\') slash--;
    if (slash > out) slash++;
    lstrcpynA(slash, "wc3net.ini", len - (int)(slash - out));
}

static void read_config(void)
{
    static volatile LONG config_loaded;
    char mode[16];
    char log_str[16];
    char ini[MAX_PATH];
    if (InterlockedCompareExchange(&config_loaded, 1, 0) != 0)
        return;
    ini_path(ini, sizeof(ini));
    GetPrivateProfileStringA("wc3net", "mode", "off", mode, sizeof(mode), ini);
    // WC3NET_MODE overrides the ini string (compared case-insensitively
    // below) so two instances in one wine bottle can take host and client
    // roles from a single wc3net.ini
    {
        char env[32];
        DWORD n = GetEnvironmentVariableA("WC3NET_MODE", env, sizeof(env));
        if (n > 0 && n < sizeof(env))
            lstrcpynA(mode, env, sizeof(mode));
    }
    g_mode_remote = lstrcmpiA(mode, "remote") == 0;
    g_mode_client = lstrcmpiA(mode, "client") == 0;
    g_mode_observe = lstrcmpiA(mode, "observe") == 0;
    g_mode_observe_net = lstrcmpiA(mode, "observenet") == 0;
    g_mode_script = lstrcmpiA(mode, "script") == 0;
    g_mode_off = !(g_mode_remote || g_mode_client || g_mode_observe ||
                   g_mode_observe_net || g_mode_script);
    g_net_disabled = GetPrivateProfileIntA("wc3net", "net", 1, ini) == 0;
    // authority dial: the wingman AI writes formation steering every tick
    // and native-scale client axes can stalemate it, the multiplier
    // brute-forces through the way wc3stick's +-512 did
    g_auth = GetPrivateProfileIntA("wc3net", "auth", 2, ini);
    if (g_auth < 1) g_auth = 1;
    if (g_auth > 4) g_auth = 4;
    GetPrivateProfileStringA("wc3net", "host", "", g_host, sizeof(g_host), ini);
    g_port = GetPrivateProfileIntA("wc3net", "port", NET_PORT_DEFAULT, ini);
    GetPrivateProfileStringA("wc3net", "logging", "off", log_str, sizeof(log_str), ini);
    {
        char env[32];
        DWORD n = GetEnvironmentVariableA("WC3NET_LOGGING", env, sizeof(env));
        if (n > 0 && n < sizeof(env))
            lstrcpynA(log_str, env, sizeof(log_str));
    }
    if (lstrcmpiA(log_str, "off") == 0 || lstrcmpiA(log_str, "none") == 0 ||
        lstrcmpiA(log_str, "0") == 0) {
        g_log_mode = LOG_MODE_OFF;
    } else if (lstrcmpiA(log_str, "critical") == 0 || lstrcmpiA(log_str, "crit") == 0) {
        g_log_mode = LOG_MODE_CRITICAL;
    } else {
        g_log_mode = LOG_MODE_DEBUG;
    }
}

void wc3net_on_attach(HMODULE game)
{
    char buf[160];
    read_config();
    if (g_log_mode == LOG_MODE_OFF)
        return;
    log_rotate_sessions();
    wsprintfA(buf, "wc3net v0 attach pid=%lu game=%p tick=%lu\r\n",
              GetCurrentProcessId(), game, GetTickCount());
    log_crit(buf);
}

// fields per notes/unit4_ctl/findings.md
static int possess_write(void *ship, LONG frame)
{
    void *ctl;
    BYTE *b;
    if (IsBadReadPtr((BYTE *)ship + 0x7c8, 4)) return 0;
    ctl = *(void **)((BYTE *)ship + 0x7c8);
    if (!ctl || IsBadWritePtr((BYTE *)ctl + 0x20, 1)) return 0;
    b = (BYTE *)ctl;
    *(int *)(b + 8) = 60;                 // throttle
    *(int *)(b + 0x10) = 6;               // pitch over, gentle
    b[4] = (frame >= 120 && frame < 210) ? (BYTE)(b[4] | 1) : (BYTE)(b[4] & ~1);  // fire 2.0-3.5 s
    return 1;
}

static void scrub_wingman_ready_comm(void *ship, int prev_comm_ts, int prev_comm_msg)
{
    static LONG sup_cnt;
    int cnt, i = 0, removed = 0;
    DWORD target_id;

    if (!ship) return;
    if (IsBadReadPtr((void *)COMM_QUEUE_CNT_VA, 4) ||
        IsBadWritePtr((void *)COMM_QUEUE_CNT_VA, 4) ||
        IsBadWritePtr((void *)COMM_QUEUE_CUR_VA, 4) ||
        IsBadWritePtr((void *)COMM_QUEUE_BUF_VA, 8 * 0x24))
        return;
    cnt = *(int *)COMM_QUEUE_CNT_VA;
    if (cnt <= 0 || cnt > 8) return;
    target_id = (DWORD)(size_t)((BYTE *)ship + 0xc);
    while (i < cnt) {
        BYTE *ent = (BYTE *)COMM_QUEUE_BUF_VA + i * 0x24;
        if (*(DWORD *)(ent + 8) == target_id &&
            *(DWORD *)(ent + 0xc) == COMM_MSG_READY_ATK) {
            BYTE *last;
            DWORD cur = *(DWORD *)COMM_QUEUE_CUR_VA;
            cnt--;
            *(int *)COMM_QUEUE_CNT_VA = cnt;
            last = (BYTE *)COMM_QUEUE_BUF_VA + cnt * 0x24;
            if (cur == (DWORD)(size_t)ent)
                *(DWORD *)COMM_QUEUE_CUR_VA = 0;
            if (i < cnt) {
                CopyMemory(ent, last, 0x24);
                if (cur == (DWORD)(size_t)last)
                    *(DWORD *)COMM_QUEUE_CUR_VA = (DWORD)(size_t)ent;
            }
            removed++;
        } else {
            i++;
        }
    }
    if (removed > 0) {
        LONG c = InterlockedIncrement(&sup_cnt);
        if (!IsBadWritePtr((BYTE *)ship + 0x6fc, 12)) {
            *(int *)((BYTE *)ship + 0x6fc) = prev_comm_msg;
            *(int *)((BYTE *)ship + 0x704) = prev_comm_ts;
        }
        if (c == 1 || (c % 10) == 0)
            log_ts("comm", "suppressed wingman ready-to-attack (0x0b) message\r\n");
    }
}

static unsigned char __attribute__((fastcall)) fgtr_relay(void *ship)
{
    LONG n = InterlockedIncrement(&g_calls);
    int i;
    int slot = -1;
    int check_comm = 0, prev_comm_msg = 0, prev_comm_ts = 0;
    unsigned char status;

    for (i = 0; i < SEEN_MAX; i++) {
        if (g_seen[i].ship == ship) { slot = i; break; }
        if (g_seen[i].ship == NULL && slot == -1) slot = i;
    }
    if (slot != -1) {
        if (g_seen[slot].ship == NULL) {
            g_seen[slot].ship = ship;
            g_seen[slot].count = 0;
            if (n < LOG_TOTAL_MAX) log_ship_state(ship, n, "new");
            if (!g_wingman && ship && !IsBadReadPtr(ship, 4)) {
                void *phys = *(void **)((BYTE *)ship + 4);
                if (phys && !IsBadReadPtr((BYTE *)phys + 0x71, 1) &&
                    *(BYTE *)((BYTE *)phys + 0x70) == 1) {
                    g_wingman = ship;
                    g_wingman_dead = 0;
                    g_wingman_part_idx = find_phys_part_idx((BYTE *)phys);
                    log_crit("possession target captured\r\n");
                    if (InterlockedCompareExchange(&g_threads_started, 1, 0) == 0) {
                        CreateThread(NULL, 0, wing_watchdog, NULL, 0, NULL);
                        // the mirror must not ride the wingman's own cadence
                        if (g_mode_remote && !g_net_disabled &&
                            InterlockedCompareExchange(&g_state_pump_started, 1, 0) == 0)
                            CreateThread(NULL, 0, state_pump, NULL, 0, NULL);
                    }
                    hook_wingman_flight(ship);
                }
            }
        }
        LONG c = InterlockedIncrement(&g_seen[slot].count);
        if (c % SAMPLE_EVERY == 0)
            log_ship_state(ship, c, "tick");
    }
    if (g_mode_remote && !g_wingman_dead && ship && !IsBadReadPtr(ship, 8)) {
        BYTE *phys = *(BYTE **)((BYTE *)ship + 4);
        if (phys && !IsBadReadPtr(phys + 0x24, 0x78) &&
            phys[0x70] == 1 && !phys[0x7d]) {
            if (g_wingman != ship) {
                BYTE *cur_p = NULL;
                if (!g_wingman_valid(g_wingman, &cur_p)) {
                    int pi = find_phys_part_idx(phys);
                    if (g_wingman_part_idx < 0 || pi < 0 || pi == g_wingman_part_idx) {
                        g_wingman = ship;
                        if (pi >= 0) g_wingman_part_idx = pi;
                        hook_wingman_flight(ship);
                        log_ts_crit("life", "fgtr_relay re-captured respawned wingman\r\n");
                    }
                }
            } else {
                void *fobj = *(void **)(phys + 0x98);
                if (fobj && fobj != g_wingman_flight)
                    hook_wingman_flight(ship);
            }
        }
    }
    if (g_mode_remote && ship == g_wingman && !g_wingman_dead &&
        !IsBadReadPtr((BYTE *)ship + 0x6fc, 12)) {
        prev_comm_msg = *(int *)((BYTE *)ship + 0x6fc);
        prev_comm_ts = *(int *)((BYTE *)ship + 0x704);
        check_comm = 1;
    }
    status = g_original(ship);
    if (check_comm)
        scrub_wingman_ready_comm(ship, prev_comm_ts, prev_comm_msg);

    if (g_mode_observe || g_mode_observe_net) return status;   // observation builds never write

    // AI disengagement, the single-simulation architecture: while client
    // input is fresh, three data writes per tick quiet the wingman's AI so
    // the client is the only pilot. ship+0x711=1 routes FUN_00463890 to the
    // FUN_00444540 thinker, away from the goal machine FUN_00462510's
    // steering and decision calls. ship+0x640=0 drops goal-object steering.
    // phys+0x7e/0x85=0 makes FUN_00462b30 return quietly without targeting.
    // On input timeout the routing byte restores and the goal machinery
    // rebuilds its own state. Residual: one behavior-object vcall in the
    // 444540 default tail and the 439830 geometry pass, both overwritten by
    // the post-update input apply before any consumer reads ctl.
    if (g_wingman == ship && !g_wingman_dead && g_mode_remote) {
        int p2, l2, r2, t2;
        DWORD b2;
        int fresh = wc3net_remote_input(&p2, &l2, &r2, &t2, &b2);
        int in_ap = 0;
        int hold_ai_off = g_ai_off && host_in_ap_or_grace(&in_ap);
        if ((fresh || hold_ai_off) && !IsBadWritePtr((BYTE *)ship + 0x711, 1)) {
            if (!g_ai_off) {
                g_ai_off = 1;
                log_line("ai disengaged, client is the pilot\r\n");
            }
            ((BYTE *)ship)[0x711] = 1;
            if (!IsBadWritePtr((BYTE *)ship + 0x640, 4))
                *(int *)((BYTE *)ship + 0x640) = 0;
            if (!IsBadReadPtr((BYTE *)ship + 4, 4)) {
                BYTE *phys = *(BYTE **)((BYTE *)ship + 4);
                if (phys && !IsBadWritePtr(phys + 0x85, 1)) {
                    phys[0x7e] = 0;
                    phys[0x85] = 0;
                }
            }
            // If we are holding AI off across an autopilot cutscene gap without
            // fresh client input yet, clear any AI-written weapon fire bits.
            if (!fresh && !IsBadReadPtr((BYTE *)ship + 0x7c8, 4)) {
                BYTE *ctl = *(BYTE **)((BYTE *)ship + 0x7c8);
                if (ctl && !IsBadWritePtr(ctl + 4, 1))
                    ctl[4] &= (BYTE)~(0x01 | 0x04);
            }
        } else if (g_ai_off && !IsBadWritePtr((BYTE *)ship + 0x711, 1)) {
            ((BYTE *)ship)[0x711] = 0;
            g_ai_off = 0;
            log_line("ai reengaged, input timed out\r\n");
        }
    }

    if (g_wingman == ship && !g_wingman_dead && g_mode_remote) {
        // tick-rate fallback apply; the frame-locked flight_relay is the
        // primary whenever its hook landed
        apply_remote_ctl(ship);
        return status;
    }

    if (g_wingman == ship && g_possess_frames == 0 && !g_possess_done) {
        g_possess_frames = 300;           // 5 s at 60 Hz
        g_possess_done = 1;
        log_line("possession script start, 300 frames\r\n");
    }
    if (g_wingman == ship && g_possess_frames > 0) {
        LONG f = InterlockedDecrement(&g_possess_frames);
        if (possess_write(ship, 299 - f)) {
            if (f == 0) log_line("possession script end\r\n");
        } else {
            g_possess_frames = 0;
            log_line("possession aborted, ctl not writable\r\n");
        }
    }
    return status;
}

// ---- Client-side transform sanity, the cheap half of the stream
// lifecycle clamps. Range check first, then a per-node teleport guard: an
// insane jump is held for one frame and only a repeated candidate is
// accepted, so the host-teardown garbage stream (the Replay crash, user
// leg finding 4) never reaches a stomp, while a genuine sector jump
// passes after exactly one held frame. Game thread only, no lock needed.
#define JUMP_SLOTS 24
static struct {
    BYTE *node;
    int   last[3];
    int   cand[3];
    int   have_last, have_cand, live;
} g_jump[JUMP_SLOTS];

static void client_reset_jump(void)
{
    ZeroMemory(g_jump, sizeof(g_jump));
}

static int transform_sane(const int pos[3], const int orient[9])
{
    int i;
    for (i = 0; i < 3; i++)
        if (pos[i] >= POS_LIMIT || pos[i] <= -POS_LIMIT) return 0;
    for (i = 0; i < 9; i++)
        if (orient[i] >= ORI_LIMIT || orient[i] <= -ORI_LIMIT) return 0;
    return 1;
}

// returns nonzero when the transform was written
static int stomp_transform_checked(BYTE *node, const int pos[3],
                                   const int orient[9])
{
    static DWORD last_reject_log;
    int i, slot = -1;
    if (!transform_sane(pos, orient)) {
        if (GetTickCount() - last_reject_log >= 1000) {
            last_reject_log = GetTickCount();
            log_line("insane transform rejected before stomp\r\n");
        }
        return 0;
    }
    for (i = 0; i < JUMP_SLOTS; i++) {
        if (g_jump[i].live && g_jump[i].node == node) { slot = i; break; }
        if (slot == -1 && !g_jump[i].live) slot = i;
    }
    if (slot == -1) {
        // table full: write guarded rather than drop a world object
        if (IsBadWritePtr(node + 0x28, 0x30)) return 0;
        CopyMemory(node + 0x28, pos, 12);
        CopyMemory(node + 0x34, orient, 36);
        return 1;
    }
    if (!g_jump[slot].live) {
        g_jump[slot].live = 1;
        g_jump[slot].node = node;
        g_jump[slot].have_last = 0;
        g_jump[slot].have_cand = 0;
    }
    if (g_jump[slot].have_cand) {
        // Accept the held jump candidate if the next packet stays near it
        // (moving ships advance ~3000 units/frame after autopilot, so exact
        // equality previously froze stomps permanently after any nav jump
        // larger than JUMP_LIMIT while moving!).
        int repeat = 1;
        for (i = 0; i < 3; i++) {
            int d = pos[i] - g_jump[slot].cand[i];
            if (d > CAND_NEAR_LIMIT || d < -CAND_NEAR_LIMIT) repeat = 0;
        }
        g_jump[slot].have_cand = 0;
        if (!repeat) {                     // new candidate, hold one frame
            g_jump[slot].cand[0] = pos[0];
            g_jump[slot].cand[1] = pos[1];
            g_jump[slot].cand[2] = pos[2];
            g_jump[slot].have_cand = 1;
            return 0;
        }                                  // near candidate: a real jump, accept
    } else if (g_jump[slot].have_last) {
        int jump = 0;
        for (i = 0; i < 3; i++) {
            int d = pos[i] - g_jump[slot].last[i];
            if (d > JUMP_LIMIT || d < -JUMP_LIMIT) jump = 1;
        }
        if (jump) {
            g_jump[slot].cand[0] = pos[0];
            g_jump[slot].cand[1] = pos[1];
            g_jump[slot].cand[2] = pos[2];
            g_jump[slot].have_cand = 1;
            return 0;
        }
    }
    if (IsBadWritePtr(node + 0x28, 0x30)) {
        g_jump[slot].live = 0;             // node gone, free the slot
        return 0;
    }
    CopyMemory(node + 0x28, pos, 12);
    CopyMemory(node + 0x34, orient, 36);
    g_jump[slot].last[0] = pos[0];
    g_jump[slot].last[1] = pos[1];
    g_jump[slot].last[2] = pos[2];
    g_jump[slot].have_last = 1;
    return 1;
}

static void kill_local_ship(BYTE *phys, int fast)
{
    if (!phys || IsBadWritePtr(phys + 0x7d, 1)) return;
    if (phys[0x7d]) return;  // already dying/dead
    phys[0x7d] = 1;
    // FUN_0046ae10 spawns zapx2 + explosion SFX when phys+0x90 == 0x300
    // (syncs full death animation when enemy is actively dying on host);
    // fast=1 sets 0x10 for immediate clean removal when host already despawned it.
    if (!IsBadWritePtr(phys + 0x90, 4))
        *(int *)(phys + 0x90) = fast ? 0x10 : 0x300;
}

static int stomp_object_checked(BYTE *node, BYTE *ship, const wc3s_object *o)
{
    // field-wise copy, never an address into the packed packet
    // (-Waddress-of-packed-member); every member is 4 B aligned by layout
    int pos[3], ori[9];
    int i, ok;
    (void)ship;
    for (i = 0; i < 3; i++) pos[i] = o->pos[i];
    for (i = 0; i < 9; i++) ori[i] = o->orient[i];
    ok = stomp_transform_checked(node, pos, ori);
    if (o->flags & 1) {
        kill_local_ship(node, 0);
        client_fire_set(node, 0);
    } else {
        // Host says this ship is ALIVE: clear any accidental local collision
        // death latch so divergent collisions can never kill a live host ship.
        if (!IsBadWritePtr(node + 0x7d, 1) && node[0x7d] != 0)
            node[0x7d] = 0;
        if (ok) {
            // Store host weapon fire bits for weap_relay, which injects them
            // into ctl+4 right before FUN_0040c370 (after FUN_00410440 clears it).
            client_fire_set(node, (unsigned char)(o->flags & 0x06));
        }
    }
    return ok;
}

// Client world apply, called from plyr_relay after the own-ship stomp.
// The client's own PLYR carries the wingman transform (the cockpit), so
// the host's Blair object is SWAPPED onto this client's local wingman
// node: the host craft becomes visible at its true position, wearing the
// wingman's local model until v2's camera work makes the mapping exact.
// The host wingman object is already on the PLYR and goes nowhere else.
// Everything else pairs statelessly by owner plus name hash in registry
// order, both arrays built from the same mission file, so no learned
// binding can go stale. Unmatched locals keep flying locally and are
// counted, tolerated until the lifecycle increment owns removals.
static void apply_world(const wc3s_packet *p, void *local_plyr)
{
    static LONG calls;
    static BYTE *muted_ship;
    static unsigned char seen_host_part[256];
    static unsigned char miss_frames[256];
    static DWORD last_sim;
    DWORD cur_sim = 0;
    world_ent loc[LOCAL_NODE_MAX];
    int nloc, i, k;
    BYTE *local_wing = NULL;
    BYTE *local_wing_ship = NULL;
    unsigned long wing_hash = 0;
    int voice_hit = 0, applied = 0, misses = 0, extras = 0;
    int wing_dead = (p->wing_flags & WC3S_WING_DEAD) != 0;
    LONG nc = InterlockedIncrement(&calls);

    if (!IsBadReadPtr((void *)SIMSTATE_VA, 4)) cur_sim = *(DWORD *)SIMSTATE_VA;
    if (cur_sim != last_sim) {
        ZeroMemory(seen_host_part, sizeof(seen_host_part));
        ZeroMemory(miss_frames, sizeof(miss_frames));
        ZeroMemory(g_client_fire, sizeof(g_client_fire));
        ZeroMemory(g_jump, sizeof(g_jump));
        g_client_spectating = 0;
        g_client_eject_anim = 0;
        g_client_eject_until = 0;
        g_gf_pending_replay = 0;
        muted_ship = NULL;
        last_sim = cur_sim;
    }

    if (p->obj_count == 0 || p->obj_count > WC3S_OBJ_MAX) return;
    if ((p->idx_blair != 0xff && p->idx_blair >= WC3S_OBJ_MAX) ||
        (p->idx_wing != 0xff && p->idx_wing >= WC3S_OBJ_MAX)) return;
    if (p->idx_wing != 0xff) wing_hash = p->objects[p->idx_wing].namehash;

    nloc = collect_world(loc, LOCAL_NODE_MAX, NULL);
    for (i = 0; i < nloc; i++)
        if (loc[i].ship && loc[i].ship == (BYTE *)local_plyr)
            loc[i].used = 2;               // cockpit carrier, never stomped here

    if (!wing_dead) {
        // local wingman node (host's captured FGTR): match by exact participant
        // table index first (identical across host/client for the same mission),
        // then voice/weap ptr, then owner==1 + type hash, then any friendly FGTR.
        if (p->idx_wing != 0xff && p->objects[p->idx_wing].part_idx != 0xff) {
            unsigned char wpidx = p->objects[p->idx_wing].part_idx;
            for (i = 0; i < nloc; i++) {
                if (loc[i].used || loc[i].part_idx != wpidx) continue;
                if (!loc[i].ship || *(DWORD *)loc[i].ship != FGTR_CLASS_VA) continue;
                local_wing = loc[i].node;
                local_wing_ship = loc[i].ship;
                loc[i].used = 3;
                voice_hit = 2;
                break;
            }
        }
        if (!local_wing && p->wing_voice) {
            for (i = 0; i < nloc; i++) {
                if (loc[i].used || loc[i].owner != 1) continue;
                if (!loc[i].ship || *(DWORD *)loc[i].ship != FGTR_CLASS_VA) continue;
                if (!IsBadReadPtr(loc[i].node + 0x9c, 4) &&
                    *(unsigned long *)(loc[i].node + 0x9c) == p->wing_voice) {
                    local_wing = loc[i].node;
                    local_wing_ship = loc[i].ship;
                    loc[i].used = 3;
                    voice_hit = 1;
                    break;
                }
            }
        }
        if (!local_wing) {
            for (i = 0; i < nloc; i++) {
                if (loc[i].used || loc[i].owner != 1) continue;
                if (!loc[i].ship || *(DWORD *)loc[i].ship != FGTR_CLASS_VA) continue;
                if (wing_hash && loc[i].hash != wing_hash) continue;
                local_wing = loc[i].node;
                local_wing_ship = loc[i].ship;
                loc[i].used = 3;
                break;
            }
        }
        if (!local_wing) {
            for (i = 0; i < nloc; i++) {
                if (loc[i].used || loc[i].owner != 1) continue;
                if (!loc[i].ship || *(DWORD *)loc[i].ship != FGTR_CLASS_VA) continue;
                local_wing = loc[i].node;
                local_wing_ship = loc[i].ship;
                loc[i].used = 3;
                break;
            }
        }
        // quiet the local wingman's radio while it wears Blair's transform
        // (comms-mute ship+0x712, unit2), once per captured ship
        if (local_wing_ship && local_wing_ship != muted_ship &&
            !IsBadWritePtr(local_wing_ship + 0x712, 1)) {
            local_wing_ship[0x712] = 1;
            muted_ship = local_wing_ship;
            log_line("local wingman node muted\r\n");
        }
        // Unit 6 Req 4: label host's craft "MAVERICK" instead of "HOBBES" (or
        // whichever wingman was replaced) on the client's targeting MFD/HUD.
        // Target MFD (FUN_00446380) renders the callsign string at
        // (*(phys+0x94)) + 0x5c7 (i.e. ship + 0x5c7); also overwrite phys+4.
        if (local_wing && !IsBadWritePtr(local_wing + 4, 9)) {
            if (local_wing[4] != 'M' || local_wing[5] != 'A' || local_wing[6] != 'V') {
                CopyMemory(local_wing + 4, "MAVERICK", 9);
            }
        }
        if (local_wing_ship && !IsBadWritePtr(local_wing_ship + 0x5c7, 9)) {
            if (local_wing_ship[0x5c7] != 'M' || local_wing_ship[0x5c8] != 'A' ||
                local_wing_ship[0x5c9] != 'V') {
                CopyMemory(local_wing_ship + 0x5c7, "MAVERICK", 9);
                log_line("host craft MFD callsign set to MAVERICK (ship+0x5c7)\r\n");
            }
        }
        // the swap: host Blair rides the local wingman node
        if (local_wing && p->idx_blair != 0xff)
            applied += stomp_object_checked(local_wing, local_wing_ship,
                                            &p->objects[p->idx_blair]);
    } else {
        // Spectator mode: client's own PLYR already carries Blair's craft/transform.
        // Mirror Blair's weapons fire onto local_plyr_phys and remove any local
        // friendly FGTR wingman node so it never clips into Blair's model.
        if (p->idx_blair != 0xff && local_plyr && !IsBadReadPtr(local_plyr, 8)) {
            BYTE *lphys = *(BYTE **)((BYTE *)local_plyr + 4);
            if (lphys)
                client_fire_set(lphys, (unsigned char)(p->objects[p->idx_blair].flags & 0x06));
        }
        for (i = 0; i < nloc; i++) {
            if (loc[i].used || loc[i].owner != 1) continue;
            if (!loc[i].ship || *(DWORD *)loc[i].ship != FGTR_CLASS_VA) continue;
            if (!loc[i].dead) {
                kill_local_ship(loc[i].node, 1);
                client_fire_set(loc[i].node, 0);
            }
            loc[i].used = 3;
        }
    }
    // everything else pairs by exact participant-table index first (both
    // instances load the same mission file so indices match across spawns),
    // falling back to owner plus hash if an index is absent
    for (k = 0; k < (int)p->obj_count; k++) {
        const wc3s_object *o = &p->objects[k];
        int found = 0;
        if (k == p->idx_blair || k == p->idx_wing) continue;
        if (o->namehash == 0) { misses++; continue; }   // unverifiable, never pair
        if (o->part_idx != 0xff) {
            for (i = 0; i < nloc; i++) {
                if (loc[i].used) continue;
                if (loc[i].part_idx != o->part_idx) continue;
                if (loc[i].hash != o->namehash) continue;
                loc[i].used = 1;
                if (stomp_object_checked(loc[i].node, loc[i].ship, o)) applied++;
                found = 1;
                break;
            }
        }
        if (!found) {
            for (i = 0; i < nloc; i++) {
                if (loc[i].used) continue;
                if (loc[i].owner != o->owner || loc[i].hash != o->namehash) continue;
                loc[i].used = 1;
                if (stomp_object_checked(loc[i].node, loc[i].ship, o)) applied++;
                found = 1;
                break;
            }
        }
        if (!found) misses++;
    }
    // Authoritative enemy/participant death sync:
    // Every WC3S packet carries host part_dead_mask (bits 0..63 from host
    // simstate+0x3b entry+0x50 bit 1 and phys[0x7d]). Immediately kill any
    // non-player, non-local_wing ship whose participant bit is set on host,
    // even if it died while the client was in autopilot or during packet loss.
    if (!(p->hdr_flags & WC3S_FLAG_AUTOPILOT)) {
        for (i = 0; i < nloc; i++) {
            if (loc[i].used == 2 || loc[i].used == 3) continue;
            if (loc[i].part_idx < 64 &&
                (p->part_dead_mask & (1ULL << loc[i].part_idx)) != 0) {
                if (!loc[i].dead) {
                    // If not currently in host objects[] (already despawned on host),
                    // remove quickly; if still exploding on host, play full explosion.
                    kill_local_ship(loc[i].node, !loc[i].used);
                    client_fire_set(loc[i].node, 0);
                }
            }
        }
        // Fallback disappearance-kill only for participant indices >= 64
        for (k = 0; k < (int)p->obj_count; k++) {
            unsigned char pi = p->objects[k].part_idx;
            if (pi >= 64 && pi < 255) {
                seen_host_part[pi] = 1;
                miss_frames[pi] = 0;
            }
        }
        for (i = 0; i < nloc; i++) {
            if (!loc[i].used) {
                int in_host = 0;
                extras++;
                for (k = 0; k < (int)p->obj_count; k++) {
                    if (p->objects[k].part_idx == loc[i].part_idx) {
                        in_host = 1;
                        break;
                    }
                }
                if (!in_host && !loc[i].dead && loc[i].owner != 1 &&
                    loc[i].part_idx >= 64 && loc[i].part_idx < 255 &&
                    seen_host_part[loc[i].part_idx]) {
                    if (++miss_frames[loc[i].part_idx] >= 30) {
                        kill_local_ship(loc[i].node, 1);
                        seen_host_part[loc[i].part_idx] = 0;
                        miss_frames[loc[i].part_idx] = 0;
                    }
                }
            }
        }
    } else {
        ZeroMemory(seen_host_part, sizeof(seen_host_part));
        ZeroMemory(miss_frames, sizeof(miss_frames));
    }
    if ((nc % 60) == 0) {
        char buf[128];
        wsprintfA(buf, "app=%d miss=%d extra=%d wingv=%d nloc=%d\r\n",
                  applied, misses, extras, voice_hit, nloc);
        log_ts("world", buf);
    }
}

// Switch client camera to external CHASE mode (mode 4) following DAT_004ab3c0.
// FUN_0045b590 only handles modes 0 (COCKPIT) and 6 (COCKHUD), so we call
// FUN_0048cc10(&DAT_004b1830, "CHASE") + FUN_0048cdb0(&DAT_004b1830, cam) directly.
static void client_ensure_chase_cam(void)
{
    cam_find_fn cam_find;
    cam_switch_fn cam_switch;
    void *cam;

    if (IsBadReadPtr((void *)CAM_MODE_VA, 2)) return;
    if (*(short *)CAM_MODE_VA == 4) return;
    cam_find = (cam_find_fn)CAM_FIND_VA;
    cam_switch = (cam_switch_fn)CAM_SWITCH_VA;
    cam = cam_find((void *)CAM_MGR_VA, "CHASE");
    if (cam) {
        cam_switch((void *)CAM_MGR_VA, cam);
        if (!IsBadWritePtr((void *)(CAM_MGR_VA + 0x10), 4))
            *(DWORD *)(CAM_MGR_VA + 0x10) = 0;
        if (!IsBadWritePtr((void *)CAM_MODE_VA, 2))
            *(short *)CAM_MODE_VA = 4;
        if (!IsBadWritePtr((void *)CAM_HUD_FLAG_VA, 1))
            *(BYTE *)CAM_HUD_FLAG_VA = 1;
        if (!IsBadWritePtr((void *)CAM_DIRTY_FLAG_VA, 1))
            *(BYTE *)CAM_DIRTY_FLAG_VA = 1;
    }
}

// Client relay: let the game's own input pipeline fill the player control
// block (FUN_00429470 inside the original update), then snapshot it for
// the sender mailbox. After the offer, the freshest WC3S stomps this PLYR's
// own transform: the camera and scene family follows the player-ship
// global DAT_004ab3c0, so moving the player moves the cockpit with no
// camera work. Writes are data fields only, no game code bytes are ever
// written. STANDING RULE.
static int __attribute__((fastcall)) plyr_relay(void *ship)
{
    static int prev_host_ap;
    static int prev_host_nav;
    static int prev_wing_dead;
    static int ever_had_wingman;
    LONG n = InterlockedIncrement(&g_plyr_calls);
    int rc;

    if (n == 1) {
        char buf[96];
        wsprintfA(buf, "plyr first sight, ship=%p\r\n", ship);
        log_line(buf);
    }
    gf_dbg_snap_arm();
    rc = g_plyr_original(ship);
    // Intercept client-initiated eject (Alt+E / Ctrl+E) or local player death
    // latch before FUN_0046ae10:514 can run the local Replay/Continue sequence.
    {
        int pdead = (!IsBadReadPtr((void *)PLAYER_DEAD_VA, 1) && *(BYTE *)PLAYER_DEAD_VA != 0);
        int peject = (!IsBadReadPtr((void *)PLAYER_EJECT_VA, 1) && *(BYTE *)PLAYER_EJECT_VA != 0);
        if (pdead || peject) {
            if (!g_client_spectating && !g_client_eject_anim && peject) {
                BYTE *phys = (ship && !IsBadReadPtr((BYTE *)ship + 4, 4))
                             ? *(BYTE **)((BYTE *)ship + 4) : NULL;
                g_client_saved_p18 = (phys && !IsBadReadPtr(phys + 0x18, 4))
                                     ? *(DWORD *)(phys + 0x18) : 0;
                g_client_eject_anim = 1;
                prev_wing_dead = 1;
                g_client_eject_until = GetTickCount() + 600;
                if (!IsBadWritePtr((void *)PLAYER_DEAD_TIMER_VA, 4)) *(int *)PLAYER_DEAD_TIMER_VA = 0x500;
                if (!IsBadWritePtr((void *)PLAYER_DEAD_PHASE2_VA, 1)) *(BYTE *)PLAYER_DEAD_PHASE2_VA = 0;
                if (!IsBadWritePtr((void *)PLAYER_DEAD_SFX_VA, 1)) *(BYTE *)PLAYER_DEAD_SFX_VA = 0;
                if (!IsBadWritePtr((void *)PLAYER_DEAD_DONE_VA, 1)) *(BYTE *)PLAYER_DEAD_DONE_VA = 0;
                if (!IsBadReadPtr((BYTE *)ship + 0x654, 1)) {
                    void *ctl = *(void **)((BYTE *)ship + 0x650);
                    wc3net_ctl_snap ej;
                    ZeroMemory(&ej, sizeof(ej));
                    if (ctl && !IsBadReadPtr((BYTE *)ctl + 8, 4))
                        ej.throttle = *(int *)((BYTE *)ctl + 8);
                    ej.buttons = (int)WC3N_BTN_CLIENT_EJECT;
                    wc3net_client_offer(&ej);
                }
                log_ts_crit("life", "client pressed eject, playing eject animation and forwarding to host\r\n");
                return rc;
            }
            if (!IsBadWritePtr((void *)PLAYER_DEAD_VA, 1)) *(BYTE *)PLAYER_DEAD_VA = 0;
            if (!IsBadWritePtr((void *)PLAYER_EJECT_VA, 1)) *(BYTE *)PLAYER_EJECT_VA = 0;
            if (!IsBadWritePtr((void *)PLAYER_DEAD_DONE_VA, 1)) *(BYTE *)PLAYER_DEAD_DONE_VA = 0;
        } else if (!IsBadReadPtr((void *)PLAYER_DEAD_DONE_VA, 1) &&
                   *(BYTE *)PLAYER_DEAD_DONE_VA != 0 &&
                   !IsBadWritePtr((void *)PLAYER_DEAD_DONE_VA, 1)) {
            *(BYTE *)PLAYER_DEAD_DONE_VA = 0;
        }
    }
    // Unit 5 increment (c): forbid client-initiated autopilot. FUN_00429470
    // sets DAT_004a2da4 = 1 on local 'A'; clear it here so the client can
    // only enter autopilot when the host does (replicated below).
    if (!IsBadWritePtr((void *)AUTOPILOT_REQ_VA, 1) &&
        *(BYTE *)AUTOPILOT_REQ_VA != 0) {
        *(BYTE *)AUTOPILOT_REQ_VA = 0;
        log_ts("ap", "client local autopilot blocked\r\n");
    }
    // Block client-initiated Nav Map ('N', scancode 0x31); Nav Map opens/closes
    // strictly in lockstep with the host (WC3S_FLAG_NAVMAP).
    if (!IsBadReadPtr((void *)CUR_KEY_SCANCODE_VA, 4) &&
        *(DWORD *)CUR_KEY_SCANCODE_VA == 0x31u &&
        !IsBadWritePtr((void *)CUR_KEY_SCANCODE_VA, 4)) {
        *(DWORD *)CUR_KEY_SCANCODE_VA = 0;
        log_ts("nav", "client local navmap blocked\r\n");
    }
    if (!IsBadReadPtr((BYTE *)ship + 0x654, 1)) {
        void *ctl = *(void **)((BYTE *)ship + 0x650);
        if (ctl && !IsBadReadPtr((BYTE *)ctl + 0x18, 1)) {
            if (g_client_spectating) {
                wc3net_ctl_snap neutral;
                ZeroMemory(&neutral, sizeof(neutral));
                neutral.throttle = *(int *)((BYTE *)ctl + 8);
                wc3net_client_offer(&neutral);
            } else {
                wc3net_ctl_snap s;
                s.buttons = ((BYTE *)ctl)[4];
                s.flags = ((BYTE *)ctl)[5];
                s.throttle = *(int *)((BYTE *)ctl + 8);
                s.lateral = *(int *)((BYTE *)ctl + 0xc);
                s.pitch = *(int *)((BYTE *)ctl + 0x10);
                s.rudder = *(int *)((BYTE *)ctl + 0x14);
                if (g_client_eject_until != 0 &&
                    (int)(g_client_eject_until - GetTickCount()) > 0) {
                    s.buttons |= (int)WC3N_BTN_CLIENT_EJECT;
                }
                wc3net_client_offer(&s);
                // client freshness: one line per 60 calls, same style as remote
                if ((n % 60) == 0) {
                    char buf[128];
                    wsprintfA(buf, "client#%ld p=%d l=%d r=%d t=%d btn=%d fl=%d\r\n",
                              n, s.pitch, s.lateral, s.rudder, s.throttle,
                              s.buttons, s.flags);
                    log_line(buf);
                }
            }
        }
    }
    // own-ship mirror stomp, post-original so it lands on next frame's
    // consumers like the possession write. The local sim keeps running
    // underneath and extrapolates with the stomped velocity between
    // packets, dead reckoning by accident, tolerated in v1. The transform
    // write is range- and teleport-checked (stomp_transform_checked), the
    // velocity rides the same span, range-checked separately.
    {
        wc3s_packet st;
        if (wc3net_state_fetch(&st)) {
            if (st.hdr_flags & WC3S_FLAG_MISSION_END) {
                int end_mode = st.throttle;
                wc3g_state_pkt gf;
                if (end_mode < 2 || end_mode > 10) end_mode = 2;
                if (gf_dbg_snap_is_valid()) {
                    if (end_mode != 3 && end_mode != 4)
                        gf_dbg_snap_restore_mem();
                } else if (wc3net_gf_fetch_state(&gf)) {
                    if (!IsBadWritePtr((void *)GF_FLAGS_VA, sizeof(gf.flags)))
                        CopyMemory((void *)GF_FLAGS_VA, gf.flags, sizeof(gf.flags));
                    if (!IsBadWritePtr((void *)GF_REGS_VA, sizeof(gf.regs))) {
                        int saved_in_sim = gf_read_int(GF_IN_SIM_VA, 0);
                        CopyMemory((void *)GF_REGS_VA, gf.regs, sizeof(gf.regs));
                        gf_write_int(GF_IN_SIM_VA, saved_in_sim);
                    }
                    {
                        char obuf[112];
                        wsprintfA(obuf, "host outcome synced: end_mode=%d r15=%d r16=%d r17=%d\r\n",
                                  end_mode, gf.regs[15], gf.regs[16], gf.regs[17]);
                        log_ts_crit("life", obuf);
                    }
                }
                if (!IsBadWritePtr((void *)SIMSTATE_MODE_VA, 4) &&
                    *(DWORD *)SIMSTATE_MODE_VA == 1) {
                    char ebuf[80];
                    *(DWORD *)SIMSTATE_MODE_VA = (DWORD)end_mode;
                    wsprintfA(ebuf, "host mission end replicated, DAT_004a41c8=%d\r\n",
                              end_mode);
                    log_ts_crit("life", ebuf);
                }
                prev_host_ap = 0;
                prev_host_nav = 0;
                prev_wing_dead = 0;
                ever_had_wingman = 0;
                g_client_spectating = 0;
                g_client_eject_anim = 0;
                g_client_eject_until = 0;
                g_client_in_navmap = 0;
                g_client_navmap_exit_req = 0;
                return rc;
            }
            client_sync_nav_idx(st.nav_idx, NULL);
            int host_ap = (st.hdr_flags & WC3S_FLAG_AUTOPILOT) != 0;
            int host_nav = (st.hdr_flags & WC3S_FLAG_NAVMAP) != 0;
            if (!host_nav && prev_host_nav) {
                prev_host_nav = 0;
                ZeroMemory(g_jump, sizeof(g_jump));
            }
            if (host_ap && !prev_host_ap) {
                if (!IsBadWritePtr((void *)AUTOPILOT_REQ_VA, 1)) {
                    *(BYTE *)AUTOPILOT_REQ_VA = 1;
                    log_ts_crit("ap", "host autopilot replicated to client\r\n");
                }
            } else if (!host_ap && prev_host_ap) {
                // Autopilot just finished: clear teleport history so the new
                // nav-point positions accept immediately on frame 1.
                ZeroMemory(g_jump, sizeof(g_jump));
            }
            prev_host_ap = host_ap;
            // while host is in autopilot/landing cutscene, do not stomp
            // transforms so the client's own cutscene/nav transition runs,
            // and overwrite any pre-autopilot fire buttons with neutral ctl
            if (host_ap) {
                wc3net_ctl_snap neutral;
                ZeroMemory(&neutral, sizeof(neutral));
                neutral.throttle = st.throttle;
                wc3net_client_offer(&neutral);
                return rc;
            }
            if (host_nav) {
                client_offer_navmap_keepalive(st.throttle);
                if (!g_client_in_navmap && !g_client_eject_anim &&
                    (IsBadReadPtr((void *)AUTOPILOT_ACT_VA, 1) ||
                     *(BYTE *)AUTOPILOT_ACT_VA == 0) &&
                    !IsBadWritePtr((void *)CUR_KEY_SCANCODE_VA, 4)) {
                    *(DWORD *)CUR_KEY_SCANCODE_VA = 0x31u;
                }
                prev_host_nav = 1;
                return rc;
            }
            int wing_dead = (st.wing_flags & WC3S_WING_DEAD) != 0;
            if (!wing_dead && st.idx_wing != 0xff)
                ever_had_wingman = 1;
            if (wing_dead && !prev_wing_dead) {
                prev_wing_dead = 1;
                g_client_eject_until = 0;
                if (ever_had_wingman && !g_client_eject_anim &&
                    (IsBadReadPtr((void *)AUTOPILOT_ACT_VA, 1) ||
                     *(BYTE *)AUTOPILOT_ACT_VA == 0)) {
                    BYTE *phys = (ship && !IsBadReadPtr((BYTE *)ship + 4, 4))
                                 ? *(BYTE **)((BYTE *)ship + 4) : NULL;
                    g_client_saved_p18 = (phys && !IsBadReadPtr(phys + 0x18, 4))
                                         ? *(DWORD *)(phys + 0x18) : 0;
                    g_client_eject_anim = 2;
                    if (!IsBadWritePtr((void *)PLAYER_DEAD_TIMER_VA, 4)) *(int *)PLAYER_DEAD_TIMER_VA = 0x500;
                    if (!IsBadWritePtr((void *)PLAYER_DEAD_PHASE2_VA, 1)) *(BYTE *)PLAYER_DEAD_PHASE2_VA = 0;
                    if (!IsBadWritePtr((void *)PLAYER_DEAD_SFX_VA, 1)) *(BYTE *)PLAYER_DEAD_SFX_VA = 0;
                    if (!IsBadWritePtr((void *)PLAYER_DEAD_DONE_VA, 1)) *(BYTE *)PLAYER_DEAD_DONE_VA = 0;
                    if (!IsBadWritePtr((void *)PLAYER_EJECT_VA, 1)) *(BYTE *)PLAYER_EJECT_VA = 0;
                    if (!IsBadWritePtr((void *)PLAYER_DEAD_VA, 1)) *(BYTE *)PLAYER_DEAD_VA = 1;
                    log_ts_crit("life", "client wingman destroyed on host, playing death animation before Blair chase cam\r\n");
                    return rc;
                }
                ZeroMemory(g_jump, sizeof(g_jump));
                log_ts_crit("life", "client wingman destroyed/ejected on host, switching to Blair external chase cam\r\n");
            }
            g_client_spectating = wing_dead;
            prev_wing_dead = wing_dead;
            if (!IsBadReadPtr(ship, 8)) {
                BYTE *phys = *(BYTE **)((BYTE *)ship + 4);
                if (phys && !IsBadWritePtr((BYTE *)phys + 0x28, 0x3c)) {
                    int pos[3], ori[9];
                    int i;
                    // field-wise copy, never an address into the packed packet
                    for (i = 0; i < 3; i++) pos[i] = st.pos[i];
                    for (i = 0; i < 9; i++) ori[i] = st.orient[i];
                    if (stomp_transform_checked(phys, pos, ori)) {
                        int vel_ok = 1;
                        for (i = 0; i < 3; i++)
                            if (st.vel[i] >= VEL_LIMIT || st.vel[i] <= -VEL_LIMIT)
                                vel_ok = 0;
                        if (vel_ok)
                            for (i = 0; i < 3; i++)
                                *(int *)((BYTE *)phys + 0x58 + 4 * i) = st.vel[i];
                    }
                    if (wing_dead && !IsBadWritePtr((BYTE *)phys + 0x7d, 1))
                        phys[0x7d] = 0;
                    apply_wing_health(phys, &st);
                }
            }
            if (!IsBadReadPtr((BYTE *)ship + 0x654, 1)) {
                BYTE *ctl = *(BYTE **)((BYTE *)ship + 0x650);
                if (ctl && !IsBadWritePtr((BYTE *)ctl + 8, 4)) {
                    *(int *)((BYTE *)ctl + 8) = st.throttle;
                    if (wing_dead) {
                        int blair_ab = (st.idx_blair < st.obj_count &&
                                        st.idx_blair < WC3S_OBJ_MAX &&
                                        (st.objects[st.idx_blair].flags & 0x08) != 0);
                        ctl[4] &= (BYTE)~(0x01 | 0x04);
                        ctl[5] &= (BYTE)~(0x10 | 0x80);
                        if (blair_ab) ctl[5] |= 0x10;
                    }
                }
            }
            if (wing_dead &&
                (IsBadReadPtr((void *)AUTOPILOT_ACT_VA, 1) ||
                 *(BYTE *)AUTOPILOT_ACT_VA == 0)) {
                client_ensure_chase_cam();
            }
            // world objects, Unit 5 increment (a)
            apply_world(&st, ship);
            // stomp freshness: one line per 60 calls, timestamped for
            // direct latency measurement against the mirror lines
            if ((n % 60) == 0) {
                char buf[96];
                wsprintfA(buf, "x=%d y=%d z=%d t=%d\r\n",
                          st.pos[0], st.pos[1], st.pos[2], st.throttle);
                log_ts("stomp", buf);
            }
        }
    }
    return rc;
}

// ---- Unit 6: Out-of-mission Victory / gameflow / cutscene / savegame sync ----

typedef void (__attribute__((fastcall)) *screen_tick_fn)(void *);
typedef void (__attribute__((fastcall)) *screen_slam_fn)(void *);
typedef void (__cdecl *ui_fill_rect_fn)(int x1, int y1, int x2, int y2, int color);
typedef unsigned int (__cdecl *ui_draw_line_fn)(void *vp, int x1, int y1, int x2, int y2, int mode, int color);
typedef void (__cdecl *ui_draw_text_fn)(void *vp, int x, int y, void *font, const char *str, void *color_tbl);
typedef int  (__cdecl *ui_font_height_fn)(void *font);
typedef int  (__cdecl *ui_font_char_w_fn)(void *font, unsigned int ch);
typedef void (__attribute__((thiscall)) *gump_evt_fn)(void *, void *);
typedef void (__attribute__((thiscall)) *branch_choose_fn)(void *, void *);
typedef unsigned int (__attribute__((thiscall)) *movie_frame_fn)(void *, unsigned int);
typedef void (__cdecl *poll_input_fn)(void);
typedef int  (__cdecl *gf_exec_fn)(int ip, int steps);
typedef int  (__cdecl *gf_load_save_fn)(int slot);
typedef int  (__cdecl *gf_cd_switch_fn)(int cd);
typedef void (__attribute__((thiscall)) *gf_music_cd_fn)(void *, unsigned int);
typedef void (__cdecl *gf_music_sync_fn)(void);
typedef void *(__attribute__((thiscall)) *gf_find_gump_fn)(void *room_list, int id);

static screen_tick_fn   g_screen_tick_orig;
static screen_slam_fn   g_screen_slam_orig;
static volatile int     g_slam_hooked;
static gump_evt_fn      g_gump_rel1_orig;
static gump_evt_fn      g_gump_rel2_orig;
static gump_evt_fn      g_gump_rel3_orig;
static gump_evt_fn      g_gump_rel4_orig;
static gump_evt_fn      g_gump_rel5_orig;
static gump_evt_fn      g_gump_rel6_orig;
static gump_evt_fn      g_gump_key5_orig;
static gump_evt_fn      g_deck_key_orig;
static gump_evt_fn      g_branch_move_orig;
static branch_choose_fn g_branch_choose_orig;
static gump_evt_fn      g_branch_key_orig;
static movie_frame_fn   g_movie_frame_orig;
static poll_input_fn    g_wc3en_poll_orig;
static BYTE            *g_wc3en_base;
static gump_evt_fn      g_saveload_act_orig;
static gump_evt_fn      g_simlist_click_orig;
static gump_evt_fn      g_simlist_cmd_orig;

// Host event latch (game thread -> gf_pump_thread)
static CRITICAL_SECTION g_gf_evt_lock;
static int              g_gf_evt_lock_init;
static unsigned long    g_gf_evt_seq;
static unsigned char    g_gf_evt_type;
static int              g_gf_evt_arg0;
static int              g_gf_evt_arg1;
static unsigned char    g_gf_branch_seq;         // 1..15 cyclic counter for redundant branch channel
static unsigned char    g_gf_branch_choice;      // latest host logical dialogue branch choice (0/1)

// Client last-applied event seq & modal/movie sync latches
static unsigned long    g_gf_last_evt_seq;
static unsigned char    g_gf_last_branch_seq;    // last seen 4-bit branch seq from host_flags
static int              g_gf_client_synced;
static int              g_gf_pending_evt;      // 1 when a CLICK or LOAD event is queued for replay
static unsigned char    g_gf_pending_evt_type;
static int              g_gf_pending_evt_arg0;
static int              g_gf_pending_evt_arg1;
static DWORD            g_gf_pending_evt_tick; // timestamp when queued (for room-transition retry timeout)
static volatile int     g_gf_in_replay;        // narrow guard during direct replay call
static volatile int     g_gf_in_exec_script;   // 1 while client is inside exec_script(ip,-1)
static int              g_gf_pending_branch;   // 1 when host picked a branch choice
static int              g_gf_pending_branch_choice;
static int              g_gf_pending_skip;     // 1 when host skipped an FMV movie segment
static DWORD            g_last_exit_prompt_tick;
static volatile int     g_host_in_hd_movie;    // 1 while host is inside wc3en HD movie loop
static int              g_host_hd_skip_sent;   // debounce WC3G_EVT_SKIP per HD movie segment
static void            *g_host_hd_last_obj;    // last seen *DAT_100549b8 on host
static int              g_cli_hd_seen_host;    // 1 once client saw host in current HD movie
static void            *g_cli_hd_last_obj;     // last seen *DAT_100549b8 on client
static int              g_cli_hd_skip_latch;   // 1 when client must skip current HD movie segment
static DWORD            g_cli_hd_start_tick;   // when client entered current HD movie segment

static int gf_read_int(DWORD va, int def)
{
    if (IsBadReadPtr((void *)va, 4)) return def;
    return *(int *)va;
}

static void gf_write_int(DWORD va, int v)
{
    if (!IsBadWritePtr((void *)va, 4)) *(int *)va = v;
}

// Locate the active dialogue branch gump (vtable 0x49b8a0).
// FUN_00482530 -> FUN_00482400 pushes a 0x4c-byte TightDispatch wrapper onto
// DAT_004b2000[DAT_004a7b4c] and attaches the actual 0x49b8a0 branch gump as
// its child (wrapper+0x18 = first child, child+4 = next sibling per FUN_00481490).
static BYTE *gf_top_branch_modal(void)
{
    int top = gf_read_int(GF_MODAL_TOP_VA, -1);
    if (top >= 0 && top < 32 &&
        !IsBadReadPtr((void *)(GF_MODAL_STACK_VA + top * 4), 4)) {
        BYTE *base = *(BYTE **)(GF_MODAL_STACK_VA + top * 4);
        if (base && !IsBadReadPtr(base, 0x20)) {
            if (!IsBadReadPtr(base, 0x68) && *(DWORD *)base == GF_BRANCH_VT_VA)
                return base;
            BYTE *child = *(BYTE **)(base + 0x18);
            int guard;
            for (guard = 0; child && guard < 16; guard++) {
                if (IsBadReadPtr(child, 0x68)) break;
                if (*(DWORD *)child == GF_BRANCH_VT_VA)
                    return child;
                child = *(BYTE **)(child + 4);
            }
        }
    }
    return NULL;
}

// Check whether wc3en.dll currently has an actively playing LibVlc_Movie.
// Note: FUN_10032a10 does NOT clear DAT_100549b8 to NULL on normal exit;
// instead FUN_1003a0e0 sets *(BYTE *)(node+0x3c) = 1 when playing and
// FUN_100320b0 sets *(BYTE *)(node+0x3c) = 0 on all linked nodes (*node)
// when stopped. *(void **)(node+0x0c) is the unique libvlc_media_player_t*.
static int gf_wc3en_hd_movie_playing(void **out_token)
{
    BYTE *base = g_wc3en_base;
    BYTE *node;
    int guard;
    if (out_token) *out_token = NULL;
    if (!base || IsBadReadPtr(base + WC3EN_HD_MOVIE_OFF, 4)) return 0;
    node = *(BYTE **)(base + WC3EN_HD_MOVIE_OFF);
    for (guard = 0; node && guard < 16; guard++) {
        if (IsBadReadPtr(node, 0x40)) break;
        if (node[0x3c] != 0) {
            if (out_token) {
                void *mp = *(void **)(node + 0x0c);
                *out_token = mp ? mp : (void *)node;
            }
            return 1;
        }
        node = *(BYTE **)node;
    }
    return 0;
}

static unsigned short gf_read_host_flags(void)
{
    unsigned short fl = 0;
    if (gf_read_int(GF_MOVIE_CTX_VA, 0) != 0)
        fl |= WC3G_HFLAG_IN_MOVIE;
    if (gf_top_branch_modal() != NULL)
        fl |= WC3G_HFLAG_IN_BRANCH;
    if (gf_wc3en_hd_movie_playing(NULL))
        fl |= WC3G_HFLAG_IN_HD_MOV;
    if (gf_read_int(GF_SCRIPT_ACT_VA, -1) == 0 &&
        !IsBadReadPtr((void *)GF_DECK_ACTIVE_VA, 1) &&
        *(BYTE *)GF_DECK_ACTIVE_VA != 0 &&
        !(fl & (WC3G_HFLAG_IN_MOVIE | WC3G_HFLAG_IN_HD_MOV))) {
        fl |= WC3G_HFLAG_DECK_ACTIVE;
    }
    if (host_mission_active()) {
        if (gf_read_int(GF_IN_SIM_VA, 0) != 0)
            fl |= WC3G_HFLAG_IN_SIM;
        else
            fl |= WC3G_HFLAG_IN_MISSION;
    }
    if (g_gf_branch_seq != 0) {
        fl |= (unsigned short)(((unsigned short)(g_gf_branch_seq & 0x0f)) << WC3G_HFLAG_BSEQ_SHIFT);
        if (g_gf_branch_choice)
            fl |= WC3G_HFLAG_BCHOICE_BIT;
    }
    return fl;
}

static void gf_build_state_pkt(wc3g_state_pkt *pkt)
{
    static unsigned long gseq;
    ZeroMemory(pkt, sizeof(*pkt));
    pkt->magic = WC3G_MAGIC;
    pkt->msg_type = WC3G_MSG_STATE;
    pkt->reserved = gf_read_host_flags();
    pkt->seq = ++gseq;
    EnterCriticalSection(&g_gf_evt_lock);
    pkt->evt_seq  = g_gf_evt_seq;
    pkt->evt_type = g_gf_evt_type;
    pkt->evt_arg0 = g_gf_evt_arg0;
    pkt->evt_arg1 = g_gf_evt_arg1;
    LeaveCriticalSection(&g_gf_evt_lock);
    pkt->script_act = gf_read_int(GF_SCRIPT_ACT_VA, 0);
    pkt->scene_id   = gf_read_int(GF_SCENE_ID_VA, 0);
    pkt->mission_id = gf_read_int(GF_MISSION_ID_VA, 0);
    pkt->room_id    = gf_read_int(GF_ROOM_ID_VA, 0);
    if (!IsBadReadPtr((void *)GF_FLAGS_VA, sizeof(pkt->flags)))
        CopyMemory(pkt->flags, (void *)GF_FLAGS_VA, sizeof(pkt->flags));
    if (!IsBadReadPtr((void *)GF_REGS_VA, sizeof(pkt->regs)))
        CopyMemory(pkt->regs, (void *)GF_REGS_VA, sizeof(pkt->regs));
}

static void gf_push_state_now(void)
{
    wc3g_state_pkt pkt;
    if (!g_gf_evt_lock_init) return;
    gf_build_state_pkt(&pkt);
    wc3net_gf_send_state(&pkt);
}

static void gf_record_host_event(unsigned char type, int arg0, int arg1)
{
    char buf[96];
    wc3g_state_pkt pkt;
    if (!g_gf_evt_lock_init) return;
    EnterCriticalSection(&g_gf_evt_lock);
    g_gf_evt_seq++;
    g_gf_evt_type = type;
    g_gf_evt_arg0 = arg0;
    g_gf_evt_arg1 = arg1;
    if (type == WC3G_EVT_BRANCH) {
        g_gf_branch_seq = (unsigned char)((g_gf_branch_seq % 15) + 1);
        g_gf_branch_choice = arg0 ? 1 : 0;
    }
    LeaveCriticalSection(&g_gf_evt_lock);
    // Push an immediate state packet so discrete events (click, skip, branch)
    // do not wait up to 100 ms for the next gf_pump_thread tick.
    gf_build_state_pkt(&pkt);
    wc3net_gf_send_state(&pkt);
    wsprintfA(buf, "evt#%lu type=%u a0=%d a1=%d act=%d sc=%d rm=%d\r\n",
              g_gf_evt_seq, (unsigned)type, arg0, arg1,
              gf_read_int(GF_SCRIPT_ACT_VA, -1),
              gf_read_int(GF_SCENE_ID_VA, -1),
              gf_read_int(GF_ROOM_ID_VA, -1));
    log_ts_crit("gf_host", buf);
}

// Resolve savegame directory: wc3w_en uses %APPDATA%\wc3w_en when UAC_AWARE!=0,
// otherwise falls back to the game directory.
static void gf_get_save_dir(char *out, int cap)
{
    char appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    if (n > 0 && n < MAX_PATH - 16) {
        char cand[MAX_PATH];
        DWORD attr;
        wsprintfA(cand, "%s\\wc3w_en", appdata);
        attr = GetFileAttributesA(cand);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            // If UAC_AWARE is active on this machine, wc3en creates %APPDATA%\wc3w_en
            // lazily on first save; create it proactively if needed when APPDATA exists
            // and wc3en.dll is loaded.
            if (GetModuleHandleA("wc3en.dll") != NULL) {
                CreateDirectoryA(cand, NULL);
                attr = GetFileAttributesA(cand);
            }
        }
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            lstrcpynA(out, cand, cap);
            return;
        }
    }
    // Fallback: DLL / game directory
    {
        HMODULE self = NULL;
        char *slash;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&gf_get_save_dir, &self);
        GetModuleFileNameA(self, out, cap);
        slash = out;
        for (char *p = out; *p; p++)
            if (*p == '\\' || *p == '/') slash = p;
        *slash = 0;
    }
}

static int gf_host_mirror_one_save(const char *dir, const char *fname)
{
    char full[MAX_PATH];
    HANDLE h;
    int ok = 0;
    wsprintfA(full, "%s\\%s", dir, fname);
    h = CreateFileA(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        unsigned char *sbuf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, WC3G_SAVE_MAX);
        DWORD sz = GetFileSize(h, NULL), rd = 0;
        if (sbuf && sz > 0 && sz <= WC3G_SAVE_MAX && ReadFile(h, sbuf, sz, &rd, NULL) && rd > 0) {
            char lbuf[128];
            ok = wc3net_gf_send_save_file(fname, sbuf, rd);
            wsprintfA(lbuf, "mirrored save %s (%lu B) ack=%d\r\n",
                      fname, (unsigned long)rd, ok);
            log_ts_crit("gf_save", lbuf);
        }
        if (sbuf) HeapFree(GetProcessHeap(), 0, sbuf);
        CloseHandle(h);
    }
    return ok;
}

// Host savegame watcher: scans save directory every 1 s for any new/modified
// *.WSG files (e.g. 00000000.WSG, WSG_NDX.WSG) and mirrors them to the client.
#define SAVE_TRACK_MAX 32
static struct {
    char     name[24];
    FILETIME mtime;
    DWORD    size;
} g_save_track[SAVE_TRACK_MAX];
static int g_save_track_count;
static int g_save_track_primed;

static void gf_host_scan_saves(void)
{
    char dir[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE hf;

    gf_get_save_dir(dir, sizeof(dir));
    wsprintfA(pat, "%s\\*.WSG", dir);
    hf = FindFirstFileA(pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        int i, found = -1, changed = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.nFileSizeLow == 0 || fd.nFileSizeLow > WC3G_SAVE_MAX) continue;
        // Skip temporary mission-restart slot 00000102.WSG
        if (lstrcmpiA(fd.cFileName, "00000102.WSG") == 0) continue;
        for (i = 0; i < g_save_track_count; i++) {
            if (lstrcmpiA(g_save_track[i].name, fd.cFileName) == 0) {
                found = i;
                if (CompareFileTime(&g_save_track[i].mtime, &fd.ftLastWriteTime) != 0 ||
                    g_save_track[i].size != fd.nFileSizeLow) {
                    g_save_track[i].mtime = fd.ftLastWriteTime;
                    g_save_track[i].size = fd.nFileSizeLow;
                    changed = 1;
                }
                break;
            }
        }
        if (found == -1 && g_save_track_count < SAVE_TRACK_MAX) {
            lstrcpynA(g_save_track[g_save_track_count].name, fd.cFileName, 24);
            g_save_track[g_save_track_count].mtime = fd.ftLastWriteTime;
            g_save_track[g_save_track_count].size = fd.nFileSizeLow;
            g_save_track_count++;
            changed = g_save_track_primed; // only send files written after startup
        }
        if (changed && wc3net_gf_client_present()) {
            gf_host_mirror_one_save(dir, fd.cFileName);
        }
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
    g_save_track_primed = 1;
}

// Client savegame writer: flushes any completed save file from host to disk.
static void gf_client_flush_saves(void)
{
    char fname[24];
    unsigned char *sbuf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, WC3G_SAVE_MAX);
    unsigned long sz = 0;
    if (!sbuf) return;
    while (wc3net_gf_pop_save_file(fname, sbuf, &sz)) {
        char dir[MAX_PATH], full[MAX_PATH];
        HANDLE h;
        gf_get_save_dir(dir, sizeof(dir));
        wsprintfA(full, "%s\\%s", dir, fname);
        h = CreateFileA(full, GENERIC_WRITE, FILE_SHARE_READ,
                        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(h, sbuf, sz, &wr, NULL);
            CloseHandle(h);
            {
                char lbuf[128];
                wsprintfA(lbuf, "wrote mirrored save %s (%lu B)\r\n",
                          fname, (unsigned long)wr);
                log_ts_crit("gf_save", lbuf);
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, sbuf);
}

// Background thread:
//   Host:   broadcasts WC3G_MSG_STATE at 10 Hz + scans for new savegames at 1 Hz.
//   Client: sends WC3G_MSG_HELLO at 10 Hz + flushes mirrored savegames to disk.
static DWORD WINAPI gf_pump_thread(LPVOID unused)
{
    DWORD last_save_scan = 0;
    DWORD last_diag = 0;
    (void)unused;
    log_ts("gf_pump", g_mode_client ? "started (client)\r\n" : "started (host)\r\n");
    for (;;) {
        Sleep(100);
        if (gf_dbg_snap_is_valid()) {
            DWORD sim = (!IsBadReadPtr((void *)SIMSTATE_VA, 4)) ? *(DWORD *)SIMSTATE_VA : 0;
            int deck = (!IsBadReadPtr((void *)GF_DECK_ACTIVE_VA, 1)) ? (int)(*(BYTE *)GF_DECK_ACTIVE_VA) : 1;
            if (sim != 0 || deck == 0)
                gf_dbg_snap_arm();
        }
        if (g_mode_client) {
            int act = gf_read_int(GF_SCRIPT_ACT_VA, 0);
            int sc  = gf_read_int(GF_SCENE_ID_VA, 0);
            int rm  = gf_read_int(GF_ROOM_ID_VA, 0);
            wc3net_gf_send_hello(act, sc, rm);
            gf_client_flush_saves();
            // During the autopilot cutscene (DAT_004a3524 != 0) or Nav Map modal,
            // FUN_0046ae10/FUN_00445150 pause PLYR::update (plyr_relay). Offer
            // neutral zero-button input keepalives so tx_thread keeps feeding the
            // host and the host wingman AI never re-engages on timeout.
            if (g_client_in_navmap ||
                (!IsBadReadPtr((void *)AUTOPILOT_ACT_VA, 1) &&
                 *(BYTE *)AUTOPILOT_ACT_VA != 0)) {
                wc3net_ctl_snap neutral;
                ZeroMemory(&neutral, sizeof(neutral));
                if (!IsBadReadPtr((void *)PLAYER_GLOBAL_VA, 4)) {
                    BYTE *phys = *(BYTE **)PLAYER_GLOBAL_VA;
                    if (phys && !IsBadReadPtr(phys + 0x94, 4)) {
                        BYTE *ctl = ship_ctl_ptr(*(BYTE **)(phys + 0x94));
                        if (ctl && !IsBadReadPtr(ctl + 8, 4))
                            neutral.throttle = *(int *)(ctl + 8);
                    }
                }
                if (g_client_eject_anim == 1 && g_client_eject_until != 0 &&
                    (int)(g_client_eject_until - GetTickCount()) > 0) {
                    neutral.buttons = (int)WC3N_BTN_CLIENT_EJECT;
                }
                wc3net_client_offer(&neutral);
            }
        } else {
            wc3g_state_pkt pkt;
            gf_build_state_pkt(&pkt);
            wc3net_gf_send_state(&pkt);

            if (GetTickCount() - last_diag >= 5000 && pkt.script_act != 5) {
                char dbuf[112];
                last_diag = GetTickCount();
                wsprintfA(dbuf, "act=%d sc=%d misn=%d rm=%d evt=%lu cli=%d\r\n",
                          pkt.script_act, pkt.scene_id, pkt.mission_id, pkt.room_id,
                          pkt.evt_seq, wc3net_gf_client_present());
                log_ts("gf_host", dbuf);
            }

            if (GetTickCount() - last_save_scan >= 1000) {
                last_save_scan = GetTickCount();
                gf_host_scan_saves();
            }
        }
    }
    return 0;
}

// Client helper: latch discrete events from a received WC3G_MSG_STATE packet.
// Returns non-zero if a new non-branch/non-skip event (CLICK or LOAD) needs
// to be executed by screen_tick_relay.
static int gf_client_latch_events(const wc3g_state_pkt *st)
{
    unsigned char bseq = (unsigned char)((st->reserved & WC3G_HFLAG_BSEQ_MASK) >> WC3G_HFLAG_BSEQ_SHIFT);
    if (!g_gf_client_synced) {
        char buf[128];
        g_gf_client_synced = 1;
        g_gf_last_evt_seq = st->evt_seq;
        g_gf_last_branch_seq = bseq;
        wsprintfA(buf, "synced with host: act=%d sc=%d misn=%d rm=%d\r\n",
                  st->script_act, st->scene_id, st->mission_id, st->room_id);
        log_ts_crit("gf_cli", buf);
        return 0;
    }
    // Redundant dialogue branch channel: survives even if host immediately
    // presses ESC (overwriting evt_type with WC3G_EVT_SKIP) or drops 1 UDP pkt.
    if (bseq != 0 && bseq != g_gf_last_branch_seq) {
        g_gf_last_branch_seq = bseq;
        g_gf_pending_branch = 1;
        g_gf_pending_branch_choice = (st->reserved & WC3G_HFLAG_BCHOICE_BIT) ? 1 : 0;
        log_ts("gf_cli", "latched host dialogue branch choice (flag channel)\r\n");
    }
    if (st->evt_seq == g_gf_last_evt_seq)
        return g_gf_pending_evt;
    if (st->evt_type == WC3G_EVT_BRANCH) {
        g_gf_last_evt_seq = st->evt_seq;
        if (bseq == 0) {
            g_gf_pending_branch = 1;
            g_gf_pending_branch_choice = st->evt_arg0 ? 1 : 0;
            log_ts("gf_cli", "latched host dialogue branch choice\r\n");
        }
        return g_gf_pending_evt;
    }
    if (st->evt_type == WC3G_EVT_SKIP) {
        g_gf_last_evt_seq = st->evt_seq;
        g_gf_pending_skip = 1;
        log_ts("gf_cli", "latched host movie skip\r\n");
        return g_gf_pending_evt;
    }
    if (st->evt_type == WC3G_EVT_REPLAY) {
        g_gf_last_evt_seq = st->evt_seq;
        g_gf_pending_replay = 1;
        g_gf_pending_replay_choice = st->evt_arg0;
        log_ts_crit("gf_cli", "latched host replay/continue choice\r\n");
        return g_gf_pending_evt;
    }
    // Queue CLICK, LOAD, MISSION (simulator launch), or DEBUG_MISN so it is
    // never dropped if the client is currently inside exec_script, in a room
    // transition, or before the new room's gump list is loaded.
    if (st->evt_type == WC3G_EVT_CLICK || st->evt_type == WC3G_EVT_LOAD ||
        st->evt_type == WC3G_EVT_MISSION || st->evt_type == WC3G_EVT_DEBUG_MISN) {
        g_gf_last_evt_seq = st->evt_seq;
        g_gf_pending_evt = 1;
        g_gf_pending_evt_type = st->evt_type;
        g_gf_pending_evt_arg0 = st->evt_arg0;
        g_gf_pending_evt_arg1 = st->evt_arg1;
        g_gf_pending_evt_tick = GetTickCount();
    } else {
        g_gf_last_evt_seq = st->evt_seq;
    }
    return g_gf_pending_evt;
}

// Clear any active input-capture gump (e.g. "ENTER CALLSIGN" input widget)
// before forcing a scene/save transition on the client so FUN_004821a0 never
// dispatches into a freed gump pointer after the room/scene is torn down.
static void gf_clear_input_capture(void)
{
    gf_write_int(GF_CAPTURED_GUMP_VA, 0);
    gf_write_int(GF_CALLSIGN_DONE_VA, 1);
}

static void gf_ensure_pilots_alive(int w1, int w2, int w3)
{
    int ids[3] = { w1, w2, w3 };
    int i;
    if (IsBadWritePtr((void *)GF_PILOT_ALIVE_VA, 12 * 4)) return;
    for (i = 0; i < 3; i++) {
        int p = ids[i];
        if (p > 0 && p < 12) {
            int *tbl = (int *)GF_PILOT_ALIVE_VA;
            if (tbl[p] == 0) tbl[p] = 8; // default live pilot rating per FUN_00406c30
        }
    }
}

static void gf_get_game_dir(char *out, int cap)
{
    HMODULE self = NULL;
    char *slash = NULL;
    char *p;
    if (cap <= 0) return;
    out[0] = 0;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&gf_get_game_dir, &self) || !self)
        return;
    if (!GetModuleFileNameA(self, out, cap)) {
        out[0] = 0;
        return;
    }
    for (p = out; *p; p++) {
        if (*p == '\\' || *p == '/') slash = p;
    }
    if (slash) *slash = 0;
}

static void gf_ensure_cd_mounted(int need_cd)
{
    int cur_cd = gf_read_int(GF_CUR_CD_VA, 1);
    char game_dir[MAX_PATH];
    char buf[160];
    int rc;

    if (need_cd < 1 || need_cd > 4) need_cd = 1;
    if (cur_cd == need_cd) return;

    gf_get_game_dir(game_dir, sizeof(game_dir));
    if (game_dir[0])
        SetCurrentDirectoryA(game_dir);

    rc = ((gf_cd_switch_fn)GF_CD_SWITCH_VA)(need_cd);
    if (rc > 0) {
        if (!IsBadReadPtr((void *)GF_MUSIC_TRACK_VA, 1)) {
            ((gf_music_cd_fn)GF_MUSIC_CD_VA)((void *)GF_MUSIC_MGR_VA,
                                             (unsigned int)(*(BYTE *)GF_MUSIC_TRACK_VA));
        }
        ((gf_music_sync_fn)GF_MUSIC_SYNC_VA)();
        wsprintfA(buf, "mounted CD%d (prev=%d rc=%d)\r\n", need_cd, cur_cd, rc);
        log_ts_crit("gf", buf);
    } else {
        int fallback_cd = (cur_cd >= 1 && cur_cd <= 4) ? cur_cd : 1;
        int fb_rc = ((gf_cd_switch_fn)GF_CD_SWITCH_VA)(fallback_cd);
        if (fb_rc > 0) {
            if (!IsBadReadPtr((void *)GF_MUSIC_TRACK_VA, 1)) {
                ((gf_music_cd_fn)GF_MUSIC_CD_VA)((void *)GF_MUSIC_MGR_VA,
                                                 (unsigned int)(*(BYTE *)GF_MUSIC_TRACK_VA));
            }
            ((gf_music_sync_fn)GF_MUSIC_SYNC_VA)();
        }
        wsprintfA(buf, "CD%d mount failed (prev=%d rc=%d fb_rc=%d dir=%s)\r\n",
                  need_cd, cur_cd, rc, fb_rc, game_dir);
        log_ts_crit("gf", buf);
    }
}

static struct {
    volatile int  valid;
    volatile int  armed;
    int           scene_id;
    int           mission_id;
    int           room_id;
    int           cur_cd;
    unsigned char flags[256];
    int           regs[96];
    int           pilots[12];
} g_dbg_snap;

static int gf_dbg_snap_is_valid(void)
{
    return g_dbg_snap.valid != 0;
}

static void gf_dbg_snap_arm(void)
{
    if (g_dbg_snap.valid && !g_dbg_snap.armed)
        g_dbg_snap.armed = 1;
}

static void gf_dbg_snap_save(void)
{
    if (g_dbg_snap.valid) return;
    g_dbg_snap.scene_id   = gf_read_int(GF_SCENE_ID_VA, 0);
    g_dbg_snap.mission_id = gf_read_int(GF_MISSION_ID_VA, 0);
    g_dbg_snap.room_id    = gf_read_int(GF_ROOM_ID_VA, 3);
    g_dbg_snap.cur_cd     = gf_read_int(GF_CUR_CD_VA, 1);
    if (g_dbg_snap.cur_cd < 1 || g_dbg_snap.cur_cd > 4) g_dbg_snap.cur_cd = 1;
    ZeroMemory(g_dbg_snap.flags, sizeof(g_dbg_snap.flags));
    ZeroMemory(g_dbg_snap.regs, sizeof(g_dbg_snap.regs));
    ZeroMemory(g_dbg_snap.pilots, sizeof(g_dbg_snap.pilots));
    if (!IsBadReadPtr((void *)GF_FLAGS_VA, sizeof(g_dbg_snap.flags)))
        CopyMemory(g_dbg_snap.flags, (void *)GF_FLAGS_VA, sizeof(g_dbg_snap.flags));
    if (!IsBadReadPtr((void *)GF_REGS_VA, sizeof(g_dbg_snap.regs)))
        CopyMemory(g_dbg_snap.regs, (void *)GF_REGS_VA, sizeof(g_dbg_snap.regs));
    if (!IsBadReadPtr((void *)GF_PILOT_ALIVE_VA, sizeof(g_dbg_snap.pilots)))
        CopyMemory(g_dbg_snap.pilots, (void *)GF_PILOT_ALIVE_VA, sizeof(g_dbg_snap.pilots));
    g_dbg_snap.armed = 0;
    g_dbg_snap.valid = 1;
    {
        char buf[128];
        wsprintfA(buf, "saved pre-debug snapshot: sc=%d misn=%d rm=%d cd=%d\r\n",
                  g_dbg_snap.scene_id, g_dbg_snap.mission_id, g_dbg_snap.room_id, g_dbg_snap.cur_cd);
        log_ts_crit("gf", buf);
    }
}

static void gf_dbg_snap_restore_mem(void)
{
    if (!g_dbg_snap.valid) return;
    if (!IsBadWritePtr((void *)GF_FLAGS_VA, sizeof(g_dbg_snap.flags)))
        CopyMemory((void *)GF_FLAGS_VA, g_dbg_snap.flags, sizeof(g_dbg_snap.flags));
    if (!IsBadWritePtr((void *)GF_REGS_VA, sizeof(g_dbg_snap.regs)))
        CopyMemory((void *)GF_REGS_VA, g_dbg_snap.regs, sizeof(g_dbg_snap.regs));
    if (!IsBadWritePtr((void *)GF_PILOT_ALIVE_VA, sizeof(g_dbg_snap.pilots)))
        CopyMemory((void *)GF_PILOT_ALIVE_VA, g_dbg_snap.pilots, sizeof(g_dbg_snap.pilots));
    gf_write_int(GF_IN_SIM_VA, 0);
    gf_write_int(GF_SCENE_ID_VA, g_dbg_snap.scene_id);
    gf_write_int(GF_MISSION_ID_VA, g_dbg_snap.mission_id);
    gf_write_int(GF_ROOM_ID_VA, g_dbg_snap.room_id);
    gf_write_int(GF_NEXT_ACT_VA, 4);
}

// ---- Point 2: Host Debug Mission Select Menu & Flag Submenu ----

#define MMENU_PER_PAGE 14

enum {
    MROW_SHIP = 0,
    MROW_W1SHIP,
    MROW_W1PILOT,
    MROW_W2PILOT,
    MROW_W3PILOT,
    MROW_LOADOUT,
    MROW_RREG1,
    MROW_RREG2
};

static struct {
    int   open;         // 0=closed, 1=mission list, 2=flag submenu
    int   tab;          // 0=All, 1=Special, 2=Sim
    int   page;
    int   sel;          // highlighted list index or submenu row
    int   misn_idx;     // index into g_wc3_missions[]
    int   ship;         // regs[22]
    int   w1_ship;      // regs[24]
    int   w1_pilot;     // regs[23]
    int   w2_pilot;     // regs[58]
    int   w3_pilot;     // regs[59]
    int   builtin;      // regs[46]
    int   rreg1_val;
    int   rreg2_val;
    int   ui_stride;
    int   ui_height;
    BYTE *blocked_gump; // root gump whose +0x3c disable flag we temporarily set
    int   prev_lmb;
    int   prev_rmb;
    BYTE  prev_keys[128];
    DWORD esc_swallow_until;
} g_mmenu = { 0, 0, 0, 0, 0, -1, -1, 0, 0, 0, 0, 0, 0, 640, 480, NULL, 0, 0, {0}, 0 };

static int mmenu_can_show(void)
{
    if (g_mode_client) return 0;
    if (gf_read_int(GF_SCRIPT_ACT_VA, -1) != 0) return 0;
    if (IsBadReadPtr((void *)GF_DECK_ACTIVE_VA, 1) || *(BYTE *)GF_DECK_ACTIVE_VA == 0) return 0;
    if (gf_read_int(GF_MODAL_TOP_VA, -1) != 0) return 0;
    if (gf_read_int(GF_LEAVE_SCENE_VA, 0) != 0) return 0;
    if (gf_read_int(GF_MOVIE_CTX_VA, 0) != 0 || gf_wc3en_hd_movie_playing(NULL)) return 0;
    return 1;
}

static BYTE *mmenu_root_gump(void)
{
    int top = gf_read_int(GF_MODAL_TOP_VA, -1);
    if (top < 0 || top >= 32 || IsBadReadPtr((void *)(GF_MODAL_STACK_VA + top * 4), 4))
        return NULL;
    return *(BYTE **)(GF_MODAL_STACK_VA + top * 4);
}

static void mmenu_set_input_block(int block)
{
    if (block) {
        BYTE *root = mmenu_root_gump();
        if (g_mmenu.blocked_gump && g_mmenu.blocked_gump != root &&
            !IsBadWritePtr(g_mmenu.blocked_gump + 0x3c, 4)) {
            *(int *)(g_mmenu.blocked_gump + 0x3c) = 0;
        }
        if (root && !IsBadWritePtr(root + 0x3c, 4)) {
            *(int *)(root + 0x3c) = 1;
            g_mmenu.blocked_gump = root;
        }
    } else if (g_mmenu.blocked_gump) {
        if (!IsBadWritePtr(g_mmenu.blocked_gump + 0x3c, 4))
            *(int *)(g_mmenu.blocked_gump + 0x3c) = 0;
        g_mmenu.blocked_gump = NULL;
    }
}

static void mmenu_close(void)
{
    g_mmenu.open = 0;
    mmenu_set_input_block(0);
}

static void mmenu_get_dims(int *out_stride, int *out_height)
{
    int stride = g_mmenu.ui_stride > 0 ? g_mmenu.ui_stride : 640;
    int height = g_mmenu.ui_height > 0 ? g_mmenu.ui_height : 480;
    if (!IsBadReadPtr((void *)UI_VIEWPORT_VA, 4)) {
        int *vp = *(int **)UI_VIEWPORT_VA;
        if (vp && !IsBadReadPtr(vp, 20) && vp[0] && !IsBadReadPtr((void *)vp[0], 20)) {
            int *surf = (int *)vp[0];
            int sw = surf[1] + 1, sh = surf[2] + 1;
            if (sw >= 320 && sw <= 1024 && sh >= 200 && sh <= 768) {
                stride = sw;
                height = sh;
            }
        }
    }
    *out_stride = stride;
    *out_height = height;
}

static void mmenu_box_rect(int stride, int height, int *x1, int *y1, int *x2, int *y2)
{
    int mw = (stride >= 620) ? 580 : (stride - 16);
    int mh = (height >= 460) ? 432 : (height - 16);
    *x1 = (stride - mw) / 2;
    *y1 = (height - mh) / 2;
    *x2 = *x1 + mw - 1;
    *y2 = *y1 + mh - 1;
}

static int mmenu_pt_in_rect(int px, int py, int x1, int y1, int x2, int y2)
{
    return (px >= x1 && px <= x2 && py >= y1 && py <= y2);
}

static int mmenu_filter_indices(int tab, int *out, int max_out)
{
    int i, n = 0;
    for (i = 0; i < WC3_MISN_COUNT && n < max_out; i++) {
        unsigned char fl = g_wc3_missions[i].flags;
        if (tab == 1 && !(fl & MISN_F_SPECIAL)) continue;
        if (tab == 2 && !(fl & MISN_F_SIM)) continue;
        out[n++] = i;
    }
    return n;
}

static void mmenu_load_misn_defaults(int misn_idx)
{
    const wc3_misn_def *m = &g_wc3_missions[misn_idx];
    g_mmenu.misn_idx = misn_idx;
    g_mmenu.ship     = m->def_ship;
    g_mmenu.w1_ship  = -1;
    g_mmenu.w1_pilot = m->def_w1;
    g_mmenu.w2_pilot = m->def_w2;
    g_mmenu.w3_pilot = m->def_w3;
    g_mmenu.builtin  = 0;
    g_mmenu.rreg1_val = m->rreg1 ? gf_read_int(GF_REGS_VA + m->rreg1 * 4, 0) : 0;
    g_mmenu.rreg2_val = m->rreg2 ? gf_read_int(GF_REGS_VA + m->rreg2 * 4, 0) : 0;
    if (g_mmenu.rreg1_val != 0) g_mmenu.rreg1_val = 1;
    if (g_mmenu.rreg2_val != 0) g_mmenu.rreg2_val = 1;
    g_mmenu.sel = 0;
}

static int mmenu_build_flag_rows(const wc3_misn_def *m, int *rows, int max_rows)
{
    int n = 0;
    if (n < max_rows) rows[n++] = MROW_SHIP;
    if (m->team_cnt >= 2 && n < max_rows) rows[n++] = MROW_W1SHIP;
    if (m->team_cnt >= 2 && n < max_rows) rows[n++] = MROW_W1PILOT;
    if (m->team_cnt >= 3 && n < max_rows) rows[n++] = MROW_W2PILOT;
    if (m->team_cnt >= 4 && n < max_rows) rows[n++] = MROW_W3PILOT;
    if (n < max_rows) rows[n++] = MROW_LOADOUT;
    if (m->rreg1 != 0 && n < max_rows) rows[n++] = MROW_RREG1;
    if (m->rreg2 != 0 && n < max_rows) rows[n++] = MROW_RREG2;
    return n;
}

static int mmenu_cycle_pilot(int cur, int dir)
{
    int i, idx = 0;
    for (i = 0; i < WC3_PILOT_COUNT; i++) {
        if (g_wc3_pilot_ids[i] == cur) { idx = i; break; }
    }
    idx = (idx + dir + WC3_PILOT_COUNT) % WC3_PILOT_COUNT;
    return g_wc3_pilot_ids[idx];
}

static void mmenu_step_flag_row(int row_id, int dir)
{
    switch (row_id) {
    case MROW_SHIP:
        g_mmenu.ship = ((g_mmenu.ship + 1 + dir + 8) % 8) - 1; // -1..6
        break;
    case MROW_W1SHIP:
        g_mmenu.w1_ship = ((g_mmenu.w1_ship + 1 + dir + 8) % 8) - 1; // -1..6
        break;
    case MROW_W1PILOT:
        g_mmenu.w1_pilot = mmenu_cycle_pilot(g_mmenu.w1_pilot, dir);
        break;
    case MROW_W2PILOT:
        g_mmenu.w2_pilot = mmenu_cycle_pilot(g_mmenu.w2_pilot, dir);
        break;
    case MROW_W3PILOT:
        g_mmenu.w3_pilot = mmenu_cycle_pilot(g_mmenu.w3_pilot, dir);
        break;
    case MROW_LOADOUT:
        g_mmenu.builtin = !g_mmenu.builtin;
        break;
    case MROW_RREG1:
        g_mmenu.rreg1_val = !g_mmenu.rreg1_val;
        break;
    case MROW_RREG2:
        g_mmenu.rreg2_val = !g_mmenu.rreg2_val;
        break;
    }
}

static void mmenu_format_flag_row(const wc3_misn_def *m, int row_id,
                                  char *line, int line_cap,
                                  char *desc, int desc_cap)
{
    const char *lbl = "", *dsc = "";
    switch (row_id) {
    case MROW_SHIP:
        wsprintfA(line, "Player Ship (r22): %s", wc3_ship_name(g_mmenu.ship));
        lstrcpynA(desc, "Fighter flown by Host / Blair (-1=default, 5=Cloak, 6=Temblor)", desc_cap);
        break;
    case MROW_W1SHIP:
        wsprintfA(line, "Wingman 1 Ship (r24): %s",
                  (g_mmenu.w1_ship == -1) ? "Match Player" : wc3_ship_name(g_mmenu.w1_ship));
        lstrcpynA(desc, "Fighter flown by Client / Wingman 1 (-1 matches player ship)", desc_cap);
        break;
    case MROW_W1PILOT:
        wsprintfA(line, "Wingman 1 Pilot (r23): %s", wc3_pilot_name(g_mmenu.w1_pilot));
        lstrcpynA(desc, "None (Solo) disables wingman 1; any pilot enables co-op slot", desc_cap);
        break;
    case MROW_W2PILOT:
        wsprintfA(line, "Wingman 2 Pilot (r58): %s", wc3_pilot_name(g_mmenu.w2_pilot));
        lstrcpynA(desc, "Second AI wingman slot in 4-ship Kilrah strike missions", desc_cap);
        break;
    case MROW_W3PILOT:
        wsprintfA(line, "Wingman 3 Pilot (r59): %s", wc3_pilot_name(g_mmenu.w3_pilot));
        lstrcpynA(desc, "Third AI wingman slot in 4-ship Kilrah strike missions", desc_cap);
        break;
    case MROW_LOADOUT:
        wsprintfA(line, "Weapons Loadout (r46): %s",
                  g_mmenu.builtin ? "Mission Built-in (1)" : "Armory Custom (0)");
        lstrcpynA(desc, "1 keeps ship default weapons/cloak; 0 uses armory loadout", desc_cap);
        break;
    case MROW_RREG1:
        wc3_rreg_info(m->rreg1, &lbl, &dsc);
        wsprintfA(line, "%s: %d", lbl, g_mmenu.rreg1_val);
        lstrcpynA(desc, dsc, desc_cap);
        break;
    case MROW_RREG2:
        wc3_rreg_info(m->rreg2, &lbl, &dsc);
        wsprintfA(line, "%s: %d", lbl, g_mmenu.rreg2_val);
        lstrcpynA(desc, dsc, desc_cap);
        break;
    default:
        if (line_cap > 0) line[0] = 0;
        if (desc_cap > 0) desc[0] = 0;
        break;
    }
    (void)line_cap;
}

static void gf_host_launch_debug_mission(void)
{
    const wc3_misn_def *m = &g_wc3_missions[g_mmenu.misn_idx];
    int mid = (int)m->id;
    char buf[192];

    mmenu_close();
    gf_clear_input_capture();
    gf_dbg_snap_save();

    gf_write_int(GF_REGS_VA + 22 * 4, g_mmenu.ship);
    gf_write_int(GF_REGS_VA + 23 * 4, g_mmenu.w1_pilot);
    gf_write_int(GF_REGS_VA + 24 * 4, g_mmenu.w1_ship);
    gf_write_int(GF_REGS_VA + 46 * 4, g_mmenu.builtin);
    gf_write_int(GF_REGS_VA + 58 * 4, g_mmenu.w2_pilot);
    gf_write_int(GF_REGS_VA + 59 * 4, g_mmenu.w3_pilot);
    if (m->rreg1) gf_write_int(GF_REGS_VA + m->rreg1 * 4, g_mmenu.rreg1_val);
    if (m->rreg2) gf_write_int(GF_REGS_VA + m->rreg2 * 4, g_mmenu.rreg2_val);

    gf_ensure_pilots_alive(g_mmenu.w1_pilot, g_mmenu.w2_pilot, g_mmenu.w3_pilot);

    gf_write_int(GF_IN_SIM_VA, 0);
    gf_write_int(GF_MISSION_ID_VA, mid);
    gf_write_int(GF_REGS_VA + 40 * 4, mid);
    if (!IsBadWritePtr((void *)GF_MOVIE_SKIP_VA, 1))
        *(BYTE *)GF_MOVIE_SKIP_VA = 0;

    gf_record_host_event(WC3G_EVT_DEBUG_MISN, mid, (int)m->need_cd);
    Sleep(25);
    gf_push_state_now();

    gf_ensure_cd_mounted((int)m->need_cd);

    gf_write_int(GF_NEXT_ACT_VA, 4);
    gf_write_int(GF_SCRIPT_ACT_VA, 5);
    gf_write_int(GF_LEAVE_SCENE_VA, 1);

    wsprintfA(buf, "debug mission launch: id=%d (%s) cd=%d ship=%d w1=%d w2=%d w3=%d builtin=%d\r\n",
              mid, m->name, (int)m->need_cd, g_mmenu.ship, g_mmenu.w1_pilot,
              g_mmenu.w2_pilot, g_mmenu.w3_pilot, g_mmenu.builtin);
    log_ts_crit("gf_host", buf);
}

static void mmenu_tick(void)
{
    WORD btns = 0;
    int mx = 0, my = 0, lmb, rmb, lclick, rclick;
    int stride, height, x1, y1, x2, y2, mw, mh;
    const BYTE *keys = NULL;
    BYTE pressed[128];
    int i;

    if (!mmenu_can_show()) {
        if (g_mmenu.open != 0 || g_mmenu.blocked_gump != NULL)
            mmenu_close();
        return;
    }

    if (!IsBadReadPtr((void *)UI_MOUSE_RAW_VA, 6)) {
        const WORD *mr = (const WORD *)UI_MOUSE_RAW_VA;
        btns = mr[0];
        mx = (int)(short)mr[1];
        my = (int)(short)mr[2];
    }
    lmb = (btns & 1) != 0;
    rmb = (btns & 2) != 0;
    lclick = lmb && !g_mmenu.prev_lmb;
    rclick = rmb && !g_mmenu.prev_rmb;
    g_mmenu.prev_lmb = lmb;
    g_mmenu.prev_rmb = rmb;

    ZeroMemory(pressed, sizeof(pressed));
    if (!IsBadReadPtr((void *)KEY_STATE_TBL_VA, 128)) {
        keys = (const BYTE *)KEY_STATE_TBL_VA;
        for (i = 0; i < 128; i++) {
            int down = (keys[i] != 0);
            if (down && !g_mmenu.prev_keys[i]) pressed[i] = 1;
            g_mmenu.prev_keys[i] = (BYTE)down;
        }
    }

    mmenu_get_dims(&stride, &height);

    if (pressed[0x43]) { // F9 toggles menu
        if (g_mmenu.open != 0) {
            mmenu_close();
        } else {
            g_mmenu.open = 1;
            mmenu_set_input_block(1);
        }
        return;
    }

    if (g_mmenu.open == 0) {
        return;
    }

    mmenu_set_input_block(1);
    mmenu_box_rect(stride, height, &x1, &y1, &x2, &y2);
    mw = x2 - x1 + 1;
    mh = y2 - y1 + 1;

    if (pressed[0x01] || rclick) { // ESC or right-click steps back / closes
        g_mmenu.esc_swallow_until = GetTickCount() + 250;
        if (g_mmenu.open == 2 && pressed[0x01]) {
            g_mmenu.open = 1;
        } else if (g_mmenu.open == 2 && rclick) {
            // Right-click inside a flag row steps backward; elsewhere goes back
            const wc3_misn_def *m = &g_wc3_missions[g_mmenu.misn_idx];
            int rows[8], nr = mmenu_build_flag_rows(m, rows, 8);
            int row_h = (mh - 92) / 8;
            int handled = 0;
            if (row_h > 36) row_h = 36;
            for (i = 0; i < nr; i++) {
                int ry1 = y1 + 48 + i * row_h;
                if (mmenu_pt_in_rect(mx, my, x1 + 10, ry1, x2 - 10, ry1 + row_h - 3)) {
                    g_mmenu.sel = i;
                    mmenu_step_flag_row(rows[i], -1);
                    handled = 1;
                    break;
                }
            }
            if (!handled) g_mmenu.open = 1;
        } else {
            mmenu_close();
        }
        return;
    }

    if (g_mmenu.open == 1) {
        int idxs[WC3_MISN_COUNT];
        int total = mmenu_filter_indices(g_mmenu.tab, idxs, WC3_MISN_COUNT);
        int per_page = (mh >= 390) ? MMENU_PER_PAGE : 8;
        int pages = (total + per_page - 1) / per_page;
        int list_h = mh - 96;
        int row_h = list_h / per_page;
        int start, cnt;

        if (pages < 1) pages = 1;
        if (g_mmenu.page >= pages) g_mmenu.page = pages - 1;
        if (g_mmenu.page < 0) g_mmenu.page = 0;
        start = g_mmenu.page * per_page;
        cnt = total - start;
        if (cnt > per_page) cnt = per_page;
        if (cnt > 0 && g_mmenu.sel >= cnt) g_mmenu.sel = cnt - 1;
        if (g_mmenu.sel < 0) g_mmenu.sel = 0;

        if (pressed[0x48] && cnt > 0) { // Up
            if (g_mmenu.sel > 0) g_mmenu.sel--;
            else if (g_mmenu.page > 0) { g_mmenu.page--; g_mmenu.sel = per_page - 1; }
        }
        if (pressed[0x50] && cnt > 0) { // Down
            if (g_mmenu.sel + 1 < cnt) g_mmenu.sel++;
            else if (g_mmenu.page + 1 < pages) { g_mmenu.page++; g_mmenu.sel = 0; }
        }
        if ((pressed[0x49] || pressed[0x4b]) && g_mmenu.page > 0) { // PgUp / Left
            g_mmenu.page--;
            g_mmenu.sel = 0;
        }
        if ((pressed[0x51] || pressed[0x4d]) && g_mmenu.page + 1 < pages) { // PgDn / Right
            g_mmenu.page++;
            g_mmenu.sel = 0;
        }
        if (pressed[0x0f]) { // Tab cycles filter tabs
            g_mmenu.tab = (g_mmenu.tab + 1) % 3;
            g_mmenu.page = 0;
            g_mmenu.sel = 0;
        }
        if (pressed[0x1c] && cnt > 0) { // Enter opens Flag Submenu
            mmenu_load_misn_defaults(idxs[start + g_mmenu.sel]);
            g_mmenu.open = 2;
            return;
        }

        if (lclick) {
            int tx = x1 + 10, tw = (mw - 120) / 3;
            // Tabs
            for (i = 0; i < 3; i++) {
                int bx1 = tx + i * (tw + 4), bx2 = bx1 + tw - 1;
                if (mmenu_pt_in_rect(mx, my, bx1, y1 + 24, bx2, y1 + 42)) {
                    g_mmenu.tab = i;
                    g_mmenu.page = 0;
                    g_mmenu.sel = 0;
                    return;
                }
            }
            // Close button
            if (mmenu_pt_in_rect(mx, my, x2 - 86, y1 + 24, x2 - 10, y1 + 42)) {
                mmenu_close();
                return;
            }
            // Mission rows
            for (i = 0; i < cnt; i++) {
                int ry1 = y1 + 50 + i * row_h;
                if (mmenu_pt_in_rect(mx, my, x1 + 10, ry1, x2 - 10, ry1 + row_h - 2)) {
                    mmenu_load_misn_defaults(idxs[start + i]);
                    g_mmenu.open = 2;
                    return;
                }
            }
            // Prev / Next buttons
            if (mmenu_pt_in_rect(mx, my, x1 + 10, y2 - 28, x1 + 104, y2 - 8) && g_mmenu.page > 0) {
                g_mmenu.page--;
                g_mmenu.sel = 0;
                return;
            }
            if (mmenu_pt_in_rect(mx, my, x2 - 104, y2 - 28, x2 - 10, y2 - 8) && g_mmenu.page + 1 < pages) {
                g_mmenu.page++;
                g_mmenu.sel = 0;
                return;
            }
        }
    } else if (g_mmenu.open == 2) {
        const wc3_misn_def *m = &g_wc3_missions[g_mmenu.misn_idx];
        int rows[8], nr = mmenu_build_flag_rows(m, rows, 8);
        int row_h = (mh - 92) / 8;
        if (row_h > 36) row_h = 36;
        if (g_mmenu.sel >= nr) g_mmenu.sel = nr - 1;
        if (g_mmenu.sel < 0) g_mmenu.sel = 0;

        if (pressed[0x48] && g_mmenu.sel > 0) g_mmenu.sel--;
        if (pressed[0x50] && g_mmenu.sel + 1 < nr) g_mmenu.sel++;
        if (pressed[0x4b]) mmenu_step_flag_row(rows[g_mmenu.sel], -1);
        if (pressed[0x4d] || pressed[0x39]) mmenu_step_flag_row(rows[g_mmenu.sel], 1);
        if (pressed[0x1c]) { // Enter launches mission directly
            gf_host_launch_debug_mission();
            return;
        }

        if (lclick) {
            for (i = 0; i < nr; i++) {
                int ry1 = y1 + 48 + i * row_h;
                if (mmenu_pt_in_rect(mx, my, x1 + 10, ry1, x2 - 10, ry1 + row_h - 3)) {
                    g_mmenu.sel = i;
                    mmenu_step_flag_row(rows[i], 1);
                    return;
                }
            }
            if (mmenu_pt_in_rect(mx, my, x1 + 10, y2 - 30, x1 + 186, y2 - 8)) {
                gf_host_launch_debug_mission();
                return;
            }
            if (mmenu_pt_in_rect(mx, my, x1 + 198, y2 - 30, x1 + 358, y2 - 8)) {
                mmenu_load_misn_defaults(g_mmenu.misn_idx);
                return;
            }
            if (mmenu_pt_in_rect(mx, my, x2 - 106, y2 - 30, x2 - 10, y2 - 8)) {
                g_mmenu.open = 1;
                return;
            }
        }
    }
}

static const char *gf_conn_status_text(void)
{
    if (g_net_disabled)
        return NULL;
    if (IsBadReadPtr((void *)GF_DECK_ACTIVE_VA, 1) || *(BYTE *)GF_DECK_ACTIVE_VA == 0)
        return NULL;
    if (gf_read_int(GF_MOVIE_CTX_VA, 0) != 0 || gf_wc3en_hd_movie_playing(NULL))
        return NULL;
    if (g_mode_client)
        return wc3net_gf_host_present() ? NULL : "CONNECTING TO HOST...";
    if (g_mode_remote || g_mode_observe_net)
        return wc3net_gf_client_present() ? NULL : "WAITING FOR CLIENT...";
    return NULL;
}

static void *ui_pick_banner_font(const char *msg, int max_w, int *out_w, int *out_h)
{
    void *cands[3] = { NULL, NULL, NULL };
    BYTE *ftbl = NULL;
    int i;

    if (!IsBadReadPtr((void *)UI_FONT_TABLE_VA, 4))
        ftbl = *(BYTE **)UI_FONT_TABLE_VA;
    if (ftbl && !IsBadReadPtr(ftbl, 0x1c)) {
        cands[0] = *(void **)(ftbl + 0x18);
        cands[1] = *(void **)(ftbl + 0x10);
    }
    if (!IsBadReadPtr((void *)UI_SMALL_FONT_VA, 4))
        cands[2] = *(void **)UI_SMALL_FONT_VA;

    for (i = 0; i < 3; i++) {
        void *font = cands[i];
        int h, w = 0, j;
        if (!font || IsBadReadPtr(font, 0x410))
            continue;
        h = ((ui_font_height_fn)UI_FONT_HEIGHT_VA)(font);
        if (h <= 4 || h > 60)
            continue;
        for (j = 0; msg[j]; j++) {
            int cw = ((ui_font_char_w_fn)UI_FONT_CHAR_W_VA)(font, (unsigned char)msg[j]);
            if (cw <= 0 || cw > 64) { w = 0; break; }
            w += cw;
        }
        if (w > 0 && w <= max_w) {
            *out_w = w;
            *out_h = h;
            return font;
        }
    }
    return NULL;
}

static void *ui_pick_small_font(int *out_h)
{
    void *cands[4] = { NULL, NULL, NULL, NULL };
    BYTE *ftbl = NULL;
    int i;

    if (!IsBadReadPtr((void *)UI_SMALL_FONT_VA, 4))
        cands[0] = *(void **)UI_SMALL_FONT_VA;
    if (!IsBadReadPtr((void *)UI_FONT_TABLE_VA, 4))
        ftbl = *(BYTE **)UI_FONT_TABLE_VA;
    if (ftbl && !IsBadReadPtr(ftbl, 0x1c)) {
        cands[1] = *(void **)(ftbl + 0x08);
        cands[2] = *(void **)(ftbl + 0x10);
        cands[3] = *(void **)(ftbl + 0x18);
    }

    for (i = 0; i < 4; i++) {
        void *font = cands[i];
        int h;
        if (!font || IsBadReadPtr(font, 0x410))
            continue;
        h = ((ui_font_height_fn)UI_FONT_HEIGHT_VA)(font);
        if (h >= 6 && h <= 24) {
            if (out_h) *out_h = h;
            return font;
        }
    }
    return NULL;
}

static void ui_draw_border_box(void *vp, int x1, int y1, int x2, int y2, BYTE bg_col, BYTE border_col)
{
    ((ui_fill_rect_fn)UI_FILL_RECT_VA)(x1, y1, x2, y2, (int)bg_col);
    if (border_col != bg_col) {
        ((ui_draw_line_fn)UI_DRAW_LINE_VA)(vp, x1, y1, x2, y1, 0, (int)border_col);
        ((ui_draw_line_fn)UI_DRAW_LINE_VA)(vp, x2, y1, x2, y2, 0, (int)border_col);
        ((ui_draw_line_fn)UI_DRAW_LINE_VA)(vp, x2, y2, x1, y2, 0, (int)border_col);
        ((ui_draw_line_fn)UI_DRAW_LINE_VA)(vp, x1, y2, x1, y1, 0, (int)border_col);
    }
}

static void ui_draw_outline(void *vp, int x1, int y1, int x2, int y2, BYTE col)
{
    ((ui_draw_line_fn)UI_DRAW_LINE_VA)(vp, x1, y1, x2, y1, 0, (int)col);
    ((ui_draw_line_fn)UI_DRAW_LINE_VA)(vp, x2, y1, x2, y2, 0, (int)col);
    ((ui_draw_line_fn)UI_DRAW_LINE_VA)(vp, x2, y2, x1, y2, 0, (int)col);
    ((ui_draw_line_fn)UI_DRAW_LINE_VA)(vp, x1, y2, x1, y1, 0, (int)col);
}

static void ui_draw_text_clipped(void *vp, int x, int y, int max_w, void *font, const char *s)
{
    char buf[128];
    int i, w = 0;
    if (!font || !s) return;
    for (i = 0; s[i] && i < (int)sizeof(buf) - 1; i++) {
        int cw = ((ui_font_char_w_fn)UI_FONT_CHAR_W_VA)(font, (unsigned char)s[i]);
        if (cw <= 0 || cw > 64) cw = 6;
        if (w + cw > max_w) break;
        buf[i] = s[i];
        w += cw;
    }
    buf[i] = 0;
    if (i > 0)
        ((ui_draw_text_fn)UI_DRAW_TEXT_VA)(vp, x, y, font, buf, (void *)UI_COLOR_TBL_VA);
}

static int mmenu_draw_overlay(int *vp, BYTE *pixels, int stride, int height,
                              BYTE *save_buf, DWORD save_cap,
                              int *ox1, int *oy1, int *ox2, int *oy2)
{
    int fh = 10, x1, y1, x2, y2, mw, mh, row;
    void *font;
    BYTE bg_col, border_col;
    int mx = -1, my = -1;

    if (g_mmenu.open == 0 || !mmenu_can_show()) return 0;
    if (IsBadReadPtr((void *)UI_BG_COLOR_VA, 1) || IsBadReadPtr((void *)UI_COLOR_TBL_VA, 256))
        return 0;
    bg_col = *(BYTE *)UI_BG_COLOR_VA;
    border_col = *(BYTE *)UI_COLOR_TBL_VA;
    font = ui_pick_small_font(&fh);
    if (!font) return 0;

    if (!IsBadReadPtr((void *)UI_MOUSE_RAW_VA, 6)) {
        const WORD *mr = (const WORD *)UI_MOUSE_RAW_VA;
        mx = (int)(short)mr[1];
        my = (int)(short)mr[2];
    }

    mmenu_box_rect(stride, height, &x1, &y1, &x2, &y2);
    mw = x2 - x1 + 1;
    mh = y2 - y1 + 1;
    if (x1 < 0 || y1 < 0 || x2 >= stride || y2 >= height ||
        (DWORD)mw * (DWORD)mh > save_cap)
        return 0;

    for (row = 0; row < mh; row++)
        CopyMemory(save_buf + row * mw, pixels + (y1 + row) * stride + x1, mw);

    ui_draw_border_box(vp, x1, y1, x2, y2, bg_col, border_col);

    if (g_mmenu.open == 1) {
        int idxs[WC3_MISN_COUNT];
        int total = mmenu_filter_indices(g_mmenu.tab, idxs, WC3_MISN_COUNT);
        int per_page = (mh >= 390) ? MMENU_PER_PAGE : 8;
        int pages = (total + per_page - 1) / per_page;
        int list_h = mh - 96;
        int row_h = list_h / per_page;
        int start, cnt, i;
        int tx = x1 + 10, tw = (mw - 120) / 3;
        const char *tabs[3] = { "ALL (65)", "SPECIAL", "SIM (10)" };
        char fbuf[96];

        if (pages < 1) pages = 1;
        if (g_mmenu.page >= pages) g_mmenu.page = pages - 1;
        start = g_mmenu.page * per_page;
        cnt = total - start;
        if (cnt > per_page) cnt = per_page;

        ui_draw_text_clipped(vp, x1 + 12, y1 + 6, mw - 24, font,
                             "DEBUG MISSION SELECT (HOST + CLIENT LOCKSTEP)");

        for (i = 0; i < 3; i++) {
            int bx1 = tx + i * (tw + 4), bx2 = bx1 + tw - 1;
            ui_draw_border_box(vp, bx1, y1 + 24, bx2, y1 + 42, bg_col, border_col);
            ui_draw_text_clipped(vp, bx1 + 6, y1 + 26, tw - 10, font,
                                 (g_mmenu.tab == i) ? (i == 0 ? ">ALL (65)" : i == 1 ? ">SPECIAL" : ">SIM (10)") : tabs[i]);
        }
        ui_draw_border_box(vp, x2 - 86, y1 + 24, x2 - 10, y1 + 42, bg_col, border_col);
        ui_draw_text_clipped(vp, x2 - 80, y1 + 26, 70, font, "[X CLOSE]");

        for (i = 0; i < cnt; i++) {
            const wc3_misn_def *m = &g_wc3_missions[idxs[start + i]];
            int ry1 = y1 + 50 + i * row_h;
            int ry2 = ry1 + row_h - 2;
            int hov = mmenu_pt_in_rect(mx, my, x1 + 10, ry1, x2 - 10, ry2);
            int cur = (g_mmenu.sel == i) || hov;
            char rbuf[112];
            if (cur)
                ui_draw_outline(vp, x1 + 10, ry1, x2 - 10, ry2, border_col);
            wsprintfA(rbuf, "%s%s", cur ? "> " : "  ", m->name);
            ui_draw_text_clipped(vp, x1 + 14, ry1 + 3, mw - 28, font, rbuf);
        }

        ui_draw_border_box(vp, x1 + 10, y2 - 28, x1 + 104, y2 - 8, bg_col, border_col);
        ui_draw_text_clipped(vp, x1 + 18, y2 - 24, 82, font, "< PREV");
        ui_draw_border_box(vp, x2 - 104, y2 - 28, x2 - 10, y2 - 8, bg_col, border_col);
        ui_draw_text_clipped(vp, x2 - 92, y2 - 24, 80, font, "NEXT >");
        wsprintfA(fbuf, "Page %d/%d (%d) - Click mission or Enter",
                  g_mmenu.page + 1, pages, total);
        ui_draw_text_clipped(vp, x1 + 114, y2 - 24, mw - 228, font, fbuf);
    } else if (g_mmenu.open == 2) {
        const wc3_misn_def *m = &g_wc3_missions[g_mmenu.misn_idx];
        int rows[8], nr = mmenu_build_flag_rows(m, rows, 8);
        int row_h = (mh - 92) / 8;
        int i;
        char tags[128];

        if (row_h > 36) row_h = 36;
        wsprintfA(tags, "%s%s%s%s%s%s%s",
                  (m->need_cd == 4) ? " [CD4]" : "",
                  (m->flags & MISN_F_PLANET) ? " [PLANET]" : "",
                  (m->flags & MISN_F_CLOAK) ? " [CLOAK]" : "",
                  (m->flags & MISN_F_TEMBLOR) ? " [TEMBLOR]" : "",
                  (m->flags & MISN_F_MULTI) ? " [3-WING]" : "",
                  (m->flags & MISN_F_SOLO) ? " [SOLO]" : "",
                  (m->flags & MISN_F_SIM) ? " [SIM]" : "");
        ui_draw_text_clipped(vp, x1 + 12, y1 + 6, mw - 24, font, "MISSION FLAGS & LAUNCH");
        {
            char sub[144];
            wsprintfA(sub, "%s%s", m->name, tags);
            ui_draw_text_clipped(vp, x1 + 12, y1 + 22, mw - 24, font, sub);
        }

        for (i = 0; i < nr; i++) {
            int ry1 = y1 + 48 + i * row_h;
            int ry2 = ry1 + row_h - 3;
            int hov = mmenu_pt_in_rect(mx, my, x1 + 10, ry1, x2 - 10, ry2);
            int cur = (g_mmenu.sel == i) || hov;
            char line[128], desc[128], pbuf[140];
            mmenu_format_flag_row(m, rows[i], line, sizeof(line), desc, sizeof(desc));
            if (cur)
                ui_draw_outline(vp, x1 + 10, ry1, x2 - 10, ry2, border_col);
            wsprintfA(pbuf, "%s%s", cur ? "> " : "  ", line);
            ui_draw_text_clipped(vp, x1 + 14, ry1 + 2, mw - 28, font, pbuf);
            if (row_h >= 28) {
                char dbuf[140];
                wsprintfA(dbuf, "   %s", desc);
                ui_draw_text_clipped(vp, x1 + 18, ry1 + fh + 4, mw - 36, font, dbuf);
            }
        }

        ui_draw_border_box(vp, x1 + 10, y2 - 30, x1 + 186, y2 - 8, bg_col, border_col);
        ui_draw_text_clipped(vp, x1 + 20, y2 - 25, 160, font, "[LAUNCH MISSION]");
        ui_draw_border_box(vp, x1 + 198, y2 - 30, x1 + 358, y2 - 8, bg_col, border_col);
        ui_draw_text_clipped(vp, x1 + 210, y2 - 25, 140, font, "[RESET DEFAULTS]");
        ui_draw_border_box(vp, x2 - 106, y2 - 30, x2 - 10, y2 - 8, bg_col, border_col);
        ui_draw_text_clipped(vp, x2 - 92, y2 - 25, 80, font, "[< BACK]");
    }

    *ox1 = x1; *oy1 = y1; *ox2 = x2; *oy2 = y2;
    return 1;
}

// Screen SLAM / present relay (vtable 0x49ceb4 -> FUN_00482740, __fastcall, ret 0).
// Draws the connection status banner and Host Mission Select overlay with
// save-under / restore around SLAM so WC3's native cursor renders cleanly on top.
static void __attribute__((fastcall)) screen_slam_relay(void *screen)
{
    static BYTE s_save_buf[640 * 80];
    static BYTE s_mmenu_save[640 * 480];
    static int in_slam;
    static const char *prev_msg;
    const char *msg;
    int did_banner = 0, bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
    int did_mmenu = 0, mx1 = 0, my1 = 0, mx2 = 0, my2 = 0;
    int *vp = NULL;
    BYTE *pixels = NULL;
    int stride = 0, height = 0;
    int saved_vp[4] = { 0, 0, 0, 0 };
    int vp_modified = 0;

    if (in_slam) {
        g_screen_slam_orig(screen);
        return;
    }
    in_slam = 1;

    msg = gf_conn_status_text();
    if (msg != prev_msg) {
        if (msg) {
            char buf[96];
            wsprintfA(buf, "status overlay shown: %s\r\n", msg);
            log_ts("ui", buf);
        } else {
            log_ts("ui", "status overlay hidden (connected)\r\n");
        }
        prev_msg = msg;
    }

    if ((msg || (g_mmenu.open != 0 && mmenu_can_show())) && !IsBadReadPtr((void *)UI_VIEWPORT_VA, 4)) {
        vp = *(int **)UI_VIEWPORT_VA;
        if (vp && !IsBadWritePtr(vp, 20) && vp[0] && !IsBadReadPtr((void *)vp[0], 20)) {
            int *surf = (int *)vp[0];
            pixels = (BYTE *)surf[0];
            stride = surf[1] + 1;
            height = surf[2] + 1;
            if (pixels && stride >= 320 && stride <= 1024 &&
                height >= 200 && height <= 768 &&
                !IsBadWritePtr(pixels, (SIZE_T)stride * (SIZE_T)height)) {
                g_mmenu.ui_stride = stride;
                g_mmenu.ui_height = height;
                saved_vp[0] = vp[1];
                saved_vp[1] = vp[2];
                saved_vp[2] = vp[3];
                saved_vp[3] = vp[4];
                vp[1] = 0;
                vp[2] = 0;
                vp[3] = stride - 1;
                vp[4] = height - 1;
                vp_modified = 1;

                if (msg) {
                    int tw = 0, th = 0;
                    void *font = ui_pick_banner_font(msg, stride - 48, &tw, &th);
                    if (font) {
                        int pad_x = 16, pad_y = 8;
                        int bw = tw + 2 * pad_x;
                        int bh = th + 2 * pad_y;
                        bx1 = (stride - bw) / 2;
                        by1 = 16;
                        bx2 = bx1 + bw - 1;
                        by2 = by1 + bh - 1;
                        if (bx1 >= 0 && by1 >= 0 && bx2 < stride && by2 < height &&
                            (DWORD)bw * (DWORD)bh <= sizeof(s_save_buf) &&
                            !IsBadReadPtr((void *)UI_BG_COLOR_VA, 1) &&
                            !IsBadReadPtr((void *)UI_COLOR_TBL_VA, 256)) {
                            BYTE bg_col = *(BYTE *)UI_BG_COLOR_VA;
                            BYTE border_col = *(BYTE *)UI_COLOR_TBL_VA;
                            int row;
                            for (row = 0; row < bh; row++)
                                CopyMemory(s_save_buf + row * bw, pixels + (by1 + row) * stride + bx1, bw);
                            ui_draw_border_box(vp, bx1, by1, bx2, by2, bg_col, border_col);
                            ((ui_draw_text_fn)UI_DRAW_TEXT_VA)(vp, bx1 + pad_x, by1 + pad_y,
                                                                font, msg, (void *)UI_COLOR_TBL_VA);
                            did_banner = 1;
                        }
                    }
                }

                did_mmenu = mmenu_draw_overlay(vp, pixels, stride, height,
                                               s_mmenu_save, sizeof(s_mmenu_save),
                                               &mx1, &my1, &mx2, &my2);

                if (did_banner || did_mmenu) {
                    // Ensure FUN_00482740 blits the union of the existing dirty rect and our overlays
                    int ux0 = saved_vp[0], uy0 = saved_vp[1], ux1 = saved_vp[2], uy1 = saved_vp[3];
                    if (ux0 > ux1 || uy0 > uy1) {
                        ux0 = 0; uy0 = 0; ux1 = stride - 1; uy1 = height - 1;
                    }
                    if (did_banner) {
                        if (bx1 < ux0) ux0 = bx1;
                        if (by1 < uy0) uy0 = by1;
                        if (bx2 > ux1) ux1 = bx2;
                        if (by2 > uy1) uy1 = by2;
                    }
                    if (did_mmenu) {
                        if (mx1 < ux0) ux0 = mx1;
                        if (my1 < uy0) uy0 = my1;
                        if (mx2 > ux1) ux1 = mx2;
                        if (my2 > uy1) uy1 = my2;
                    }
                    if (ux0 < 0) ux0 = 0;
                    if (uy0 < 0) uy0 = 0;
                    if (ux1 >= stride) ux1 = stride - 1;
                    if (uy1 >= height) uy1 = height - 1;
                    vp[1] = ux0; vp[2] = uy0; vp[3] = ux1; vp[4] = uy1;
                } else {
                    vp[1] = saved_vp[0];
                    vp[2] = saved_vp[1];
                    vp[3] = saved_vp[2];
                    vp[4] = saved_vp[3];
                    vp_modified = 0;
                }
            }
        }
    }

    g_screen_slam_orig(screen);

    if (did_mmenu) {
        int mw = mx2 - mx1 + 1, mh = my2 - my1 + 1, row;
        for (row = 0; row < mh; row++)
            CopyMemory(pixels + (my1 + row) * stride + mx1, s_mmenu_save + row * mw, mw);
    }
    if (did_banner) {
        int bw = bx2 - bx1 + 1, bh = by2 - by1 + 1, row;
        for (row = 0; row < bh; row++)
            CopyMemory(pixels + (by1 + row) * stride + bx1, s_save_buf + row * bw, bw);
    }
    if (vp_modified) {
        vp[1] = saved_vp[0];
        vp[2] = saved_vp[1];
        vp[3] = saved_vp[2];
        vp[4] = saved_vp[3];
    }

    in_slam = 0;
}

// Screen tick relay (hooks 0x49ceb0 slot 0 -> 0x44bf40).
// Runs every frame on the Victory deck AND inside modal dialogs (such as
// conversation branch choices in FUN_004824e0).
static void __attribute__((fastcall)) screen_tick_relay(void *screen)
{
    if (g_dbg_snap.valid && g_dbg_snap.armed &&
        gf_read_int(GF_SCRIPT_ACT_VA, -1) == 0 &&
        !IsBadReadPtr((void *)GF_DECK_ACTIVE_VA, 1) &&
        *(BYTE *)GF_DECK_ACTIVE_VA != 0 &&
        gf_read_int(GF_LEAVE_SCENE_VA, 0) == 0) {
        int restore_cd = g_dbg_snap.cur_cd;
        int restore_sc = g_dbg_snap.scene_id;
        int restore_rm = g_dbg_snap.room_id;
        int restore_ms = g_dbg_snap.mission_id;
        char buf[128];
        gf_dbg_snap_restore_mem();
        g_dbg_snap.valid = 0;
        g_dbg_snap.armed = 0;
        gf_ensure_cd_mounted(restore_cd);
        gf_clear_input_capture();
        gf_write_int(GF_SCRIPT_ACT_VA, 4);
        gf_write_int(GF_NEXT_ACT_VA, 4);
        gf_write_int(GF_LEAVE_SCENE_VA, 1);
        if (!g_mode_client)
            gf_push_state_now();
        wsprintfA(buf, "restored pre-debug snapshot: sc=%d misn=%d rm=%d cd=%d (reloading scene)\r\n",
                  restore_sc, restore_ms, restore_rm, restore_cd);
        log_ts_crit("gf", buf);
        g_screen_tick_orig(screen);
        return;
    }
    if (!g_mode_client) {
        mmenu_tick();
    } else {
        static int prev_esc_down;
        wc3g_state_pkt st;
        int have_st = wc3net_gf_fetch_state(&st);

        if (have_st) {
            int cur_act   = gf_read_int(GF_SCRIPT_ACT_VA, 0);
            int cur_scene = gf_read_int(GF_SCENE_ID_VA, 0);
            int cur_room  = gf_read_int(GF_ROOM_ID_VA, 0);
            int run_evt   = gf_client_latch_events(&st);

            if (run_evt && g_gf_pending_evt && g_gf_pending_evt_type == WC3G_EVT_DEBUG_MISN) {
                gf_dbg_snap_save();
            }

            // If client is currently inside a dialogue branch modal (vtable 0x49b8a0)
            // and the host has picked a branch choice, apply and trigger it immediately.
            if (g_gf_pending_branch) {
                BYTE *modal = gf_top_branch_modal();
                if (modal) {
                    int choice = g_gf_pending_branch_choice;
                    int flipped = *(int *)(modal + 0x58);
                    g_gf_pending_branch = 0;
                    // FUN_00414e70 flips choice if *(modal+0x58) != 0, so pre-invert
                    *(int *)(modal + 0x64) = flipped ? !choice : choice;
                    log_ts("gf_cli", "replay dialogue branch choice\r\n");
                    g_gf_in_replay = 1;
                    g_branch_choose_orig(modal, NULL);
                    g_gf_in_replay = 0;
                }
            }

            if (!g_gf_in_exec_script) {
                int ran_click_or_load = 0;
                if (st.mission_id >= 0 && st.mission_id != gf_read_int(GF_MISSION_ID_VA, -1)) {
                    gf_write_int(GF_MISSION_ID_VA, st.mission_id);
                }

                // Sync campaign flags[256] and simulator mission selection
                // continuously while inside Victory deck (act==0).
                if (cur_act == 0) {
                    g_gf_pending_replay = 0;
                    if (!IsBadWritePtr((void *)GF_FLAGS_VA, sizeof(st.flags)))
                        CopyMemory((void *)GF_FLAGS_VA, st.flags, sizeof(st.flags));
                }
                if (cur_act == 0 && (st.reserved & WC3G_HFLAG_DECK_ACTIVE)) {
                    gf_write_int(GF_SIM_SCENE_VA, st.regs[38]);
                    gf_write_int(GF_SIM_MISN_VA, st.regs[39]);
                }

                // Replay discrete host events (hotspot clicks, save loads)
                if (run_evt && g_gf_pending_evt) {
                    unsigned char etype = g_gf_pending_evt_type;
                    int earg0 = g_gf_pending_evt_arg0;
                    int earg1 = g_gf_pending_evt_arg1;
                    if (etype == WC3G_EVT_CLICK && (cur_act == 0 || cur_act == 4)) {
                        gf_find_gump_fn find_gump = (gf_find_gump_fn)GF_FIND_GUMP_VA;
                        gf_exec_fn exec_script = (gf_exec_fn)GF_EXEC_SCRIPT_VA;
                        // If the click was issued in a different room than the client
                        // is currently in, jump to that room first and wait for
                        // FUN_00435300 to load that room's gump list on the next tick.
                        if (earg1 > 0 && cur_room != earg1 &&
                            (GetTickCount() - g_gf_pending_evt_tick) < 1500) {
                            gf_clear_input_capture();
                            gf_write_int(GF_REGS_VA + 4 * 4, st.regs[4]);
                            gf_write_int(GF_ROOM_ID_VA, earg1);
                            ran_click_or_load = 1;
                        } else {
                            void *gump = find_gump((void *)GF_ROOM_GUMPS_VA, earg0);
                            if (gump && !IsBadReadPtr((BYTE *)gump + 0x44, 4)) {
                                int ip = *(int *)((BYTE *)gump + 0x44);
                                g_gf_pending_evt = 0;
                                if (ip != -1) {
                                    char buf[96];
                                    wsprintfA(buf, "replay click gump_id=%d ip=%d rm=%d\r\n",
                                              earg0, ip, cur_room);
                                    log_ts("gf_cli", buf);
                                    // Keep simulator mission/scene regs aligned right before launch
                                    if (cur_room == 9) {
                                        gf_write_int(GF_SIM_SCENE_VA, st.regs[38]);
                                        gf_write_int(GF_SIM_MISN_VA, st.regs[39]);
                                    }
                                    gf_write_int(GF_CLICK_GUMP_VA, earg0);
                                    // Clear any stale movie skip flag before launching a new script/cutscene
                                    if (!IsBadWritePtr((void *)GF_MOVIE_SKIP_VA, 1))
                                        *(BYTE *)GF_MOVIE_SKIP_VA = 0;
                                    g_gf_pending_skip = 0;
                                    g_cli_hd_skip_latch = 0;
                                    g_gf_in_exec_script = 1;
                                    exec_script(ip, -1);
                                    g_gf_in_exec_script = 0;
                                    ran_click_or_load = 1;
                                }
                            } else if ((GetTickCount() - g_gf_pending_evt_tick) >= 1500) {
                                char buf[96];
                                wsprintfA(buf, "drop click gump_id=%d (not found in rm=%d, target rm=%d)\r\n",
                                          earg0, cur_room, earg1);
                                log_ts_crit("gf_cli", buf);
                                g_gf_pending_evt = 0;
                            }
                        }
                    } else if (etype == WC3G_EVT_LOAD && earg0 >= 0 && earg0 <= 0x66) {
                        char dir[MAX_PATH], sfile[MAX_PATH], buf[96];
                        DWORD wait_start = GetTickCount();
                        g_gf_pending_evt = 0;
                        gf_clear_input_capture();
                        gf_client_flush_saves();
                        gf_get_save_dir(dir, sizeof(dir));
                        wsprintfA(sfile, "%s\\%08d.WSG", dir, earg0);
                        // Give gf_pump_thread / state_rx_thread up to 600 ms if the mirrored
                        // save file chunks are still in flight before falling back.
                        while (GetFileAttributesA(sfile) == INVALID_FILE_ATTRIBUTES &&
                               (GetTickCount() - wait_start) < 600) {
                            Sleep(20);
                            gf_client_flush_saves();
                        }
                        if (GetFileAttributesA(sfile) != INVALID_FILE_ATTRIBUTES) {
                            gf_load_save_fn load_save = (gf_load_save_fn)GF_LOAD_SAVE_VA;
                            int loaded;
                            wsprintfA(buf, "replay load slot=%d\r\n", earg0);
                            log_ts_crit("gf_cli", buf);
                            loaded = load_save(earg0);
                            if (!loaded) {
                                wsprintfA(buf, "load_save(%d) returned 0, syncing state directly\r\n", earg0);
                                log_ts_crit("gf_cli", buf);
                                if (!IsBadWritePtr((void *)GF_REGS_VA, sizeof(st.regs)))
                                    CopyMemory((void *)GF_REGS_VA, st.regs, sizeof(st.regs));
                                gf_write_int(GF_SCENE_ID_VA, st.scene_id);
                                gf_write_int(GF_ROOM_ID_VA, st.room_id);
                                gf_write_int(GF_MISSION_ID_VA, st.mission_id);
                                gf_write_int(GF_SCRIPT_ACT_VA, 4);
                                gf_write_int(GF_NEXT_ACT_VA, 4);
                                gf_write_int(GF_LEAVE_SCENE_VA, 1);
                            }
                        } else {
                            // Fallback if file wasn't on disk: sync flags, regs, scene, room directly
                            wsprintfA(buf, "load slot=%d missing on disk, syncing state directly\r\n", earg0);
                            log_ts_crit("gf_cli", buf);
                            if (!IsBadWritePtr((void *)GF_REGS_VA, sizeof(st.regs)))
                                CopyMemory((void *)GF_REGS_VA, st.regs, sizeof(st.regs));
                            gf_write_int(GF_SCENE_ID_VA, st.scene_id);
                            gf_write_int(GF_ROOM_ID_VA, st.room_id);
                            gf_write_int(GF_MISSION_ID_VA, st.mission_id);
                            gf_write_int(GF_SCRIPT_ACT_VA, 4);
                            gf_write_int(GF_NEXT_ACT_VA, 4);
                            gf_write_int(GF_LEAVE_SCENE_VA, 1);
                        }
                        ran_click_or_load = 1;
                        cur_act   = gf_read_int(GF_SCRIPT_ACT_VA, 0);
                        cur_scene = gf_read_int(GF_SCENE_ID_VA, 0);
                        cur_room  = gf_read_int(GF_ROOM_ID_VA, 0);
                    } else if (etype == WC3G_EVT_MISSION && (cur_act == 0 || cur_act == 4)) {
                        // Host launched a simulator mission via FUN_00417cc0 (case 0x25).
                        // earg0 = exact resolved DAT_004ab8dc (sim_misn), earg1 = DAT_004ab8d8 (sim_scene).
                        // Launch directly with the exact mission ID (matching FUN_00417cc0 lines 78-82)
                        // rather than replaying gump 3 against the client's stale listbox selection!
                        char buf[96];
                        g_gf_pending_evt = 0;
                        gf_clear_input_capture();
                        wsprintfA(buf, "replay simulator launch misn=%d sc=%d\r\n", earg0, earg1);
                        log_ts_crit("gf_cli", buf);
                        if (!IsBadWritePtr((void *)GF_REGS_VA, sizeof(st.regs)))
                            CopyMemory((void *)GF_REGS_VA, st.regs, sizeof(st.regs));
                        gf_write_int(GF_SIM_MISN_VA, earg0);
                        gf_write_int(GF_SIM_SCENE_VA, earg1);
                        gf_write_int(GF_NEXT_ACT_VA, 4);
                        gf_write_int(GF_SCRIPT_ACT_VA, 3);
                        gf_write_int(GF_LEAVE_SCENE_VA, 1);
                        ran_click_or_load = 1;
                    } else if (etype == WC3G_EVT_DEBUG_MISN && (cur_act == 0 || cur_act == 4)) {
                        char buf[96];
                        g_gf_pending_evt = 0;
                        gf_clear_input_capture();
                        gf_dbg_snap_save();
                        if (!IsBadWritePtr((void *)GF_FLAGS_VA, sizeof(st.flags)))
                            CopyMemory((void *)GF_FLAGS_VA, st.flags, sizeof(st.flags));
                        if (!IsBadWritePtr((void *)GF_REGS_VA, sizeof(st.regs)))
                            CopyMemory((void *)GF_REGS_VA, st.regs, sizeof(st.regs));
                        gf_ensure_pilots_alive(st.regs[23], st.regs[58], st.regs[59]);
                        gf_ensure_cd_mounted(earg1);
                        gf_write_int(GF_IN_SIM_VA, 0);
                        gf_write_int(GF_MISSION_ID_VA, earg0);
                        gf_write_int(GF_REGS_VA + 40 * 4, earg0);
                        if (!IsBadWritePtr((void *)GF_MOVIE_SKIP_VA, 1))
                            *(BYTE *)GF_MOVIE_SKIP_VA = 0;
                        gf_write_int(GF_NEXT_ACT_VA, 4);
                        gf_write_int(GF_SCRIPT_ACT_VA, 5);
                        gf_write_int(GF_LEAVE_SCENE_VA, 1);
                        wsprintfA(buf, "replay debug mission launch id=%d cd=%d\r\n", earg0, earg1);
                        log_ts_crit("gf_cli", buf);
                        ran_click_or_load = 1;
                    } else {
                        g_gf_pending_evt = 0;
                    }
                }

                // If host is in a different scene/act (e.g. host already on deck,
                // loaded a save from another scene, or launched mission/sim), jump client to match.
                // Note: FUN_00435300 clears DAT_004a3580 (GF_SCRIPT_ACT_VA) to 0 at entry,
                // and at exit only copies DAT_004a3590 -> DAT_004a3580 when DAT_004a3590 != 4!
                // Leaving DAT_004a3580 == 0 when setting GF_LEAVE_SCENE_VA = 1 makes
                // FUN_00404560 hit case 0 (exit game), so ALWAYS set GF_SCRIPT_ACT_VA = 4!
                // Gate passive room/scene sync on WC3G_HFLAG_DECK_ACTIVE so the client
                // never stomps room_id to a stale pre-mission room while the host is still
                // running post-mission INIT scripts or FMVs (e.g. rm=3 before Flight Deck rm=1).
                {
                    static DWORD host_inflight_since;
                    if (cur_act == 0 && st.script_act == 0 &&
                        gf_read_int(GF_LEAVE_SCENE_VA, 0) == 0) {
                        int host_deck = (st.reserved & WC3G_HFLAG_DECK_ACTIVE) != 0;
                        host_inflight_since = 0;
                        if (st.scene_id != cur_scene && (host_deck || cur_scene == 9999)) {
                            char buf[96];
                            wsprintfA(buf, "scene sync %d -> %d (rm=%d)\r\n", cur_scene, st.scene_id, st.room_id);
                            log_ts_crit("gf_cli", buf);
                            gf_clear_input_capture();
                            if (!IsBadWritePtr((void *)GF_REGS_VA, sizeof(st.regs)))
                                CopyMemory((void *)GF_REGS_VA, st.regs, sizeof(st.regs));
                            gf_write_int(GF_SCENE_ID_VA, st.scene_id);
                            gf_write_int(GF_ROOM_ID_VA, st.room_id);
                            gf_write_int(GF_SCRIPT_ACT_VA, 4);
                            gf_write_int(GF_NEXT_ACT_VA, 4);
                            gf_write_int(GF_LEAVE_SCENE_VA, 1);
                        } else if (host_deck && st.room_id != cur_room &&
                                   !ran_click_or_load && !g_gf_pending_evt) {
                            // Sync prev_room_id (regs[4] at DAT_004ab850) alongside room_id
                            gf_clear_input_capture();
                            gf_write_int(GF_REGS_VA + 4 * 4, st.regs[4]);
                            gf_write_int(GF_ROOM_ID_VA, st.room_id);
                        }
                    } else if ((cur_act == 0 || cur_act == 4) &&
                               !ran_click_or_load && !g_gf_pending_evt &&
                               gf_read_int(GF_LEAVE_SCENE_VA, 0) == 0) {
                        int host_misn = (st.script_act == 5) || ((st.reserved & WC3G_HFLAG_IN_MISSION) != 0);
                        int host_sim  = (st.script_act == 3) || ((st.reserved & WC3G_HFLAG_IN_SIM) != 0);
                        if (host_misn || host_sim) {
                            int immediate = (st.script_act == 5 || st.script_act == 3);
                            if (host_inflight_since == 0) host_inflight_since = GetTickCount();
                            if (immediate || (GetTickCount() - host_inflight_since) >= 1500) {
                                host_inflight_since = 0;
                                if (host_sim) {
                                    char buf[96];
                                    int sim_misn = (st.regs[39] > 0) ? st.regs[39] : st.mission_id;
                                    wsprintfA(buf, "host launched simulator misn=%d, following\r\n", sim_misn);
                                    log_ts_crit("gf_cli", buf);
                                    gf_clear_input_capture();
                                    if (!IsBadWritePtr((void *)GF_REGS_VA, sizeof(st.regs)))
                                        CopyMemory((void *)GF_REGS_VA, st.regs, sizeof(st.regs));
                                    gf_write_int(GF_SIM_MISN_VA, sim_misn);
                                    gf_write_int(GF_SIM_SCENE_VA, st.regs[38]);
                                    gf_write_int(GF_NEXT_ACT_VA, 4);
                                    gf_write_int(GF_SCRIPT_ACT_VA, 3);
                                    gf_write_int(GF_LEAVE_SCENE_VA, 1);
                                } else if (st.mission_id >= 0) {
                                    char buf[96];
                                    int need_cd = wc3_mission_need_cd(st.mission_id);
                                    wsprintfA(buf, "host launched mission %d, following\r\n", st.mission_id);
                                    log_ts_crit("gf_cli", buf);
                                    gf_clear_input_capture();
                                    if (!IsBadWritePtr((void *)GF_REGS_VA, sizeof(st.regs)))
                                        CopyMemory((void *)GF_REGS_VA, st.regs, sizeof(st.regs));
                                    gf_ensure_pilots_alive(st.regs[23], st.regs[58], st.regs[59]);
                                    gf_ensure_cd_mounted(need_cd);
                                    gf_write_int(GF_MISSION_ID_VA, st.mission_id);
                                    gf_write_int(GF_SCRIPT_ACT_VA, 5);
                                    gf_write_int(GF_NEXT_ACT_VA, 4);
                                    gf_write_int(GF_LEAVE_SCENE_VA, 1);
                                }
                            }
                        } else {
                            host_inflight_since = 0;
                        }
                    } else {
                        host_inflight_since = 0;
                    }
                }
            }
        }

        // Point 3: Ensure client can always press ESC on the Victory deck to open
        // the native "EXIT GAME (Y/N)?" prompt (FUN_004121c0 case 0x82d) and quit cleanly,
        // even in rooms (like Room 8) where wc3en suppresses ESC -> Alt+X.
        if (!g_gf_in_exec_script && g_deck_key_orig &&
            !IsBadReadPtr((void *)GF_KEYDOWN_ESC_VA, 1) &&
            !IsBadReadPtr((void *)GF_DECK_ACTIVE_VA, 1)) {
            int esc_down = (*(BYTE *)GF_KEYDOWN_ESC_VA != 0);
            int deck_act = (*(BYTE *)GF_DECK_ACTIVE_VA != 0);
            int top = gf_read_int(GF_MODAL_TOP_VA, -1);
            if (!esc_down && prev_esc_down && deck_act && top == 0 &&
                (GetTickCount() - g_last_exit_prompt_tick) > 500) {
                int evt[8] = { 0x80, 1, 0, 0x82d, 0, 0, 0, 0 };
                g_last_exit_prompt_tick = GetTickCount();
                log_ts("gf_cli", "ESC released on deck, invoking EXIT GAME prompt\r\n");
                g_deck_key_orig(NULL, evt);
            }
            prev_esc_down = esc_down;
        }
    }
    g_screen_tick_orig(screen);
}

// FMV movie per-frame relay (hooks 0x49bc58 slot 0 -> 0x41b740).
// Runs every video frame inside FUN_0041c240 -> FUN_004567a0 -> FUN_00456ab0.
//   Host:   detects when FUN_0041b740 returns 0 (movie skipped via ESC or
//           double-click in FUN_0041be00) and broadcasts WC3G_EVT_SKIP immediately.
//   Client: blocks independent client movie skips while synced on deck, polls
//           WC3G_MSG_STATE, and sets DAT_004a24c4 = 1 (the game's native skip flag)
//           whenever the host skips a movie segment, enters a dialogue branch modal,
//           or finishes the movie.
static unsigned int __attribute__((thiscall)) movie_frame_relay(void *movie, unsigned int param_2)
{
    static void *last_movie_ctx;
    static int   seen_host_in_movie;
    static DWORD start_tick;
    unsigned int rc;

    if (g_mode_client && g_gf_client_synced) {
        void *cur_ctx = (void *)(size_t)gf_read_int(GF_MOVIE_CTX_VA, 0);
        int do_skip = 0;
        wc3g_state_pkt st;

        if (cur_ctx != last_movie_ctx) {
            last_movie_ctx = cur_ctx;
            seen_host_in_movie = 0;
            start_tick = GetTickCount();
        }
        if (wc3net_gf_fetch_state(&st)) {
            gf_client_latch_events(&st);
            if (st.reserved & (WC3G_HFLAG_IN_MOVIE | WC3G_HFLAG_IN_HD_MOV))
                seen_host_in_movie = 1;
            if (g_gf_pending_skip) {
                g_gf_pending_skip = 0;
                do_skip = 1;
            } else if (g_gf_pending_branch || (st.reserved & WC3G_HFLAG_IN_BRANCH)) {
                do_skip = 1;
            } else if (seen_host_in_movie &&
                       !(st.reserved & (WC3G_HFLAG_IN_MOVIE | WC3G_HFLAG_IN_HD_MOV))) {
                do_skip = 1;
            } else if (!seen_host_in_movie &&
                       (GetTickCount() - start_tick) > 600 &&
                       !(st.reserved & (WC3G_HFLAG_IN_MOVIE | WC3G_HFLAG_IN_HD_MOV)) &&
                       ((st.reserved & (WC3G_HFLAG_DECK_ACTIVE | WC3G_HFLAG_IN_MISSION | WC3G_HFLAG_IN_SIM)) != 0 ||
                        st.script_act == 5 || st.script_act == 3)) {
                do_skip = 1;
            }
        }
        if (do_skip) {
            if (!IsBadWritePtr((void *)GF_MOVIE_SKIP_VA, 1))
                *(BYTE *)GF_MOVIE_SKIP_VA = 1;
            log_ts("gf_cli", "skipping movie segment to match host\r\n");
        } else {
            // Prevent client from skipping a deck cutscene ahead of the host
            if (!IsBadWritePtr((void *)GF_MOVIE_SKIP_VA, 1))
                *(BYTE *)GF_MOVIE_SKIP_VA = 0;
            if (!IsBadWritePtr((void *)GF_KEYDOWN_ESC_VA, 1))
                *(BYTE *)GF_KEYDOWN_ESC_VA = 0;
        }
    }

    rc = g_movie_frame_orig(movie, param_2);

    if (!g_mode_client && rc == 0) {
        gf_record_host_event(WC3G_EVT_SKIP, 0, 0);
    }
    return rc;
}

// wc3en HD movie input-pump relay (hooks wc3en.dll DAT_10054abc -> 0x00482920).
// wc3en.dll replaces FUN_0041c240 with FUN_10032d30 -> FUN_10032a10 (LibVLC HD FMVs),
// which never ticks movie_frame_relay (0x49bc58); instead its tight loop calls
// (*DAT_10054abc)() and checks *DAT_10054b38 (DAT_004a24c4) and DAT_10054541 (ESC).
//   Host:   tracks whether LibVLC HD movie (DAT_100549b8) is active, and when ESC or
//           DAT_004a24c4 is set inside the HD movie loop, immediately broadcasts
//           WC3G_EVT_SKIP so the client skips its HD movie in lockstep.
//   Client: while synced on deck and inside an HD movie (DAT_100549b8 != NULL),
//           suppresses independent client ESC/skip and sets DAT_004a24c4 = 1
//           whenever the host skips, enters a dialogue branch modal, or finishes
//           the HD movie segment.
static void __cdecl wc3en_poll_input_relay(void)
{
    BYTE *base = g_wc3en_base;
    void *hd_tok = NULL;
    int playing = gf_wc3en_hd_movie_playing(&hd_tok);

    g_wc3en_poll_orig();

    if (!base) return;

    if (!g_mode_client) {
        if (playing) {
            int esc_down = !IsBadReadPtr(base + WC3EN_ESC_DOWN_OFF, 1) &&
                           (*(BYTE *)(base + WC3EN_ESC_DOWN_OFF) != 0);
            int game_esc = !IsBadReadPtr((void *)GF_KEYDOWN_ESC_VA, 1) &&
                           (*(BYTE *)GF_KEYDOWN_ESC_VA != 0);
            int skip_flag = !IsBadReadPtr((void *)GF_MOVIE_SKIP_VA, 1) &&
                            (*(BYTE *)GF_MOVIE_SKIP_VA != 0);
            if (!g_host_in_hd_movie || hd_tok != g_host_hd_last_obj) {
                g_host_in_hd_movie = 1;
                g_host_hd_last_obj = hd_tok;
                g_host_hd_skip_sent = 0;
            }
            if ((esc_down || game_esc || skip_flag) && !g_host_hd_skip_sent) {
                g_host_hd_skip_sent = 1;
                gf_record_host_event(WC3G_EVT_SKIP, 1, 0);
            }
        } else {
            if (g_host_in_hd_movie) {
                g_host_in_hd_movie = 0;
                g_host_hd_last_obj = NULL;
                g_host_hd_skip_sent = 0;
                gf_push_state_now();
            }
        }
    } else {
        if (g_client_in_navmap) {
            wc3s_packet st;
            int have_st = wc3net_state_fetch(&st);
            if (g_client_navmap_exit_req || !have_st ||
                !(st.hdr_flags & WC3S_FLAG_NAVMAP) ||
                (st.hdr_flags & (WC3S_FLAG_AUTOPILOT | WC3S_FLAG_MISSION_END))) {
                client_inject_navmap_exit(0);
            } else if (!IsBadReadPtr((void *)KEY_QUEUE_CNT_VA, 4) &&
                       !IsBadWritePtr((void *)KEY_QUEUE_CNT_VA, 4) &&
                       !IsBadWritePtr((void *)KEY_QUEUE_BUF_VA, 16 * 12)) {
                DWORD cnt = *(DWORD *)KEY_QUEUE_CNT_VA;
                DWORD i, out = 0;
                DWORD *q = (DWORD *)KEY_QUEUE_BUF_VA;
                if (cnt > 16) cnt = 16;
                for (i = 0; i < cnt; i++) {
                    DWORD sc = q[i * 3];
                    if (sc == 1u || sc == 0x1cu || sc == 0x31u || sc == 0x19u)
                        continue;
                    if (out != i) {
                        q[out * 3 + 0] = q[i * 3 + 0];
                        q[out * 3 + 1] = q[i * 3 + 1];
                        q[out * 3 + 2] = q[i * 3 + 2];
                    }
                    out++;
                }
                *(DWORD *)KEY_QUEUE_CNT_VA = out;
            }
            return;
        }
        if (g_gf_client_synced) {
            static DWORD replay_enter_tick;
            DWORD sim = (!IsBadReadPtr((void *)SIMSTATE_VA, 4)) ? *(DWORD *)SIMSTATE_VA : 0;
            DWORD smode = (!IsBadReadPtr((void *)SIMSTATE_MODE_VA, 4)) ? *(DWORD *)SIMSTATE_MODE_VA : 0;
            int in_replay = (sim != 0 && gf_read_int(GF_IN_SIM_VA, 0) == 0 &&
                             (smode == 3 || smode == 4));
            if (in_replay) {
                wc3g_state_pkt st;
                int have_st;
                if (replay_enter_tick == 0)
                    replay_enter_tick = GetTickCount();
                // Block independent client Replay / Continue / Quit keypresses
                if (!IsBadWritePtr((void *)KEY_QUEUE_CNT_VA, 4))
                    *(DWORD *)KEY_QUEUE_CNT_VA = 0;
                have_st = wc3net_gf_fetch_state(&st);
                if (have_st) {
                    gf_client_latch_events(&st);
                    if (!g_gf_pending_replay &&
                        (GetTickCount() - replay_enter_tick) > 600) {
                        if (st.script_act == 5 || (st.reserved & WC3G_HFLAG_IN_MISSION)) {
                            g_gf_pending_replay = 1;
                            g_gf_pending_replay_choice = 0;
                        } else if (st.reserved & WC3G_HFLAG_DECK_ACTIVE) {
                            g_gf_pending_replay = 1;
                            g_gf_pending_replay_choice = 1;
                        }
                    }
                }
                if (g_gf_pending_replay) {
                    int choice = g_gf_pending_replay_choice;
                    DWORD sc = (choice == 0) ? 0x15u : (choice == 1) ? 0x31u : 0x2du;
                    char rbuf[96];
                    g_gf_pending_replay = 0;
                    if (choice == 1) {
                        if (gf_dbg_snap_is_valid()) {
                            gf_dbg_snap_restore_mem();
                        } else if (have_st) {
                            if (!IsBadWritePtr((void *)GF_FLAGS_VA, sizeof(st.flags)))
                                CopyMemory((void *)GF_FLAGS_VA, st.flags, sizeof(st.flags));
                            if (!IsBadWritePtr((void *)GF_REGS_VA, sizeof(st.regs))) {
                                int saved_in_sim = gf_read_int(GF_IN_SIM_VA, 0);
                                CopyMemory((void *)GF_REGS_VA, st.regs, sizeof(st.regs));
                                gf_write_int(GF_IN_SIM_VA, saved_in_sim);
                            }
                        }
                    }
                    if (choice == 2 && !IsBadWritePtr((void *)KEY_LALT_DOWN_VA, 1))
                        *(BYTE *)KEY_LALT_DOWN_VA = 1;
                    if (!IsBadWritePtr((void *)KEY_QUEUE_BUF_VA, 12) &&
                        !IsBadWritePtr((void *)KEY_QUEUE_CNT_VA, 4)) {
                        ((DWORD *)KEY_QUEUE_BUF_VA)[0] = sc;
                        ((DWORD *)KEY_QUEUE_BUF_VA)[1] = 0;
                        ((DWORD *)KEY_QUEUE_BUF_VA)[2] = 1;
                        *(DWORD *)KEY_QUEUE_CNT_VA = 1;
                    }
                    wsprintfA(rbuf, "replay choice=%d (sc=0x%x) injected into Replay/Continue screen\r\n",
                              choice, (unsigned)sc);
                    log_ts_crit("gf_cli", rbuf);
                }
                return;
            }
            replay_enter_tick = 0;
            if (playing) {
                wc3g_state_pkt st;
                if (hd_tok != g_cli_hd_last_obj) {
                    g_cli_hd_last_obj = hd_tok;
                    g_cli_hd_seen_host = 0;
                    g_cli_hd_skip_latch = 0;
                    g_cli_hd_start_tick = GetTickCount();
                }
                if (wc3net_gf_fetch_state(&st)) {
                    gf_client_latch_events(&st);
                    if (st.reserved & (WC3G_HFLAG_IN_HD_MOV | WC3G_HFLAG_IN_MOVIE))
                        g_cli_hd_seen_host = 1;
                    if (g_gf_pending_skip) {
                        g_gf_pending_skip = 0;
                        g_cli_hd_skip_latch = 1;
                    } else if (g_gf_pending_branch || (st.reserved & WC3G_HFLAG_IN_BRANCH)) {
                        g_cli_hd_skip_latch = 1;
                    } else if (g_cli_hd_seen_host &&
                               !(st.reserved & (WC3G_HFLAG_IN_HD_MOV | WC3G_HFLAG_IN_MOVIE))) {
                        g_cli_hd_skip_latch = 1;
                    } else if (!g_cli_hd_seen_host &&
                               (GetTickCount() - g_cli_hd_start_tick) > 600 &&
                               !(st.reserved & (WC3G_HFLAG_IN_HD_MOV | WC3G_HFLAG_IN_MOVIE)) &&
                               ((st.reserved & (WC3G_HFLAG_DECK_ACTIVE | WC3G_HFLAG_IN_MISSION | WC3G_HFLAG_IN_SIM)) != 0 ||
                                st.script_act == 5 || st.script_act == 3)) {
                        // Host is already past this HD movie (back on deck or in mission/sim)
                        // before client observed WC3G_HFLAG_IN_HD_MOV; catch up cleanly.
                        g_cli_hd_skip_latch = 1;
                    }
                }
                if (g_cli_hd_skip_latch) {
                    if (!IsBadWritePtr((void *)GF_MOVIE_SKIP_VA, 1))
                        *(BYTE *)GF_MOVIE_SKIP_VA = 1;
                } else {
                    // Block independent client ESC / double-click skip during synced HD movie
                    if (!IsBadWritePtr((void *)GF_MOVIE_SKIP_VA, 1))
                        *(BYTE *)GF_MOVIE_SKIP_VA = 0;
                    if (!IsBadWritePtr(base + WC3EN_ESC_DOWN_OFF, 1))
                        *(BYTE *)(base + WC3EN_ESC_DOWN_OFF) = 0;
                    if (!IsBadWritePtr((void *)GF_KEYDOWN_ESC_VA, 1))
                        *(BYTE *)GF_KEYDOWN_ESC_VA = 0;
                }
            } else {
                if (g_cli_hd_skip_latch) {
                    // Clear DAT_004a24c4 once the HD movie stops so it doesn't
                    // leak into FUN_10032d30's bVar1 or the next cutscene segment.
                    if (!IsBadWritePtr((void *)GF_MOVIE_SKIP_VA, 1))
                        *(BYTE *)GF_MOVIE_SKIP_VA = 0;
                    g_cli_hd_skip_latch = 0;
                }
                g_cli_hd_last_obj = NULL;
                g_cli_hd_seen_host = 0;
            }
        }
    }
}

// Shared helper for hotspot click-release relays (0x401dc0 / 0x4133f0 / 0x413740).
// Host: executes click normally, and if it ran a room/NPC script (gump+0x44 != -1),
//       broadcasts WC3G_EVT_CLICK(gump_id = *(gump+0xc)).
// Client: blocks local clicks on room-navigation / NPC-conversation hotspots so
//         the client cannot move rooms or speak to characters on its own.
static void gf_handle_gump_release(void *gump, void *evt, gump_evt_fn orig, int pressed_off)
{
    int has_script = (gump && !IsBadReadPtr((BYTE *)gump + 0x44, 4) &&
                      *(int *)((BYTE *)gump + 0x44) != -1);
    int gump_id = (gump && !IsBadReadPtr((BYTE *)gump + 0xc, 4))
                  ? *(int *)((BYTE *)gump + 0xc) : -1;
    int on_deck = (gf_read_int(GF_SCRIPT_ACT_VA, -1) == 0 &&
                   gf_read_int(GF_SCENE_ID_VA, -1) != 9999);
    if (g_mode_client && !g_gf_in_replay) {
        // While on the Victory deck (FUN_00435300 sets DAT_004a3580 = 0),
        // block script hotspots (room moves, NPC talks, mission launch).
        // Non-script UI buttons (volume, options) still work.
        if (has_script && on_deck) {
            // Zero the class-specific pressed flag, then call orig so it
            // performs its own mouse-capture release (FUN_00482360) and flag
            // cleanup without executing the hotspot script.
            if (!IsBadWritePtr((BYTE *)gump + pressed_off, 4))
                *(DWORD *)((BYTE *)gump + pressed_off) = 0;
            orig(gump, evt);
            return;
        }
    }
    if (!g_mode_client && on_deck && g_mmenu.open != 0) {
        if (!IsBadWritePtr((BYTE *)gump + pressed_off, 4))
            *(DWORD *)((BYTE *)gump + pressed_off) = 0;
        orig(gump, evt);
        return;
    }
    if (!g_mode_client && has_script && on_deck) {
        // Only broadcast if this click will actually fire (gump pressed flag is set
        // and mouse button 1 was released). Never broadcast a stray release that
        // orig will ignore.
        int pressed = (!IsBadReadPtr((BYTE *)gump + pressed_off, 4) &&
                       *(DWORD *)((BYTE *)gump + pressed_off) != 0);
        int btn1 = (evt && !IsBadReadPtr((BYTE *)evt + 0xc, 4) &&
                    ((*(DWORD *)((BYTE *)evt + 0xc) & 0xffff) == 1));
        if (pressed && btn1) {
            int cur_rm = gf_read_int(GF_ROOM_ID_VA, 0);
            // Room 9 gump 3 ("Fly Simulator") sends msg 0x25 to the simulator listbox
            // (0x49bac0 -> FUN_00417cc0), which resolves DAT_004ab8dc from its private
            // listbox selection (+0xb4). Skip WC3G_EVT_CLICK here; simlist_cmd_relay
            // broadcasts WC3G_EVT_MISSION with the exact resolved mission ID instead.
            if (!(cur_rm == 9 && gump_id == 3)) {
                // Broadcast BEFORE calling orig, because orig may block inside a cutscene
                // FMV (FUN_0040fd20) or mission launch; client needs the trigger immediately
                // to play the cutscene in lockstep!
                gf_record_host_event(WC3G_EVT_CLICK, gump_id, cur_rm);
            }
        }
    }
    orig(gump, evt);
}

static void __attribute__((thiscall)) gump_rel1_relay(void *gump, void *evt)
{
    gf_handle_gump_release(gump, evt, g_gump_rel1_orig, 0x64);
}
static void __attribute__((thiscall)) gump_rel2_relay(void *gump, void *evt)
{
    gf_handle_gump_release(gump, evt, g_gump_rel2_orig, 0xa8);
}
static void __attribute__((thiscall)) gump_rel3_relay(void *gump, void *evt)
{
    gf_handle_gump_release(gump, evt, g_gump_rel3_orig, 0xac);
}
static void __attribute__((thiscall)) gump_rel4_relay(void *gump, void *evt)
{
    gf_handle_gump_release(gump, evt, g_gump_rel4_orig, 0x90);
}
static void __attribute__((thiscall)) gump_rel5_relay(void *gump, void *evt)
{
    gf_handle_gump_release(gump, evt, g_gump_rel5_orig, 0x90);
}
static void __attribute__((thiscall)) gump_rel6_relay(void *gump, void *evt)
{
    gf_handle_gump_release(gump, evt, g_gump_rel6_orig, 0x90);
}

// Hotspot keyboard shortcut relay (0x49b668 -> 0x4130b0).
// Host: records WC3G_EVT_CLICK on key-release activation of a script hotspot.
// Client: blocks keyboard shortcut activation of script hotspots on deck.
static void __attribute__((thiscall)) gump_key5_relay(void *gump, void *evt)
{
    int has_script = (gump && !IsBadReadPtr((BYTE *)gump + 0x44, 4) &&
                      *(int *)((BYTE *)gump + 0x44) != -1);
    int gump_id = (gump && !IsBadReadPtr((BYTE *)gump + 0xc, 4))
                  ? *(int *)((BYTE *)gump + 0xc) : -1;
    int key_state = (evt && !IsBadReadPtr((BYTE *)evt + 4, 4))
                    ? *(int *)((BYTE *)evt + 4) : 0;
    int on_deck = (gf_read_int(GF_SCRIPT_ACT_VA, -1) == 0 &&
                   gf_read_int(GF_SCENE_ID_VA, -1) != 9999);
    if (g_mode_client && has_script && on_deck)
        return;
    if (!g_mode_client && has_script && on_deck && key_state == 2) {
        int cur_rm = gf_read_int(GF_ROOM_ID_VA, 0);
        if (!(cur_rm == 9 && gump_id == 3)) {
            gf_record_host_event(WC3G_EVT_CLICK, gump_id, cur_rm);
        }
    }
    g_gump_key5_orig(gump, evt);
}

// Victory deck global hotkey relay (0x49b620 -> 0x4121c0).
// Tracks when the native Alt+X (0x82d) EXIT GAME prompt runs so our client ESC
// fallback in screen_tick_relay never double-prompts.
static void __attribute__((thiscall)) deck_key_relay(void *gump, void *evt)
{
    if (!g_mode_client &&
        (g_mmenu.open != 0 || (int)(g_mmenu.esc_swallow_until - GetTickCount()) > 0))
        return;
    if (evt && !IsBadReadPtr((BYTE *)evt + 0xc, 4)) {
        int key = *(int *)((BYTE *)evt + 0xc);
        if (key == 0x82d)
            g_last_exit_prompt_tick = GetTickCount();
        // On client, block Tab/Enter hotspot cursor cycling (0x0f, 0x20f, 0x1c)
        if (g_mode_client && (key == 0x0f || key == 0x20f || key == 0x1c))
            return;
    }
    g_deck_key_orig(gump, evt);
}

// Dialogue branch mouse-move relay (0x49b8bc -> 0x414e30).
static void __attribute__((thiscall)) branch_move_relay(void *branch, void *evt)
{
    if (g_mode_client) return;
    g_branch_move_orig(branch, evt);
}

// Dialogue branch choice relay (0x49b8c0 -> 0x414e70).
// FUN_00414e70 is __thiscall taking 1 stack arg (ret 4)! Using fastcall (ret 0)
// leaves 4 bytes on the caller's stack and crashes the host immediately after
// selecting a dialogue choice.
static void __attribute__((thiscall)) branch_choose_relay(void *branch, void *evt)
{
    if (g_mode_client && !g_gf_in_replay) {
        // Block client from picking dialogue branches independently
        return;
    }
    if (!g_mode_client && branch && !IsBadReadPtr((BYTE *)branch + 0x64, 4)) {
        int raw = *(int *)((BYTE *)branch + 0x64);
        int flipped = *(int *)((BYTE *)branch + 0x58);
        int logical = flipped ? !raw : raw;
        int bid = *(int *)((BYTE *)branch + 0x5c);
        gf_record_host_event(WC3G_EVT_BRANCH, logical, bid);
    }
    g_branch_choose_orig(branch, evt);
}

// Dialogue branch keyboard choice relay (0x49b8c8 -> 0x414cf0).
static void __attribute__((thiscall)) branch_key_relay(void *branch, void *evt)
{
    if (g_mode_client) return;
    if (branch && evt && !IsBadReadPtr((BYTE *)evt + 0xc, 4) &&
        *(int *)((BYTE *)evt + 4) == 1 && *(int *)((BYTE *)evt + 0xc) == 0x1c &&
        !IsBadReadPtr((BYTE *)branch + 0x64, 4)) {
        int raw = *(int *)((BYTE *)branch + 0x64);
        if (raw == 0 || raw == 1) {
            int flipped = *(int *)((BYTE *)branch + 0x58);
            int logical = flipped ? !raw : raw;
            int bid = *(int *)((BYTE *)branch + 0x5c);
            gf_record_host_event(WC3G_EVT_BRANCH, logical, bid);
        }
    }
    g_branch_key_orig(branch, evt);
}

// Save/Load dialog action relay (0x49b508 -> 0x40aa40).
static void __attribute__((thiscall)) saveload_act_relay(void *dlg, void *evt)
{
    int act = (evt && !IsBadReadPtr((BYTE *)evt + 4, 4)) ? *(int *)((BYTE *)evt + 4) : 0;
    int slot = (dlg && !IsBadReadPtr((BYTE *)dlg + 0xb4, 4)) ? *(int *)((BYTE *)dlg + 0xb4) : -1;
    int prev_load_ok = gf_read_int(GF_LOAD_OK_VA, 0);
    if (!g_mode_client && act == 0xc && slot >= 0 && prev_load_ok == 0) {
        // Mirror the savegame to the client synchronously BEFORE FUN_0040aa40 -> FUN_00408e70
        // modifies host DAT_004a3584/DAT_004ab84c. Otherwise gf_pump_thread can broadcast
        // the new scene/room in WC3G_MSG_STATE before the .WSG file arrives on the client,
        // causing a scene_sync/EVT_LOAD race.
        if (wc3net_gf_client_present()) {
            char dir[MAX_PATH], sname[24];
            gf_get_save_dir(dir, sizeof(dir));
            wsprintfA(sname, "%08d.WSG", slot);
            gf_host_mirror_one_save(dir, "WSG_NDX.WSG");
            gf_host_mirror_one_save(dir, sname);
        }
    }
    g_saveload_act_orig(dlg, evt);
    if (!g_mode_client && act == 0xc && slot >= 0 &&
        prev_load_ok == 0 && gf_read_int(GF_LOAD_OK_VA, 0) == 1) {
        // FUN_0040aa40 actually loaded this slot (DAT_0049ff08 transitioned 0 -> 1).
        // Save files were just mirrored above; broadcast WC3G_EVT_LOAD immediately.
        gf_record_host_event(WC3G_EVT_LOAD, slot, 0);
    }
}

// Simulator mission listbox click relay (0x49bad0 -> FUN_00417920, ret 4).
// Blocks independent client clicks on the simulator mission listbox.
static void __attribute__((thiscall)) simlist_click_relay(void *simlist, void *evt)
{
    if (g_mode_client) return;
    g_simlist_click_orig(simlist, evt);
}

// Simulator mission listbox command relay (0x49bb10 -> FUN_00417cc0, ret 4).
// Case 0x25 ("Fly Simulator") resolves DAT_004ab8dc (sim_misn) from the listbox's
// private selection (+0xb4) and sets DAT_004a3580 = 3, DAT_004a3590 = 4,
// DAT_004a3594 = 1. Broadcast WC3G_EVT_MISSION with the exact resolved mission ID
// so the client always launches the identical simulator mission (not its stale
// default listbox index 0).
static void __attribute__((thiscall)) simlist_cmd_relay(void *simlist, void *evt)
{
    int is_launch = (evt && !IsBadReadPtr((BYTE *)evt + 0xc, 4) &&
                     *(int *)((BYTE *)evt + 0xc) == -1 &&
                     *(int *)((BYTE *)evt + 4) == 0x25);
    g_simlist_cmd_orig(simlist, evt);
    if (!g_mode_client && is_launch &&
        gf_read_int(GF_SCRIPT_ACT_VA, 0) == 3 &&
        gf_read_int(GF_LEAVE_SCENE_VA, 0) == 1) {
        int sim_misn  = gf_read_int(GF_SIM_MISN_VA, 0);
        int sim_scene = gf_read_int(GF_SIM_SCENE_VA, gf_read_int(GF_SCENE_ID_VA, 0));
        gf_record_host_event(WC3G_EVT_MISSION, sim_misn, sim_scene);
    }
}

// Vtable-entry swap shared by both relays. The slot must still hold the
// expected original update before it points at ours. Vtable data only, no
// game code bytes are ever written. STANDING RULE. Callers capture the typed
// original before the first call: once the slot is swapped, the very next
// game dispatch runs the relay.
static int install_vtable_hook(BYTE **slot, DWORD expected, void *relay,
                               const char *tag)
{
    char buf[128];
    DWORD prot = 0;

    if (IsBadReadPtr(slot, 4) || (DWORD)(size_t)*slot != expected) {
        static int complained;
        if (!complained) {
            complained = 1;
            wsprintfA(buf, "vtable slot at %p holds %p, expected 0x%x, not hooking\r\n",
                      slot, IsBadReadPtr(slot, 4) ? NULL : *slot, expected);
            log_crit(buf);
        }
        return 0;
    }

    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &prot)) {
        wsprintfA(buf, "vtable virtualprotect failed %lu\r\n", GetLastError());
        log_crit(buf);
        return 0;
    }
    *slot = (BYTE *)relay;
    VirtualProtect(slot, 4, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), slot, 4);
    // game base was verified 0x400000, so the expected VA is the original's
    // actual address
    wsprintfA(buf, "relay installed, slot %p -> %s %p, original 0x%lx\r\n",
              slot, tag, relay, expected);
    log_line(buf);
    return 1;
}

void wc3net_install(HMODULE game)
{
    char buf[128];
    int attempt;
    BYTE **slot;        // vtable slot to swap, PLYR in client mode, FGTR otherwise
    DWORD expected;     // original update the slot must still hold
    void *relay;
    const char *tag;

    if ((DWORD)(size_t)game != GAME_BASE_EXPECTED) {
        wsprintfA(buf, "unexpected game base %p, hook not installed\r\n", game);
        log_crit(buf);
        return;
    }
    // Poll instead of one long sleep: a single Sleep(90000) hung once under
    // CrossOver, thread never woke, relay never engaged. Retry every 5 s for
    // up to 10 min; install_vtable_hook logs its own refusal reason and
    // returns nonzero only on success.
    read_config();
    if (g_mode_off) {
        log_crit("mode=off, proxy acting as pure pass-through (single-player)\r\n");
        return;
    }
    {
        static int veh_installed;
        LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(wc3net_crash_filter);
        if (prev != wc3net_crash_filter)
            g_prev_uef = prev;
        if (!veh_installed) {
            AddVectoredExceptionHandler(1, wc3net_veh);
            veh_installed = 1;
        }
    }
    if (g_mode_client) {
        wsprintfA(buf, "client mode, host=%s port=%d\r\n", g_host, g_port);
        log_crit(buf);
    }
    // One seam discipline, two targets: mode=client swaps the PLYR player
    // update to ship input out, every other mode swaps the FGTR wingman
    // update as before. The typed original is captured here, before any
    // swap attempt can succeed.
    if (g_mode_client) {
        slot = (BYTE **)((BYTE *)game + (PLYR_VTABLE_VA - GAME_BASE_EXPECTED));
        expected = PLYR_UPDATE_EXPECTED;
        relay = (void *)&plyr_relay;
        tag = "plyr_relay";
        g_plyr_original = (plyr_fn)((BYTE *)game + (PLYR_UPDATE_VA - GAME_BASE_EXPECTED));
    } else {
        slot = (BYTE **)((BYTE *)game + (FGTR_VTABLE_VA - GAME_BASE_EXPECTED));
        expected = FGTR_UPDATE_EXPECTED;
        relay = (void *)&fgtr_relay;
        tag = "fgtr_relay";
        g_original = (fgtr_fn)((BYTE *)game + (FGTR_UPDATE_VA - GAME_BASE_EXPECTED));
    }
    // Defer 15 s in 1 s slices so wc3en finishes load-time patching before
    // vtable swaps run, without forcing a long wait on launch.
    // Stage 1: install screen_slam_relay as soon as Screen/viewport exist so
    // the connection status banner appears immediately after intro movies.
    for (attempt = 0; attempt < 15; attempt++) {
        Sleep(1000);
        if (GetModuleHandleA("WC3W.EXE") != game) {
            log_crit("module check failed during deferral, not hooking\r\n");
            return;
        }
        if (!g_slam_hooked && !g_net_disabled &&
            (g_mode_client || g_mode_remote || g_mode_observe_net) &&
            gf_read_int(UI_SCREEN_OBJ_VA, 0) != 0 &&
            gf_read_int(UI_VIEWPORT_VA, 0) != 0) {
            HMODULE h_wc3en = GetModuleHandleA("wc3en.dll");
            if (h_wc3en) g_wc3en_base = (BYTE *)h_wc3en;
            g_screen_slam_orig = (screen_slam_fn)SCREEN_SLAM_EXP;
            if (install_vtable_hook((BYTE **)SCREEN_SLAM_SLOT_VA, SCREEN_SLAM_EXP,
                                    (void *)&screen_slam_relay, "screen_slam_relay")) {
                g_slam_hooked = 1;
            }
        }
    }
    log_line("deferral done, polling for install\r\n");
    for (attempt = 1; attempt <= 120; attempt++) {
        if (GetModuleHandleA("WC3W.EXE") != game) {
            log_crit("module check failed, not hooking\r\n");
            return;
        }
        if (install_vtable_hook(slot, expected, relay, tag)) {
            // Install fighter WEAP vtable slot 1 hook on both host and client
            // (vtable data only, no code bytes written).
            g_weap_original = (weap_fn)((BYTE *)game + (WEAP_UPDATE_VA - GAME_BASE_EXPECTED));
            install_vtable_hook(
                (BYTE **)((BYTE *)game + (WEAP_VTABLE_SLOT_VA - GAME_BASE_EXPECTED)),
                WEAP_UPDATE_EXPECTED, (void *)&weap_relay, "weap_relay");
            // Install NAVMAP camera vtable hooks (+0x14 enter modal, +0x18 per-frame tick)
            g_navmap_enter_orig = (navmap_enter_fn)NAVMAP_ENTER_EXP;
            install_vtable_hook((BYTE **)NAVMAP_ENTER_SLOT_VA, NAVMAP_ENTER_EXP,
                                (void *)&navmap_enter_relay, "navmap_enter_relay");
            g_navmap_tick_orig = (navmap_tick_fn)NAVMAP_TICK_EXP;
            install_vtable_hook((BYTE **)NAVMAP_TICK_SLOT_VA, NAVMAP_TICK_EXP,
                                (void *)&navmap_tick_relay, "navmap_tick_relay");
            if (g_mode_client) {
                // Client-side DMGA relays: prevent divergent local damage from
                // prematurely killing enemies or host Blair while still alive on host.
                g_dmga_fgtr_orig = (dmga_fn)DMGA_FGTR_EXPECTED;
                install_vtable_hook((BYTE **)DMGA_FGTR_SLOT_VA, DMGA_FGTR_EXPECTED,
                                    (void *)&dmga_fgtr_relay, "dmga_fgtr_relay");
                g_dmga_cptl_orig = (dmga_fn)DMGA_CPTL_EXPECTED;
                install_vtable_hook((BYTE **)DMGA_CPTL_SLOT_VA, DMGA_CPTL_EXPECTED,
                                    (void *)&dmga_cptl_relay, "dmga_cptl_relay");
            }
            // Unit 6: Install out-of-mission Victory / gameflow / cutscene hooks
            // (all vtable data swaps, zero game code bytes modified).
            if (!g_gf_evt_lock_init) {
                InitializeCriticalSection(&g_gf_evt_lock);
                g_gf_evt_lock_init = 1;
            }
            g_screen_tick_orig = (screen_tick_fn)SCREEN_TICK_EXP;
            install_vtable_hook((BYTE **)SCREEN_TICK_SLOT_VA, SCREEN_TICK_EXP,
                                (void *)&screen_tick_relay, "screen_tick_relay");
            if (!g_slam_hooked) {
                g_screen_slam_orig = (screen_slam_fn)SCREEN_SLAM_EXP;
                if (install_vtable_hook((BYTE **)SCREEN_SLAM_SLOT_VA, SCREEN_SLAM_EXP,
                                        (void *)&screen_slam_relay, "screen_slam_relay")) {
                    g_slam_hooked = 1;
                }
            }
            g_gump_rel1_orig = (gump_evt_fn)GUMP_REL1_EXP;
            install_vtable_hook((BYTE **)GUMP_REL1_SLOT_VA, GUMP_REL1_EXP,
                                (void *)&gump_rel1_relay, "gump_rel1_relay");
            g_gump_rel2_orig = (gump_evt_fn)GUMP_REL2_EXP;
            install_vtable_hook((BYTE **)GUMP_REL2_SLOT_VA, GUMP_REL2_EXP,
                                (void *)&gump_rel2_relay, "gump_rel2_relay");
            g_gump_rel3_orig = (gump_evt_fn)GUMP_REL3_EXP;
            install_vtable_hook((BYTE **)GUMP_REL3A_SLOT_VA, GUMP_REL3_EXP,
                                (void *)&gump_rel3_relay, "gump_rel3a_relay");
            install_vtable_hook((BYTE **)GUMP_REL3B_SLOT_VA, GUMP_REL3_EXP,
                                (void *)&gump_rel3_relay, "gump_rel3b_relay");
            install_vtable_hook((BYTE **)GUMP_REL3C_SLOT_VA, GUMP_REL3_EXP,
                                (void *)&gump_rel3_relay, "gump_rel3c_relay");
            install_vtable_hook((BYTE **)GUMP_REL3D_SLOT_VA, GUMP_REL3_EXP,
                                (void *)&gump_rel3_relay, "gump_rel3d_relay");
            g_gump_rel4_orig = (gump_evt_fn)GUMP_REL4_EXP;
            install_vtable_hook((BYTE **)GUMP_REL4_SLOT_VA, GUMP_REL4_EXP,
                                (void *)&gump_rel4_relay, "gump_rel4_relay");
            g_gump_rel5_orig = (gump_evt_fn)GUMP_REL5_EXP;
            install_vtable_hook((BYTE **)GUMP_REL5_SLOT_VA, GUMP_REL5_EXP,
                                (void *)&gump_rel5_relay, "gump_rel5_relay");
            g_gump_rel6_orig = (gump_evt_fn)GUMP_REL6_EXP;
            install_vtable_hook((BYTE **)GUMP_REL6_SLOT_VA, GUMP_REL6_EXP,
                                (void *)&gump_rel6_relay, "gump_rel6_relay");
            g_gump_key5_orig = (gump_evt_fn)GUMP_KEY5_EXP;
            install_vtable_hook((BYTE **)GUMP_KEY5_SLOT_VA, GUMP_KEY5_EXP,
                                (void *)&gump_key5_relay, "gump_key5_relay");
            g_deck_key_orig = (gump_evt_fn)DECK_KEY_EXP;
            install_vtable_hook((BYTE **)DECK_KEY_SLOT_VA, DECK_KEY_EXP,
                                (void *)&deck_key_relay, "deck_key_relay");
            g_branch_move_orig = (gump_evt_fn)BRANCH_MOVE_EXP;
            install_vtable_hook((BYTE **)BRANCH_MOVE_SLOT, BRANCH_MOVE_EXP,
                                (void *)&branch_move_relay, "branch_move_relay");
            g_branch_choose_orig = (branch_choose_fn)BRANCH_CHOOSE_EXP;
            install_vtable_hook((BYTE **)BRANCH_CHOOSE_SLOT, BRANCH_CHOOSE_EXP,
                                (void *)&branch_choose_relay, "branch_choose_relay");
            g_branch_key_orig = (gump_evt_fn)BRANCH_KEY_EXP;
            install_vtable_hook((BYTE **)BRANCH_KEY_SLOT, BRANCH_KEY_EXP,
                                (void *)&branch_key_relay, "branch_key_relay");
            g_movie_frame_orig = (movie_frame_fn)MOVIE_FRAME_EXP;
            install_vtable_hook((BYTE **)MOVIE_FRAME_SLOT_VA, MOVIE_FRAME_EXP,
                                (void *)&movie_frame_relay, "movie_frame_relay");
            g_saveload_act_orig = (gump_evt_fn)SAVELOAD_ACT_EXP;
            install_vtable_hook((BYTE **)SAVELOAD_ACT_SLOT, SAVELOAD_ACT_EXP,
                                (void *)&saveload_act_relay, "saveload_act_relay");
            g_simlist_click_orig = (gump_evt_fn)SIMLIST_CLICK_EXP;
            install_vtable_hook((BYTE **)SIMLIST_CLICK_SLOT, SIMLIST_CLICK_EXP,
                                (void *)&simlist_click_relay, "simlist_click_relay");
            g_simlist_cmd_orig = (gump_evt_fn)SIMLIST_CMD_EXP;
            install_vtable_hook((BYTE **)SIMLIST_CMD_SLOT, SIMLIST_CMD_EXP,
                                (void *)&simlist_cmd_relay, "simlist_cmd_relay");
            {
                HMODULE h_wc3en = GetModuleHandleA("wc3en.dll");
                if (h_wc3en) {
                    g_wc3en_base = (BYTE *)h_wc3en;
                    g_wc3en_poll_orig = (poll_input_fn)WC3EN_POLL_INPUT_EXP;
                    install_vtable_hook((BYTE **)(g_wc3en_base + WC3EN_POLL_INPUT_OFF),
                                        WC3EN_POLL_INPUT_EXP,
                                        (void *)&wc3en_poll_input_relay,
                                        "wc3en_poll_input_relay");
                }
            }
            {
                LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(wc3net_crash_filter);
                if (prev != wc3net_crash_filter)
                    g_prev_uef = prev;
            }
            if (g_mode_client) {
                // no listener in client mode, this process only ships input
                if (!g_net_disabled) {
                    if (wc3net_client_start(g_host, g_port))
                        wsprintfA(buf, "client sender up, udp %s:%d\r\n", g_host, g_port);
                    else
                        wsprintfA(buf, "client sender FAILED %lu, input stays local\r\n",
                                  GetLastError());
                } else {
                    wsprintfA(buf, "client sender skipped, net=off\r\n");
                }
                log_crit(buf);
                // WC3S returns arrive at the tx socket's ephemeral source
                // port; net=off leaves this thread unstarted, same meaning
                // as the sender skip above
                if (!g_net_disabled) {
                    wsprintfA(buf, wc3net_state_start_rx()
                        ? "client state rx up\r\n"
                        : "client state rx FAILED, cockpit stays local\r\n");
                    log_crit(buf);
                    CreateThread(NULL, 0, gf_pump_thread, NULL, 0, NULL);
                }
            } else if ((g_mode_remote || g_mode_observe_net) && !g_net_disabled) {
                if (wc3net_net_start(g_port))
                    wsprintfA(buf, "net listener up, udp/%d, remote mode\r\n", g_port);
                else
                    wsprintfA(buf, "net listener FAILED %lu, remote input dead\r\n",
                              GetLastError());
                log_crit(buf);
                CreateThread(NULL, 0, gf_pump_thread, NULL, 0, NULL);
                if (g_mode_remote && InterlockedCompareExchange(&g_state_pump_started, 1, 0) == 0)
                    CreateThread(NULL, 0, state_pump, NULL, 0, NULL);
            }
            return;
        }
        if (attempt % 12 == 1)
            log_line("install attempt pending\r\n");
        Sleep(5000);
    }
    log_crit("install gave up after 120 attempts\r\n");
}

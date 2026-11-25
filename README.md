# WC3 Co-op

This mod adds two-player co-op to *Wing Commander III: Heart of the Tiger* on
the GOG *30th Anniversary Edition* (Kilrathi Saga Windows engine, `WC3W.EXE`).

One player hosts and controls Blair. The second player connects from a separate
game install and flies the mission's wingman in place of the AI pilot.
Outside missions, both players stay in sync across the TCS Victory decks, cutscenes,
dialogue choices, savegames, and the flight simulator. This is a personal-use project.

## Legal boundary and copyright

- Electronic Arts owns *Wing Commander III*, its binaries, data files, script
  dialogue, movies, and assets.
- This repository contains my own code and notes only. It includes the
  `XanLib.dll` proxy source (`src/wc3net/`), build and install scripts
  (`scripts/`), and my own technical notes.
- No commit of any copyrighted game material. The repository holds no game
  binaries, no game assets, no game dialogue or scripts, and no decompiled or
  disassembled game code.
- You must own a legal copy of the game. The mod does not include the game and
  cannot run without it.
- `wc3en.dll` (Matt Wells' patch) is not part of this project. The proxy works
  alongside it, including its LibVLC HD movie playback, and leaves it
  untouched.

## Features (v0.11.15)

### In-mission co-op

- **Full wingman control at 60 Hz.** The client flies the mission's wingman
  with their own joystick or keyboard through the game's native input pipeline,
  while the host streams own-ship physics and all live ships in the mission at
  60 Hz.
- **Host craft visibility and callsign.** The client renders the host's ship in
  its world and labels it `MAVERICK` on the HUD and MFD.
- **Autopilot and sector transitions.** Both players enter autopilot and landing
  cutscenes together under host control.
- **Wingman death, eject, and spectator chase cam.** Ejecting or losing your
  wingman plays the native eject or destruction sequence, then switches the
  client to an external chase camera following host Blair until the mission
  ends.
- **Mission outcome and death screen sync.** Campaign mission outcomes,
  mission flags, and Replay, Continue, or Quit choices on the death screen
  replicate from host to client.

### Out-of-mission campaign, simulator, and tools

- **Victory deck and cutscene lockstep.** Room navigation, crew conversations,
  dialogue branch choices, and cutscene skips replicate from host to client.
- **Savegame mirroring.** Saving or loading on the host mirrors the save
  files to the client. This overwrites matching savegame slots on the client,
  so back up any single-player saves first.
- **Flight simulator sync.** Launching any simulator mission from the Default,
  Black Box, or Alternate tabs launches the same mission on both machines.
- **Connection status banner.** After intro movies finish, an on-screen banner
  (`WAITING FOR CLIENT...` on the host, `CONNECTING TO HOST...` on the client)
  shows connection status until the peer joins.
- **Host debug mission menu (`F9`).** Pressing `F9` on the Victory deck opens a
  mission selector for all 65 campaign and simulator missions with custom ship,
  wingman, pilot, loadout, and mission flag overrides, launching both players
  together and returning to the prior campaign state afterward.
- **Configurable logging.** `wc3net.log` supports `debug`, `critical`, and
  `off` verbosity modes and retains at most the last 2 sessions.

## How it works

The mod uses a host-authoritative state-stomp architecture.

- **Proxy DLL injection.** The game loads our proxy as `XanLib.dll`, one of its
  own imports. The proxy forwards all 22 original exports to the real library
  renamed `XanLibReal.dll`. `scripts/make_proxy_def.sh` regenerates the
  forwarder table from the game's import table.
- **Vtable and data hooks only.** The proxy never writes game code bytes.
  Patching live code pages under CrossOver corrupts the translation layer,
  while vtable slot swaps remain stable.
- **Host (`mode=remote`).** The host hooks the `FGTR` wingman update slot,
  flight model, weapons, navmap, and Victory deck UI slots. It applies client
  input while keeping wingman AI disengaged, streams `WC3S` mission snapshots
  at 60 Hz, and broadcasts `WC3G` campaign and savegame state at roughly 10 Hz.
- **Client (`mode=client`).** The client hooks the `PLYR` update slot, damage
  and weapons relays, navmap, and Victory deck UI slots. It sends its
  calibrated control block at 60 Hz (`WC3N`), overwrites its local world from
  `WC3S`, and replays host `WC3G` campaign, cutscene, and savegame events. One
  simulation runs, on the host.
- **Network transport.** All traffic shares one UDP port (`3443` by default),
  demultiplexed by a 4-byte magic prefix (`WC3N` for input, `WC3S` for mission
  state, `WC3G` for campaign and savegame sync). Mailboxes keep the newest
  packet, switch cleanly when the sender changes, and fall back to wingman AI
  after 1 second without input.

## Requirements

### Both machines (host and client)

- A legal copy of *Wing Commander III: Heart of the Tiger*, GOG *30th
  Anniversary Edition* (`Kilrathi_Saga` Windows build).
- Verify that `WC3W.EXE` matches the supported binary.
  - On Windows (Command Prompt):
    ```bat
    certutil -hashfile WC3W.EXE SHA256
    ```
  - On macOS (Terminal):
    ```sh
    shasum -a 256 WC3W.EXE
    ```
  - Expected SHA-256:
    `1fffd1840356ea09b4e475c6648eda98e9485e1e421336dc927a25db274cd91f`
- Either machine (Windows, or macOS with CrossOver or Wine) can act as host or
  client.
- Both machines must sit on the same LAN, or on a UDP-routable network.
- Both machines must run the same build of `XanLib.dll`.
- A joystick configured in the game's setup UI works best, though keyboard and
  mouse also pass through the game's input pipeline.

### Build toolchain (only if compiling from source)

If you use a pre-built release (`XanLib.dll` and `wc3net.ini`), skip this
section and go straight to [Installation and setup](#installation-and-setup).

- **macOS or Linux.** Install a 32-bit MinGW-w64 cross-compiler
  (`brew install mingw-w64` provides `i686-w64-mingw32-gcc`).
- **Windows.** Install a 32-bit (`i686`) MinGW-w64 GCC, such as MSYS2
  (`winget install MSYS2.MSYS2`, then `pacman -S --needed mingw-w64-i686-gcc`)
  or place `i686-w64-mingw32-gcc` on `PATH`.

## Building from source

### macOS or Linux (cross-compile)

Build the proxy DLL from `src/wc3net/`:

```sh
cd src/wc3net
i686-w64-mingw32-gcc -shared -o XanLib.dll dllmain.c hook.c net.c \
    xanlib_fwd.def -Wall -lws2_32
```

Confirm the build has no warnings and exports all 22 forwarded names:

```sh
i686-w64-mingw32-objdump -p XanLib.dll | grep Forwarder
```

### Windows (native)

Run from the repository root:

```bat
scripts\build_windows.bat
```

The script finds `i686-w64-mingw32-gcc` or MSYS2
`C:\msys64\mingw32\bin\gcc.exe`, compiles `src\wc3net\XanLib.dll`, and copies
it to `lancopy\`.

## Installation and setup

> [!WARNING]
> **Back up your client savegames before playing.** Because host save and load
> actions synchronize to the client, existing single-player save files on the
> client (`.WSG` files and `WSG_NDX.WSG`) can be overwritten and permanently
> lost. Copy your existing savegames to a separate folder first if you want to
> keep them safe.

Do not run GOG Galaxy "Verify / Repair" while the proxy is installed, because
GOG Galaxy will overwrite `XanLib.dll`. Restore the original DLL first if you
need to verify game files.

### Option 1: Pre-built release (`XanLib.dll` and `wc3net.ini`)

If you downloaded a pre-built release archive containing `XanLib.dll` and
`wc3net.ini`, you do not need a compiler or build scripts. On both host and
client machines, inside the game's `Kilrathi_Saga` folder (the folder
containing `WC3W.EXE`):

1. Rename the original `XANLIB.DLL` to `XanLibReal.dll`.
2. Copy the pre-built `XanLib.dll` and `wc3net.ini` into `Kilrathi_Saga`.
3. Edit `wc3net.ini` for each machine's role:
   - On the **host**, set `mode=remote`.
   - On the **client**, set `mode=client` and `host=<HOST_LAN_IP>`.
4. Launch `WC3W.EXE` on both machines.
5. To uninstall later, delete the proxy `XanLib.dll` and rename
   `XanLibReal.dll` back to `XANLIB.DLL`.

### Option 2: Windows (automated scripts)

The Windows scripts check common GOG `Kilrathi_Saga` install paths. You can
also pass the game folder as an argument or set `WC3_GAME_DIR`.

```bat
REM 1. Install the proxy (backs up XANLIB.DLL to XanLibReal.dll, copies XanLib.dll and default wc3net.ini)
scripts\install_proxy_windows.bat

REM 2. Edit <Kilrathi_Saga>\wc3net.ini (set mode=remote on host, or mode=client and host=<HOST_LAN_IP> on client)

REM 3. Launch the game (updates the proxy, clears wc3net.log, starts WC3W.EXE)
scripts\start_windows.bat                REM uses mode from wc3net.ini
scripts\start_windows.bat host           REM sets WC3NET_MODE=remote
scripts\start_windows.bat client         REM sets WC3NET_MODE=client
scripts\start_windows.bat singleplayer   REM sets WC3NET_MODE=off (vanilla single-player)

REM Revert to vanilla at any time
scripts\restore_proxy_windows.bat
```

### Option 3: macOS and CrossOver (automated scripts)

1. Check the `GAME=` path at the top of `scripts/install_proxy.sh` and
   `scripts/restore_proxy.sh`. Update it if your CrossOver bottle or game path
   differs.
2. Run the install script:
   ```sh
   scripts/install_proxy.sh     # renames XANLIB.DLL to XanLibReal.dll and copies XanLib.dll
   ```
3. Create `wc3net.ini` in the same `Kilrathi_Saga` folder (see
   [Configuration](#configuration)).
4. Revert to vanilla at any time:
   ```sh
   scripts/restore_proxy.sh
   ```

### Option 4: Manual installation from source build (Windows, macOS, or Wine)

In the game's `Kilrathi_Saga` folder (next to `WC3W.EXE`):

1. Rename the original `XANLIB.DLL` to `XanLibReal.dll`.
2. Copy your built `src/wc3net/XanLib.dll` into the folder.
3. Create `wc3net.ini` next to `XanLib.dll` using the examples below.
4. To uninstall later, delete the proxy `XanLib.dll` and rename
   `XanLibReal.dll` back to `XANLIB.DLL`.

### Firewall and network setup

- Find the host machine's LAN IPv4 address (for example using `ipconfig /all` on Windows or
  `ipconfig getifaddr en0` on macOS) and set `host=<HOST_LAN_IP>` in the
  client's `wc3net.ini`.
- If necessary, allow inbound UDP port `3443` (or your configured `port`) for `WC3W.EXE` in
  Windows Defender Firewall, or in the macOS firewall if enabled.
- If either machine runs a VPN (such as WireGuard or Tailscale) that captures
  LAN routes, exclude the local subnet or add a route between the two peers.
- `wc3en.dll` is optional on both sides. Vanilla installs and `wc3en.dll`
  installs both work.

## Configuration

### `wc3net.ini` (host and client)

The proxy reads `wc3net.ini` once on DLL attach from the same directory as
`XanLib.dll` (`Kilrathi_Saga\`). Place all keys under the `[wc3net]` section.

#### Complete key reference

| Key | Applies to | Type and allowed values | Default | Environment override | Description |
| --- | --- | --- | --- | --- | --- |
| `mode` | Both | `remote`, `client`, `off` (aliases `singleplayer`, `vanilla`, `disabled`), `script`, `observe`, or `observenet` | `off` (fallback if omitted; `lancopy/wc3net.ini` defaults to `remote`) | `WC3NET_MODE` (case-insensitive) | Operating role of this game instance. See mode details below. |
| `host` | Client (`mode=client`) | Literal IPv4 address up to 63 characters (for example `192.168.1.100`). No DNS or hostnames. | Empty | None | Host IPv4 address that receives client `WC3N` and `WC3G` UDP packets. Ignored on host. |
| `port` | Both | Integer UDP port (`1` to `65535`) | `3443` | None | UDP port shared by `WC3N`, `WC3S`, and `WC3G`. Must match on host and client. |
| `net` | Both | `1` (or any non-zero integer) to enable, `0` to disable | `1` | None | Disables all UDP sockets and network threads when set to `0`. Keeps input and cockpit local for offline diagnostics. |
| `auth` | Host (`mode=remote`) | Integer `1` to `4` (values outside this range clamp to `1` or `4`) | `2` | None | Axis authority multiplier applied to incoming client pitch, lateral, and rudder inputs on the host. Leave at `2` for normal play. |
| `logging` | Both | `debug`, `critical` (alias `crit`), or `off` (aliases `none`, `0`) | `off` | `WC3NET_LOGGING` (case-insensitive) | Controls `wc3net.log` verbosity. Any unrecognized value falls back to `debug`. |

#### `mode` values

- **`remote` (host, standard template).** Listens on UDP `port`, applies client `WC3N` input to
  the mission wingman, streams `WC3S` state and world snapshots at 60 Hz, and
  broadcasts `WC3G` campaign, cutscene, and savegame sync.
- **`client` (client).** Sends local player `WC3N` input to `host:port`,
  receives `WC3S` state and world updates plus `WC3G` campaign and savegame
  sync, and renders the wingman's cockpit (or Blair's chase camera while
  spectating).
- **`off` (or `singleplayer`, `vanilla`, `disabled`; fallback).** Disables all
  hooks, overlays, and network threads so the proxy acts as a pure pass-through
  to `XanLibReal.dll`. Also used as the fallback if `wc3net.ini` is missing or
  `mode` is omitted or unrecognized. Use this to play vanilla single-player
  without uninstalling `XanLib.dll`.
- **`script` (diagnostic).** Runs a built-in scripted pitch, throttle,
  and fire test pattern on the wingman through the host `FGTR` hook with no
  network sockets.
- **`observe` (diagnostic).** Read-only local mode. Hooks the `FGTR` update and
  logs telemetry without writing game state or opening sockets.
- **`observenet` (diagnostic).** Read-only network mode. Starts the UDP
  listener and logs incoming packets without writing game state.

#### `logging` values

The DLL writes `wc3net.log` next to `XanLib.dll` and trims older sessions on
startup to keep at most the last 2 sessions.

- **`debug`.** Logs critical events plus 1 to 10 Hz telemetry
  (`walk`, `mirror`, `stomp`, `world`, `watch`, `tick#`, `rx#`, `client#`,
  `flight#`, and 5-second `gf_host` heartbeats).
- **`critical` (or `crit`).** Logs critical events only. These include crashes
  (`VEH` and `UEF`), attach, hook, and network startup or errors, peer
  connection and sync, wingman capture, respawn, death, eject, and mission end,
  discrete campaign and cutscene events, savegame transfers, `F9` debug mission
  launches and snapshot restores, and CD mounts.
- **`off` (or `none`, `0`; default).** Disables `wc3net.log` writes and log rotation.

#### Host example

```ini
[wc3net]
mode=remote
port=3443
net=1
auth=2
logging=critical
```

#### Client example

```ini
[wc3net]
mode=client
host=192.168.1.100
port=3443
net=1
logging=critical
```

#### Environment variable overrides

- `WC3NET_MODE` overrides `mode` (for example `WC3NET_MODE=client`). Two
  instances in the same Wine or CrossOver bottle can run as host and client
  from a single `wc3net.ini`.
- `WC3NET_LOGGING` overrides `logging` (for example `WC3NET_LOGGING=debug`).

## Running a co-op session

1. Install the same `XanLib.dll` build on both machines and configure
   `wc3net.ini` (`mode=remote` on host, `mode=client` and `host=<HOST_LAN_IP>`
   on client).
2. Launch `WC3W.EXE` on both machines.
3. Wait through the 15-second startup hook deferral after launch so `wc3en.dll`
   can finish load-time patching before vtable hooks install. After intro
   movies finish, the host displays `WAITING FOR CLIENT...` and the client
   displays `CONNECTING TO HOST...`. Both banners disappear once the peers
   connect.
4. On the TCS Victory, the host navigates rooms, talks to crew, picks dialogue
   branches, saves or loads games, and launches campaign or simulator missions.
   The client follows in lockstep.
5. In flight, the client steers the wingman (pitch, roll and yaw, throttle,
   guns, missiles, afterburner, and eject). If client input stops for about 1
   second, the wingman falls back to AI and returns to client control when
   input resumes.
6. On the Victory deck, the host can press `F9` to open the debug mission menu
   and launch any of the 65 campaign or simulator missions with custom ship,
   wingman, or flag overrides. Both machines restore their pre-debug campaign
   state when returning to the Victory deck.

### Testing without a second machine

- **Same-bottle loopback.** Launch a second instance in the same Wine or
  CrossOver bottle with `WC3NET_MODE=client`.
- **Wire check with the game closed.** Run `python3 scripts/wc3n_check.py 3443 150`
  on the host to verify that `WC3N` UDP packets reach the host.

## Diagnostics (`wc3net.log`)

The proxy writes `wc3net.log` in the game directory unless `logging=off`.

- **Host lines.** `attach` (pid and game base), `relay installed` (vtable
  swaps), `net listener up`, `possession target captured`, `ai disengaged`,
  `rx#` (accepted client input and source address), `remote#` (applied frames),
  `mirror` and `walk` (1 Hz world and registry health), and `gf` (gameflow,
  savegame mirroring, CD switches, debug menu, and snapshot restore).
- **Client lines.** `client sender up`, `client state rx up`, `client#` (1 Hz
  outgoing input sample), `stomp` (1 Hz own-ship state), `world` (1 Hz world
  reconcile), and `gf` (room, scene, cutscene, savegame, and replay sync).
- **Common faults.**
  - `rx=0` for several minutes points to a network block. Check `host=` in
    `wc3net.ini`, the host UDP `3443` firewall rule, and any active VPN routes.
  - `72 B WC3S from a pre-v0.10 host, upgrade both sides` means the host and
    client run mismatched `XanLib.dll` builds. Copy the same DLL to both
    machines.
  - `vtable slot ... holds ... expected` means `WC3W.EXE` does not match the
    supported SHA-256 binary.

## Repository layout

```text
src/wc3net/        proxy DLL: dllmain.c (attach), hook.c (vtable relays,
                   world/gameflow sync, spectator cam, navmap, F9 debug menu),
                   net.c (UDP sockets, mailboxes, savegame transfer),
                   net.h (WC3N, WC3S, and WC3G wire structs),
                   missions.h (65-mission catalog and CD mapping),
                   xanlib_fwd.def (22 XanLibReal forwarders)
scripts/           install/restore/make_proxy_def (macOS), *_windows.bat
                   (Windows build/install/restore/start), wc3n_check.py
                   wire-proof listener
lancopy/           staged LAN bundle templates (wc3net.ini, README.txt;
                   compiled XanLib.dll is built locally)
```

## Engineering rules encoded in this codebase

- Never write game code bytes. Use vtable slots and data fields only.
- Match the original function's full calling convention and return value in
  every relay across every path.
- Defer main hook installation for 15 seconds in 1-second slices after attach.
  Never use one long `Sleep()`, which can hang under CrossOver.
- Resolve `wc3net.ini` and `wc3net.log` against the DLL's own directory.
- Use source-aware mailbox acceptance. Sequence numbers are per-sender, so a
  changed sender always wins.

## Attribution and reuse

Any reuse, rework, or republishing of this work requires crediting the author.


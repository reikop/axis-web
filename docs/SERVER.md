# The online path: what a private server would take (reverse-engineering notes)

The offline port (see the top-level `README.md`) is complete for everything that runs
client-side without a live match. This document consolidates the reverse-engineering of the
**online path** — why the original client can't reach an in-match state offline, and the exact
entry points a private-server / launcher reimplementation would need. It is a map for a future
project, not a working server. All addresses are static (image base `0x400000`, no ASLR, so
static = runtime). Everything here was read from the client or measured at runtime with Frida;
nothing is guessed.

## Why in-match is unreachable offline (measured root cause)

AXIS is a **two-process** title: a separate **`AxisPlay.exe` launcher** performs login,
server/channel selection and NetSphere authentication, then hands the 3D client **`AXIS.EXE`** a
ready session. This install ships only `AXIS.EXE`.

Run standalone (server-select gate bypassed via the registry key below), the client **hangs at the
"INITIALIZING NETSPHERE OPERATING SYSTEM" boot splash** and never reaches any menu. Measured with
Frida at that splash:

- it calls **only** `timeGetTime` (~300×/s, its frame timer) and a little `GetTickCount`;
- **zero** `ws2_32` calls — `WSAStartup`, `socket`, `connect`, `gethostbyname` are never called;
- **zero** registry reads, `CreateFile`, `FindWindow`, `CreateThread`.

So standalone `AXIS.EXE` does not even initialise networking; it spins waiting on an internal
"init complete" signal that only the (absent) launcher + (dead) NetSphere backend would provide.
The splash text is a texture, not a string, so the init state machine is not locatable by string
search. Consequently the per-tick simulation and animation that only run in-match (SPEC §2.10, §5)
cannot be observed offline, and neither can the authoritative input→acceleration transfer.

**Architecture correction — `AXIS.EXE` does no networking itself; `AxisPlay.exe` is the net
front-end and the two share memory (measured from startup, 2026-10-16).** Spawning `AXIS.EXE` under
Frida and tracing from entry shows it creates **~142 of its own events** (`CreateEventW` ×100,
`CreateEventA` ×42), several **anonymous** file mappings (`CreateFileMappingW` ×5, all unnamed) and
many `MapViewOfFile`/`DuplicateHandle` — but makes **zero** Winsock calls across the whole startup
(`WSAStartup`/`socket`/`connect`/`gethostbyname` = 0). So the 3D client never touches the network at
all: **`AxisPlay.exe` owns the sockets and relays to `AXIS.EXE` through inherited shared memory +
events** (classic launcher/renderer split). Therefore the socket/connect chain found statically in
`AXIS.EXE` below (`FUN_00452350`/`FUN_00453dd0`/`FUN_00452f20`) is **not the runtime path** standalone
— it is unused dead/fallback code; the live transport is the shared-memory IPC with the launcher. The
events `AXIS.EXE` blocks on at the splash are its **own** events, signalled by a worker that consumes
the launcher's shared-memory handoff, which never arrives. The real emulation target is thus
`AxisPlay.exe`'s **IPC side** (the shared-memory layout + event protocol), which is reversible from
`AXIS.EXE`'s reader side — not the in-client socket code.

**Closing this is a server-emulator-scale project**: reimplement the launcher handoff + a
protocol-correct lobby/game server, not merely a game-packet relay.

### No internal init loop to patch — the client is event-blocked (thread dump)

Enumerating the client's threads at the splash (Frida `Process.enumerateThreads`) shows **all 18
threads parked in OS wait syscalls** — `ntdll` `NtWaitForSingleObject`/`NtWaitForMultipleObjects`/
`NtDelayExecution` and `win32u` message-waits — with **no thread executing any `AXIS.EXE` code**,
and PCs unchanged across samples. So the boot stall is **not** a spinning state machine with a flag
to flip: the client is fully idle, blocked on events/messages that never arrive. Advancing it means
*posting the exact handoff signal the launcher would* (which event handle / window message), i.e.
reversing and reimplementing `AxisPlay.exe`'s signaling — not a one-byte patch. (A string search for
`netsphere`/`initializing`/`axisplay` also returns nothing: the splash is a texture, so there is no
init-state string to anchor on either.) A direct intervention was also tried and measured
insufficient: force-pulsing `SetEvent` on all nine Events the client waits on (40× over 4 s) left it
**still on the splash, unchanged** (and did not crash) — so the init is not gated on a bare event that
can be nudged; it needs the real launcher-provided handoff data/connection, confirming the dependency
is content, not just signalling. A second intervention — force-writing the connection/transport
state globals (`DAT_009fad08`=connected, `DAT_00a0c854`/`DAT_00a0ec74`=transport active, own actor id)
to 1 for ~5 s — also left the client on the splash, unchanged: the boot gate is **upstream** of the
connection state, a specific launcher "NetSphere ready" handoff not reachable by flag-poking. Finding
and satisfying it needs dynamic coverage analysis of the splash state machine (texture-based, so not
string-anchored) plus the skirmish-host feed — i.e. the AxisPlay emulator.

The handoff is also **not** a named-IPC signal that could be spoofed from outside: hooking the wait
APIs and resolving every waited-on handle with `NtQueryObject` (type + name; note the 32-bit
`UNICODE_STRING.Buffer` is at offset +4) shows the client blocks on **13 distinct handles — 9 Events,
2 Mutexes, 2 Semaphores — all anonymous except one named mutex, a `…\DirectSound Administrator shared
thread array` lock (the audio subsystem, unrelated to the launcher)**. It also creates/opens **no**
named event, mutex or file-mapping (`CreateEventW`/`OpenEventW`/`CreateMutexW`/`OpenMutexW`/
`CreateFileMappingW`/`OpenFileMappingW` all unseen). So the waits are on internal anonymous sync
objects (set by work the handoff would drive), with no launcher-named object to signal. The launcher
passes the client
**anonymous/inherited handles at `CreateProcess` time** (the usual socket-/event-handle inheritance
pattern), which a standalone launch never receives. Emulating it therefore requires actually
reimplementing `AxisPlay.exe`'s process-launch + handle-passing (and then the backend it hands off),
not signalling a named object — confirming the server-emulator scale.

## What is already mapped (the in-game wire protocol — fully reversed)

Once a client is in a match the wire format is **completely** reverse-engineered and reimplemented
offline (see `docs/SPEC.md` §4, `web/crypt.js`, `server/relay.js`, `tools/parse_ape.py`):

- **Frame**: 64-byte (`0x40`) payload, 9-byte header, 73-byte frame; per-channel sequence ring.
  Builder `pnSendPacketData` = `FUN_004a79c0` → socket send `FUN_00450700`.
- **Outbound**: all packet builders decoded — type 0 state snapshot, 1 input, 2 identity, 3–13
  control/chat, 12 join sync, 14 HP.
- **Inbound (in-game)**: dispatcher `FUN_0048bca0` = `switch(payload[1])`, counterpart to the
  builders; type-0 apply verified (position int16×10, yaw byte×14.0625, X–Z ground / Y up).
- **Determinism**: shared RNG pool verified (MSVC `srand(1)`), lockstep draws 247/247 on a replay.

So a server that is already *in a match* with a client speaks a known language — recorded matches
(`AutoPlay/*.ape`) are exactly this traffic and already replay in the engine.

## Is there an offline skirmish/host path? No standalone one in the client (checked)

`initdata\axis.cfg` (plaintext) is a full client config and includes a **host/skirmish block** —
`set_server name Kalimdor`, `set_server map 5`, `set_server numofplayer 16`, `set_server type DEATH
MATCH`, `set_server gameflag/timeflag` — alongside `set_player`, `set_graphic`, `set_sound` and the
in-match keybindings, and `initdata\` also has `skirmish.dat` + `profeng/profkor.dat` (profiles). So a
**local skirmish / host mode exists** at the product level, which would reach an in-match state
without NetSphere. But it does **not** give `AXIS.EXE` a standalone entry: the renderer does **no
networking** (so it cannot itself host a server), and a string search of `AXIS.EXE` for
`skirmish`/`deathmatch`/`set_server`/`host` finds **nothing** — the host/skirmish setup and those
config keys live in **`AxisPlay.exe`**. So even offline skirmish is launcher-hosted (AxisPlay runs the
local server + bots and feeds `AXIS.EXE` via the same memory injection); it is the same launcher
dependency, not a client-only offline match. Reaching in-match offline therefore still requires the
AxisPlay reimplementation — but note it need **not** involve a live backend for skirmish, which makes
a local-skirmish-hosting AxisPlay emulator the most plausible future route to exercising the in-match
simulation.

## The transport is in-process buffers, not a socket (recv reversed)

`FUN_004a7ab0` (the packet drain the net tick calls) makes **no Winsock calls** — consistent with the
zero-Winsock startup trace. It reads 64-byte (`0x40`) packets out of **in-process global buffers**,
round-robin over 16 channels (`DAT_00b12bb0 & 0xf`): the per-channel sequence ring at
`DAT_00af0b70 + (channel*0x80 + idx)*0x40` (the same ring `pnSendPacketData` writes) and the per-actor
roster slots at `DAT_009fad20 + actor*0x410` (the same slots the lobby handler `FUN_00450520` fills),
gated by per-actor ready-flags `DAT_009fad18[actor*0x104]` and sequence indices `DAT_00b12b70`. So the
client's "send"/"recv" move 64-byte frames in and out of these globals; something external deposits
inbound frames there (a shared section, cross-process injection, or a net worker — not `ws2_32`,
which the client never calls). **For an emulator this is the concrete injection point**: writing a
64-byte frame into the inbound ring / roster slot and setting the actor's ready-flag is exactly what
delivers a packet to the client; `server/relay.js` already produces these frames. Open question for
the launcher side: how frames physically cross from the network front-end into these globals.

**Raw-AFD ruled out — the client does no network I/O at all, so frames are injected cross-process
(measured 2026-10-16).** To rule out the client doing sockets below `ws2_32` (raw `\Device\Afd` via
`NtDeviceIoControlFile`, which `ws2_32` hooks miss), a spawn-time `ntdll` trace hooked `NtCreateFile`
(checking the object name for `afd`/`socket`) and `NtDeviceIoControlFile`: **zero AFD/socket handles
are ever opened** (`afd_hits=0`) — the `NtDeviceIoControlFile` calls are GPU/display IOCTLs. The client
only opens data files at init (e.g. `initdata\axis.cfg`, `initdata\msgkor.dat`). So AXIS.EXE performs
**no network I/O by any path**; `AxisPlay.exe` must deposit inbound 64-byte frames **directly into the
client's memory** (a shared PE section it maps, or `WriteProcessMemory` into the `DAT_00af0b70` ring /
`DAT_009fad20` slots, then set `DAT_009fad18` and signal). That injection point is externally reachable
(e.g. an emulator or even Frida `WriteProcessMemory`), but it only matters once the client is driven
past the boot handshake — which is the same launcher-handoff dependency.

## The lobby / connection entry points (mapped; the missing front half)

What is **not** reachable offline is everything before in-match. The entry points:

- **Connected gate**: `pnSendPacketData` only sends when `FUN_004a7d30` returns the global
  `DAT_009fad08` (the "connected" flag).
- **Transport open / connect**: `FUN_00452350(mode, addr)` resets the roster (`FUN_00450a30`),
  copies the server address from `DAT_00a0e7f0` (populated from the registry `TcpDNS`),
  then `switch(mode)` selects a backend: **mode 0 → `FUN_00453dd0(addr)`** = TCP client connect
  (init `FUN_00452bb0`, socket `FUN_00452ec0`, connect `FUN_00452f20(0,addr)`; sets
  `DAT_00a0ec74=1`); modes 1–3 are other transports. `connect` is an ordinal import (no name).
- **Lobby/session inbound dispatcher**: `FUN_00450520` = `switch(msgType-1)` on the lower
  transport layer (distinct from the in-game `FUN_0048bca0`):
  - type 1 → incoming **chat/text** (`FUN_004507e0` formats `"<name> > <text>"`),
  - type 2 → **session reset** (`FUN_00450a30` clears the per-actor roster `DAT_009fad10`,
    stride `0x410`; sets session id `DAT_009fee1c`),
  - **type 6 → join-ACK**: sets `DAT_009fad08 = 1`, marks the actor slot, and stores the client's
    own actor id in `DAT_009fee24` (the id `pnSendPacketData` stamps into every outbound frame),
  - type 7 → per-actor roster record copied to `DAT_009fad20 + actor*0x410`.
  So a server drives a client "connected" by sending session (2) then join-ACK (6).

## What a reimplementation would require (in order)

1. **Get the client past boot init** — satisfy whatever internal signal the "INITIALIZING
   NETSPHERE" state waits on (needs dynamic coverage analysis of the main-loop state machine; the
   connect chain above is never reached until this clears) **or** reimplement the `AxisPlay.exe`
   launcher that sets it up and hands off the session.
2. **Drive/implement the connection** — the client reads **DirectInput** (raw device state, not
   window messages), so any UI step needs hardware `SendInput`; or the launcher opens the socket
   and hands the client a ready transport.
3. **Serve the login + lobby handshake** — answer the client's first packets, then emit session
   (type 2) and join-ACK (type 6) so `DAT_009fad08` flips and the client enters a room.
4. **Serve the match** — from there the fully-mapped in-game protocol (above) applies, and the
   per-tick simulation / animation would finally run and could be captured to verify the two
   remaining reconstructions (SPEC §2.10 animation sampler, §5 input→acceleration).

## Reproduce the measurements

- Registry gate bypass (lets the client reach the boot splash; remove with
  `reg delete "HKCU\SOFTWARE\AxisArena" /f`):
  `SelectServer` → `TcpDNS=127.0.0.1`, `LadderDNS=127.0.0.1`, `TcpPort=7777`, `LadderPort=7778`.
- Launch `AXIS.EXE`, attach Frida, hook `FUN_004962d0`/`FUN_00487380`/`FUN_00497d70` (0 calls =
  per-tick not running) and `wglSwapBuffers` (~480/s = alive); hook `ws2_32`/`kernel32` to see the
  init makes no external calls. A window screenshot shows the splash.

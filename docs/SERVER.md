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

**Closing this is a server-emulator-scale project**: reimplement the launcher handoff + a
protocol-correct lobby/game server, not merely a game-packet relay.

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

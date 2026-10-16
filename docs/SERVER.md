# The online path: what a private server would take (reverse-engineering notes)

The offline port (see the top-level `README.md`) is complete for everything that runs
client-side without a live match. This document consolidates the reverse-engineering of the
**online path**. The headline finding (first section below) is that the client has a **built-in,
server-free, in-engine AutoPlay replay path whose logic runs fully offline**; the rest maps the
**live** online path — why an *interactive* match needs the launcher + a server, and the exact
entry points a private-server / launcher reimplementation would need. It is a map for a future
project, not a working server. All addresses are static (image base `0x400000`, no ASLR, so
static = runtime). Everything here was read from the client or measured at runtime with Frida;
nothing is guessed.

## BREAKTHROUGH: offline in-engine AutoPlay replay runs without a server (measured 2026-10-16)

The earlier conclusion below ("in-match is unreachable offline, needs a full launcher + server
emulator") is **partly overturned**. AXIS has a **built-in, server-free, in-engine AutoPlay /
attract-demo path** that loads a real recorded match from `AutoPlay/*.ape` straight into the full
3D engine. On this install its **game logic executes end-to-end offline** — the only thing between
it and visible pixels is the boot-splash *presentation* layer (still tied to the launcher boot
completion). This is exactly the mechanism the web port's replay feature reproduces — and the web
port actually **renders** it, which the gated native client does not do offline.

### The main-loop state machine (mapped + verified live)

`FUN_0041fc00` (the per-frame game tick) is the gate:

```c
if (DAT_0076e398 != 9999) { FUN_0041f460(); return; }   // boot/splash state handler
if (DAT_0067febc != 9999)   FUN_0041f390();             // (secondary transition)
if (DAT_0068134c == 0)    { FUN_004450c0(); FUN_00401030(); }  // one-time init
ECX = *(void**)0x00909764;                              // <-- game-object base pointer
FUN_00461440(ECX);                                      // mode dispatcher
```

- **`DAT_0076e398` @ `0x76e398`** is the master scene-state sentinel; **9999 = "ready"**. At the
  splash it sits at **200**. `DAT_0067febc` @ `0x67febc` is a secondary sentinel.
- **The game-object base pointer is the global at `DAT_00909764` @ `0x909764`** (runtime value e.g.
  `0x1186a020`; a ~37 MB heap object that embeds the whole world state inline — all the `base+0x…`
  offsets below are members of it).
- **Mode dispatcher `FUN_00461440`** = `switch(*(base+0x233cc20))`: **1 → `processAutoPlayMode`
  (`FUN_00467bf0`)**, 2 → `FUN_0048a680`, 3 → `FUN_004897f0`, **4 → `FUN_00487380` (playback)**.

### `processAutoPlayMode` = `FUN_00467bf0` — the offline loader/replayer

A sub-mode state machine at `*(base+0x233cc24)`; subtype `*(base+8)` selects the source (**3 =
from file**). It builds the path **`"AutoPlay/%s"`** (fmt `0x4c6248`) from the name buffer at
`base+0x1f1cbc + 0x40*index` (index = `*(base+0x1f35c0)`), `fopen(path,"rb")` → handle at
`base+0x2340d08`, reads the header (version into `base+0x1f35c4`, must be **`0x1201`**; map id
`DAT_009dbd44`; 32-byte name `DAT_00680af8`; …). Sub-modes:
`0 START → 1 LOAD_AUTO_PLAY → 2 RECEIVING_STAGE → 3 LOAD_GROUND → 4 LOAD_OBJECT →
5 SET_COLLISION_VOLUME → 6 ANALYZE_BATTLE_FIELD → (7 load actors) → 8 END_LOADING`, then returns
the next mode (subtype 3 → **4 = playback**). On open failure it prints
`"can't open file %s - processAutoPlayMode()"` (`0x4c621c`).

The file-list populator is **`FUN_00487130(name)`**: with `NULL` it scans `"AutoPlay\AutoPlay%03d.ape"`
(`0x4c8c60`/`0x4c8c70`) for indices 000–999 — on Windows's case-insensitive FS this matches the
shipped `Autoplay001.ape`; with an explicit name it opens `"AutoPlay\<name>.ape"`. Playback reuses
the **in-game recv path**: sub-mode 2 drains `FUN_004a7ab0` → `FUN_0048bca0` (the in-game inbound
dispatcher, SPEC §4), the `.ape` bytes being fed into the ring by `FUN_004612b0`.

### What ran, live (Frida, pid-of-the-day, 2026-10-16)

1. Forcing `DAT_0076e398 = 9999` on the gated client advanced it straight into
   `FUN_00461440 → processAutoPlayMode` — **first proof** was the `"can't open file AutoPlay/ -
   processAutoPlayMode()"` dialog (empty `%s`, because the name buffer wasn't populated).
2. The base pointer read from `0x909764` = `0x1186a020`. At the splash the object is **uninitialised**
   (`*(base+0x233cc20)` mode = 0, `*(base+8)` subtype = −1) because the normal boot states (300–900)
   are gated off.
3. Priming the base — mode `+0x233cc20 = 1`, subtype `+8 = 3`, sub-mode `+0x233cc24 = 0`,
   frame-counter `+0x233cc28 = 0`, index `+0x1f35c0 = 0`, name `"Autoplay001.ape"` at `+0x1f1cbc` —
   then holding `DAT_0076e398 = 9999` made `processAutoPlayMode` **run the full pipeline**:
   sub-mode `0 → 1` (**file opened**, handle `0x4d1968`, **version `0x1201`**, **map id 13**) `→ 2 →
   3 → 4 → 5 → 6 → 8`, after which the mode advanced to **4 (playback)** and stayed there, state
   `9999`, **stable for 30+ s**. So a real recorded match loaded into the real engine with **no
   server and no launcher**.
4. **The match actually plays** — hooking the in-game apply dispatcher `FUN_0048bca0` during mode-4
   playback shows it called **~45×/s** with real `.ape` frames (type bytes 0/1/2 — the same in-game
   protocol as SPEC §4), i.e. the recorded actors are being driven by the recorded data through the
   real in-game pipeline; `FUN_00487380` (playback) and `wglSwapBuffers` run every frame (~230 fps),
   both climbing steadily. So this is a **live in-match replay executing with no server** — not just a
   loader that reached a terminal state. (`FUN_004a7ab0`, the loader-phase drain, is **not** used in
   mode 4; playback sources frames by its own path but still routes them through `FUN_0048bca0`.)
5. The window nonetheless shows the boot/wait overlay: toggling `DAT_0067febc` changed nothing and
   `wglSwapBuffers` fires every frame, so the on-screen image is a **camera/overlay presentation
   state** drawn over the running scene, independent of the scene-state sentinels — the WinMain boot
   layer, which hands off only on the launcher's boot-completion. The replay **logic + actor drive +
   render + present all run**; only the visible camera/overlay for `AXIS.EXE` is unresolved, and it
   is not needed by the web port, which renders the same `.ape` data itself.

### The skeleton/animation render path also runs offline (mode-2 force, verified 2026-10-16)

The per-entity **model/animation/skeleton render is `FUN_00497d70`** (17 KB), called **only from the
mode-2 handler `FUN_0048a680`** (`FUN_00461440` case 2). In plain autoplay (mode 4 / `FUN_00487380`)
it is **not** called — measured: during mode-4 playback `FUN_00497d70` = **0 calls** while the small
per-tick `FUN_004962d0` runs ~230×/s. But after the scene finishes loading, **forcing
`*(base+0x233cc20) = 2`** (so the dispatcher runs `FUN_0048a680`) makes **`FUN_00497d70` execute every
frame** (measured ~230×/s, 1:1 with the mode-2 handler) on the autoplay-loaded actors. So the
skeleton/animation code path **runs offline and is directly observable in memory** — the
long-standing "animation keyframe sampler can't be observed offline (needs a live match)" caveat
(SPEC §2.10) is **no longer true on this install**: the sampler now executes and can be hooked. (The
window still shows the splash overlay — pixels are gated by the independent boot layer — but the
sampler does not need pixels to be captured.)

Call-frequency profiling of **all ~80 callees** of `FUN_00497d70` in the forced mode-2 render maps the
pipeline. The hot paths are: `0x467a60` (~184/frame) and `0x467a50` (~71/frame, viewport-width getter)
GL matrix-stack helpers; `0x466c60` (~53/frame) = **per-axis vector rotation** and `0x466c00`
(~37/frame) = **vec3 normalize** (both decompiled); `0x47c800` (~212/frame, matrix multiply),
`0x47ca20` (~42/frame, **4×4 matrix copy**), `0x49e274` (~2562/frame, float math leaf); `0x471e30`
(~1/entity, **camera + entity screen-projection** for HUD/targeting — not the skeletal sampler); and a
handful of ~1/entity callees including `0x4971b0`, which decompiles to a **procedural grid/ring mesh
builder** (an animated water/effect surface, using `0x466c60` to sweep a 4-point ring at 0/90/180/270°).
`0x470bc0` (~3/frame) is the already-reconstructed broadphase/xform.

**Verified rotation/angle convention (engine-wide):** `FUN_00466c60(axis, angle)` rotates a vec3 about
`axis` (0=X, 1=Y, 2=Z) by `angle` expressed in **integer tenths of a degree** (`angle mod 3600`,
3600 = 360°), via precomputed LUTs — **cosine table at `0x00a19408`, sine table at `0x00a1cc48`**, each
3600 floats. This is how the engine applies all rotations (procedural meshes and skeletal bones alike);
the same `(&DAT_00a19408)[idx mod 3600]` LUT read appears in `FUN_00471e30`.

### Static analysis of the render path's bone/orientation math (no runtime gate needed)

Statically reading `FUN_00497d70`'s `FUN_00466c60` call sites (xrefs at `0x498fd4`/`0x498fea`/`0x499001`
and the `0x49a7xx`/`0x49afxx`/`0x49b3xx+` clusters) resolves what those rotations are — and it is **model
orientation from actor state, not time-indexed keyframe interpolation**:

- A `switch` on a discrete direction/pose code (`0x110`/`1`/`0x10`/`0xf1`/`0x1000`…) pushes **fixed
  45°-step angles** (−3150/−900/−2700/−450/−1350/−2250/−1800 tenths-deg = −315°/−90°/−270°/−45°/−135°/
  −225°/−180°) into `FUN_00466c60(axis=2, angle)` — an **8-direction billboard facing** rotation about Z.
- Then it loads per-actor **pitch/yaw** floats at `[base+0x233ca28]` / `[base+0x233ca2c]`, rounds them with
  `FUN_0049e274` (= `__ftol`, VS library) to the integer tenths-of-degree LUT index, and rotates. These
  angles are the actor's networked orientation (type-0 state; yaw byte ×14.0625 per SPEC §4), **not**
  keyframes read from the skeleton file by a time cursor.

So in the reachable render path there is **no per-key stride/time-base keyframe sampler**: model posing is
**orientation-from-state** (billboard facing + pitch/yaw in tenths-of-degree via the sin/cos LUTs), which
is exactly what the web port's replay already does (it orients `.ape` actors by their state). The earlier
framing of a missing "animation keyframe sampler" is, for this path, resolved to orientation-from-state;
whether AXIS has a *separate* skeletal-keyframe subsystem for some models (the `.shp`/skeleton files are
decoded) is not exercised here and remains the only open animation question — a fidelity detail, not a
blocker for the offline port.

**Honest status of the character skeleton keyframe sampler — still gated (correcting the earlier
"now observable" claim):** the **character** per-bone keyframe sampler was **not** among the hot callees
in the forced-playback state. What runs is procedural-mesh + camera/projection + matrix/normalize
helpers; no per-bone character-animation hot path appears. That is consistent with no visible character
models being drawn (the window shows the splash overlay, not the arena), i.e. the character skeletal
draw is gated behind the **same scene-active / visible-model condition as the on-screen pixels**. So the
sampler is **not** independently observable after all — it sits behind the same boot/presentation gate.
Reconstructing its per-key stride/time-base therefore still depends on cracking that gate (or on static
analysis of the skeletal-draw branch within the 17 KB `FUN_00497d70`). It remains a **fidelity**
refinement only: the web port already animates replay actors from the `.ape` actor state, and the
rotation convention above (tenths-of-degree + the two LUTs) is the one concrete new primitive this
profiling did pin down.

### What this means for the boundary

- The in-match per-tick data (actor transforms + animation per frame) **is present offline** in the
  `.ape` stream and **is replayable** — the web port already decodes and renders it (`web/ape.js` +
  the engine replay mode). The AutoPlay path is the native client doing the *same thing*.
- The **animation keyframe sampler is now observable offline** (above) — a reconstruction target, no
  longer a structural blocker. It affects in-match skeletal-pose fidelity; the web port already
  animates replay actors from the `.ape` actor state, so this is a fidelity improvement, not a gap in
  the offline port.
- Reaching a **live, interactive** match still needs the server-side **input→acceleration** transfer
  (SPEC §5) — but **replay does not**: positions come from the file, not from client input. So the
  "no offline ground truth" caveat now applies **only** to *live play*, not to observing recorded
  motion or animation.
- Remaining native-only gap for on-screen pixels in `AXIS.EXE`: the boot-splash presentation overlay
  (a separate subsystem, drawn over the running scene even while `wglSwapBuffers` fires every frame).
  Not required for the web port, which renders the same `.ape` replays itself.

---

## Why a *live* match is unreachable offline (measured root cause)

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
- **Drive the offline AutoPlay logic** (the breakthrough above): at the splash, attach Frida, read
  `base = *(void**)0x909764`, then write `base+0x233cc20 = 1` (mode=AutoPlay), `base+8 = 3`
  (from-file), `base+0x233cc24 = 0`, `base+0x233cc28 = 0`, `base+0x1f35c0 = 0`, the ANSI string
  `"Autoplay001.ape"` at `base+0x1f1cbc`, and hold `DAT_0076e398 (0x76e398) = 9999` every frame.
  Hooking `FUN_00467bf0` entry logs the sub-mode advancing `0→1→…→8` with the opened file handle at
  `base+0x2340d08`, version `base+0x1f35c4 == 0x1201` and map id `DAT_009dbd44`; the mode
  `base+0x233cc20` then advances to `4` (playback) and holds. (The window keeps showing the splash —
  the presentation layer is independent of these sentinels.)
- **Observe the in-match pipeline + animation path**: with the above running, hook `FUN_0048bca0`
  (in-game apply) — it fires ~45×/s with type 0/1/2 frames (actors driven by the replay). Then, once
  `base+0x233cc20` reaches `4`, **overwrite it with `2`**: the dispatcher switches to `FUN_0048a680`,
  which calls the model/skeleton render `FUN_00497d70` every frame (hook both to confirm ~230×/s) —
  the animation sampler then runs and can be captured from its `0x472xxx–0x473xxx` callees.

# AXIS multiplayer relay (server skeleton)

A zero-dependency Node.js WebSocket server that reproduces the reverse-engineered AXIS
packet transport (see `../docs/SPEC.md §4`). It is the server the browser/WASM client
connects to in place of the original Winsock server.

```sh
node relay.js [port]      # default 8140, no npm install needed
```

## What it does (verified)

- WebSocket handshake + framing using only Node's `http`/`crypto` (no `ws` dependency).
- Assigns each client an **actor slot** (0–15, matching AXIS's 0x10 actor loop) and tells
  it its slot with a `[0xFF, actorId]` control message.
- Accepts the game's **fixed 64-byte player-action payload** and **relays** it to the other
  clients, framed as `[u32 tick][u8 actorId][64-byte payload]` so clients can order and
  attribute updates. (AXIS uses a per-channel sequenced relay model.)

A two-client self-test confirms a 64-byte packet sent by one client arrives at the other as
a 69-byte relayed frame with the correct tick, source actor, and payload.

## What it is not (yet)

This is the **transport + slot + relay** foundation, not full game logic. Completing the
server needs every packet type's semantics and server-side validation — which are
recoverable from **decrypted `.ape` replays** (they record the real 64-byte packets, see
`../docs/SPEC.md §4`) plus reversing the recv/dispatch path. Those build directly on top of
this relay.

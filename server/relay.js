#!/usr/bin/env node
/*
 * AXIS multiplayer relay — a zero-dependency WebSocket server that reproduces the
 * reverse-engineered packet model (see ../docs/SPEC.md §4):
 *
 *   - clients exchange fixed 64-byte player-action payloads (the game's packet body),
 *   - the server assigns each client an actor slot and relays every packet it receives
 *     to the other clients (the game uses a per-channel sequenced relay model),
 *   - a monotonically increasing tick is stamped on each relayed frame.
 *
 * This is the server skeleton the WASM client (approach B, ../port) connects to in place
 * of the original Winsock server. Full gameplay needs every packet type's semantics
 * (recoverable from decrypted .ape replays, §4) and server-side validation; this gives the
 * transport + slot assignment + relay that all of that builds on.
 *
 * No npm install — the WebSocket handshake and framing use only Node's http/crypto.
 *     node relay.js [port]        (default 8140)
 */
'use strict';
const http = require('http');
const crypto = require('crypto');

const PORT = parseInt(process.argv[2] || '8140', 10);
const PAYLOAD = 0x40;                 // 64-byte player-action payload (SPEC §4)
const MAX_PLAYERS = 16;               // AXIS iterates 0x10 actor slots
const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

const clients = new Map();            // actorId -> { socket, alive }
let tick = 0;

function accept(key) {
  return crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
}

function wsFrame(payload) {           // server->client binary frame (unmasked)
  const len = payload.length;
  let head;
  if (len < 126) head = Buffer.from([0x82, len]);
  else if (len < 65536) { head = Buffer.alloc(4); head[0] = 0x82; head[1] = 126; head.writeUInt16BE(len, 2); }
  else { head = Buffer.alloc(10); head[0] = 0x82; head[1] = 127; head.writeUInt32BE(0, 2); head.writeUInt32BE(len, 6); }
  return Buffer.concat([head, payload]);
}

function send(sock, payload) { try { sock.write(wsFrame(payload)); } catch (e) {} }

function firstFreeSlot() {
  for (let i = 0; i < MAX_PLAYERS; i++) if (!clients.has(i)) return i;
  return -1;
}

// relay one 64-byte player payload from `fromId` to everyone else, framed as
// [u32 tick][u8 actor][payload...] so the client can order and attribute it.
function relay(fromId, payload) {
  const frame = Buffer.alloc(5 + payload.length);
  frame.writeUInt32LE(tick++, 0);
  frame.writeUInt8(fromId, 4);
  payload.copy(frame, 5);
  for (const [id, c] of clients) if (id !== fromId) send(c.socket, frame);
}

function parseFrames(buf, onMsg) {     // minimal client->server frame parser
  let off = 0;
  while (off + 2 <= buf.length) {
    const b1 = buf[off], b2 = buf[off + 1];
    const opcode = b1 & 0x0f, masked = b2 & 0x80;
    let len = b2 & 0x7f, p = off + 2;
    if (len === 126) { if (p + 2 > buf.length) break; len = buf.readUInt16BE(p); p += 2; }
    else if (len === 127) { if (p + 8 > buf.length) break; len = Number(buf.readBigUInt64BE(p)); p += 8; }
    let mask = null;
    if (masked) { if (p + 4 > buf.length) break; mask = buf.slice(p, p + 4); p += 4; }
    if (p + len > buf.length) break;
    const data = buf.slice(p, p + len);
    if (mask) for (let i = 0; i < data.length; i++) data[i] ^= mask[i & 3];
    if (opcode === 0x8) return { closed: true, rest: buf.slice(p + len) };
    if (opcode === 0x1 || opcode === 0x2) onMsg(data);
    off = p + len;
  }
  return { rest: buf.slice(off) };
}

const server = http.createServer((req, res) => { res.writeHead(426); res.end('WebSocket only'); });

server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'];
  if (!key) { socket.destroy(); return; }
  socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
    'Sec-WebSocket-Accept: ' + accept(key) + '\r\n\r\n');

  const id = firstFreeSlot();
  if (id < 0) { socket.destroy(); return; }
  clients.set(id, { socket });
  console.log(`+ player ${id} joined (${clients.size}/${MAX_PLAYERS})`);
  // tell the client its assigned actor slot: [0xFF][actorId]
  send(socket, Buffer.from([0xff, id]));

  let acc = Buffer.alloc(0);
  socket.on('data', (chunk) => {
    acc = Buffer.concat([acc, chunk]);
    const r = parseFrames(acc, (data) => {
      // accept the fixed 64-byte payload (ignore anything malformed)
      if (data.length === PAYLOAD) relay(id, data);
    });
    acc = r.rest;
    if (r.closed) socket.end();
  });
  const bye = () => { if (clients.delete(id)) console.log(`- player ${id} left (${clients.size}/${MAX_PLAYERS})`); };
  socket.on('close', bye); socket.on('error', bye);
});

server.listen(PORT, () => {
  console.log(`AXIS relay on ws://127.0.0.1:${PORT}  (64-byte packets, ${MAX_PLAYERS} slots)`);
});

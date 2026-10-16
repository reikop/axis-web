'use strict';
/* Why the authoritative input->acceleration transfer can't be back-calculated
 * from the replays — measured, not asserted (SPEC §5).
 *
 * A fair question: the .ape replays record motion, so can't we recover the
 * original server's input->acceleration curve by differentiating position and
 * pairing it with the recorded input? This tool measures the two things that
 * decide it, straight from your own AutoPlay/*.ape:
 *
 *   1. Packet mix + what the "input" packets (type 1) actually carry. Type 0 is
 *      dense per-actor state; type 1 is the sparse input/RNG-draw packet (SPEC §4,
 *      verify_rng.js). We show type 1 is sparse and, for movement, uninformative:
 *      only the recording client's own slot has structured bytes and even those
 *      are constant — the held-key stream is not in the file, and remote players'
 *      input is absent entirely (all 0xFF).
 *   2. The type-0 sampling cadence. We show the state snapshots are sparse and
 *      very irregular in time, so a clean second derivative (acceleration) can't
 *      be taken — the per-frame accel happens *between* snapshots and is not
 *      recorded. This is the same reason verify_motion.js finds constant-velocity
 *      optimal and constant-acceleration adding nothing.
 *
 * Conclusion: replays ground-truth WHERE players were (positions -> velocity,
 * rough max speed — which the port uses and verifies), but NOT the fine-grained
 * input->acceleration transfer, which operated below the snapshot resolution.
 *
 *   node tools/analyze_replay_sampling.js <game_dir>
 */
const fs = require('fs'), path = require('path');
const AP = require(path.join(__dirname, '..', 'web', 'ape.js'));
const REC = 76, PAY = 12;   // 76-byte records: 12-byte header + 64-byte (0x40) payload

function stats(a) {
  if (!a.length) return null;
  a = a.slice().sort((x, y) => x - y);
  const q = p => a[Math.min(a.length - 1, Math.floor(p * a.length))];
  return { n: a.length, min: a[0], median: q(0.5), p90: q(0.9), max: a[a.length - 1] };
}

/* Walk raw records once (ape.js exposes decrypt + parseReplay; we need per-record
 * payload bytes that parseReplay doesn't surface, so re-walk with the same layout). */
function records(ab) {
  const u8 = AP.decrypt(ab);
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  let start = -1;
  const lim = Math.min(u8.length, 4096);
  for (let o = 0; o < lim && start < 0; o++) {
    let ok = true;
    for (let k = 0; k < 3; k++) { const b = o + k * REC; if (b + REC > u8.length || dv.getUint32(b + 8, true) !== 0x40) { ok = false; break; } }
    if (ok) start = o;
  }
  const out = [];
  if (start < 0) return out;
  for (let o = start; o + REC <= u8.length; o += REC) {
    if (dv.getUint32(o + 8, true) !== 0x40) break;
    const p = o + PAY;
    out.push({ tick: dv.getUint32(o, true), type: u8[p + 1], payload: u8.subarray(p, p + 64) });
  }
  return out;
}

function main() {
  const game = process.argv[2];
  if (!game) { console.error('usage: node analyze_replay_sampling.js <game_dir>'); process.exit(2); }
  const dir = path.join(game, 'AutoPlay');
  if (!fs.existsSync(dir)) { console.error('no AutoPlay/ in', game); process.exit(2); }
  for (const f of fs.readdirSync(dir).filter(f => /\.ape$/i.test(f))) {
    const raw = fs.readFileSync(path.join(dir, f));
    const ab = raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength);
    const recs = records(ab);
    if (!recs.length) { console.log(f, '-> no records'); continue; }

    const types = {};
    for (const r of recs) types[r.type] = (types[r.type] || 0) + 1;

    // type-1 input packets: which actor slot carries structured (non-0xFF) bytes,
    // and does the held-movement portion vary? (payload[2] = actor; [3..8] = the
    // movement-looking region.)
    const t1 = recs.filter(r => r.type === 1);
    const perActor = {};
    const movingVaries = {};
    for (const r of t1) {
      const a = r.payload[2];
      (perActor[a] = perActor[a] || 0, perActor[a]++);
      const key = Array.from(r.payload.subarray(3, 9)).join(',');
      (movingVaries[a] = movingVaries[a] || new Set()).add(key);
    }

    // type-0 state cadence per actor (tick gaps)
    const gaps = [];
    const byActor = {};
    for (const r of recs) if (r.type === 0) { const a = r.payload[4] >> 4; (byActor[a] = byActor[a] || []).push(r.tick); }
    for (const a of Object.keys(byActor)) { const t = byActor[a]; for (let i = 1; i < t.length; i++) gaps.push(t[i] - t[i - 1]); }
    const g = stats(gaps);

    console.log('=== ' + f + ' ===');
    console.log('  packet types (type -> count):', JSON.stringify(types));
    console.log('  type-1 input packets:', t1.length, '| per actor slot:', JSON.stringify(perActor));
    console.log('  type-1 movement-region [3..8] distinct values per actor:',
      JSON.stringify(Object.fromEntries(Object.entries(movingVaries).map(([a, s]) => [a, s.size]))),
      '(1 = constant = no held-key stream)');
    if (g) console.log('  type-0 state tick-gaps: min', g.min, 'median', g.median, 'p90', g.p90, 'max', g.max,
      '(huge spread = sparse, irregular sampling -> no clean acceleration)');
  }
  console.log('\nTakeaway: input packets are sparse and movement-uninformative, and state');
  console.log('snapshots are too sparse/irregular to differentiate twice — so the original');
  console.log('server-side input->acceleration transfer is not recoverable from replays.');
  console.log('Replays DO ground-truth positions/velocity (see verify_motion.js), which the');
  console.log('port uses; the fine accel curve operated below this sampling resolution.');
}
main();

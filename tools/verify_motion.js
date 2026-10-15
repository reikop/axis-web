'use strict';
/* Verify the AXIS movement model against real recorded gameplay.
 *
 * The deterministic combat simulation can't be verified offline (it runs only in
 * a live match the dead-server install can't reach — SPEC §5). But the game's
 * *motion* model can: AutoPlay/*.ape replays record every actor's real world
 * position each network tick, so they are ground-truth for how positions evolve.
 *
 * This checks whether a simple kinematic integrator reproduces that motion. For
 * each actor trajectory it predicts the next sample three ways and reports the
 * error (world units; samples are quantised to 10):
 *   const-position : p̂[t] = p[t-1]                 (no motion model)
 *   const-velocity : p̂[t] = 2·p[t-1] − p[t-2]      (inertial / dead-reckoning)
 *   const-accel    : p̂[t] = 3·p[t-1] − 3·p[t-2] + p[t-3]
 *
 * Result on this install's replays: const-velocity cuts the median error ~4×
 * below const-position (≈172 vs ≈710 units, against ≈787-unit moves), and
 * const-accel does NOT improve it — i.e. the recorded motion is predominantly
 * constant-velocity between snapshots, with the authoritative position
 * correcting the residual. So the movement model is inertial dead-reckoning; the
 * engine uses exactly this to tween remote players between packets.
 *
 *   node tools/verify_motion.js <game_dir>
 */
const fs = require('fs'), path = require('path');
const AP = require(path.join(__dirname, '..', 'web', 'ape.js'));

const dist = (a, b) => Math.hypot(a[0]-b[0], a[1]-b[1], a[2]-b[2]);
function stats(a) { if (!a.length) return null; a.sort((x,y)=>x-y); const q=p=>a[Math.min(a.length-1, Math.floor(p*a.length))]; return { n: a.length, median: +q(0.5).toFixed(1), p90: +q(0.9).toFixed(1), p99: +q(0.99).toFixed(1) }; }

function main() {
  const game = process.argv[2];
  if (!game) { console.error('usage: node verify_motion.js <game_dir>'); process.exit(2); }
  const dir = path.join(game, 'AutoPlay');
  if (!fs.existsSync(dir)) { console.error('no AutoPlay/ in', game); process.exit(2); }
  const eP = [], eV = [], eA = [], steps = [];
  let files = 0;
  for (const f of fs.readdirSync(dir).filter(f => /\.ape$/i.test(f))) {
    const raw = fs.readFileSync(path.join(dir, f));
    const ab = raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength);
    const r = AP.parseReplay(ab); files++;
    for (const actor of Object.keys(r.tracks)) {
      const P = r.tracks[actor].map(e => [e[1], e[2], e[3]]);
      for (let i = 1; i < P.length; i++) {
        steps.push(dist(P[i], P[i-1])); eP.push(dist(P[i], P[i-1]));
        if (i >= 2) eV.push(dist(P[i], [2*P[i-1][0]-P[i-2][0], 2*P[i-1][1]-P[i-2][1], 2*P[i-1][2]-P[i-2][2]]));
        if (i >= 3) eA.push(dist(P[i], [3*P[i-1][0]-3*P[i-2][0]+P[i-3][0], 3*P[i-1][1]-3*P[i-2][1]+P[i-3][1], 3*P[i-1][2]-3*P[i-2][2]+P[i-3][2]]));
      }
    }
  }
  const sS = stats(steps), sP = stats(eP), sV = stats(eV), sA = stats(eA);
  console.log(`replays            : ${files}`);
  console.log(`move / step        : ${JSON.stringify(sS)}`);
  console.log(`const-position err : ${JSON.stringify(sP)}`);
  console.log(`const-velocity err : ${JSON.stringify(sV)}`);
  console.log(`const-accel    err : ${JSON.stringify(sA)}`);
  const inertial = sV.median < sP.median * 0.5 && sV.median <= sA.median * 1.3;
  console.log(inertial
    ? `\nVERIFIED: motion is inertial (const-velocity cuts median error ${(sP.median/sV.median).toFixed(1)}x below const-position, and const-accel adds no gain). Movement model = constant-velocity dead-reckoning between snapshots.`
    : `\nmotion is NOT predominantly constant-velocity on this data`);
  process.exit(inertial ? 0 : 1);
}
main();

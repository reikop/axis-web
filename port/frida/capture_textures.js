/* Dynamic texture-binding capture for AXIS (Frida).
 *
 * The static .shp group names are fragmentary (SPEC 2.9); the real per-group
 * texture name is built at run time. This hooks the running game to record it.
 *
 * PREREQUISITE — get the client past its server-select gate (it reads
 * HKCU\SOFTWARE\AxisArena\SelectServer, normally written by AxisPlay.exe):
 *   reg add "HKCU\SOFTWARE\AxisArena\SelectServer" /v TcpDNS    /t REG_SZ    /d 127.0.0.1 /f
 *   reg add "HKCU\SOFTWARE\AxisArena\SelectServer" /v LadderDNS /t REG_SZ    /d 127.0.0.1 /f
 *   reg add "HKCU\SOFTWARE\AxisArena\SelectServer" /v TcpPort   /t REG_DWORD /d 7777      /f
 *   reg add "HKCU\SOFTWARE\AxisArena\SelectServer" /v LadderPort/t REG_DWORD /d 7778      /f
 * (remove later: reg delete "HKCU\SOFTWARE\AxisArena" /f)
 *
 * Then SPAWN the game with this script (so the hooks are live before the one-time
 * load that resolves textures):
 *   frida -f AXIS.EXE -l capture_textures.js      # (run in the install dir)
 *
 * AXIS.EXE loads at its preferred base 0x400000 (no ASLR), so file addresses are
 * runtime addresses. Hooks:
 *   FUN_004658d0 (0x4658d0) — per-shape group texturing (ecx = render object)
 *   FUN_00456070 (0x456070) — texture-lookup-by-name (param_1 = name C string)
 * Each FUN_00456070 call made while inside FUN_004658d0 is attributed to that shape.
 *
 * Observed (title/menu scene): each group binds ONE "Texture\\NN" slot
 * (00/01/02/07…) or a shape-relative path (e.g. Shape\\item\\warpzone\\warpzone).
 */
'use strict';
const base = Process.enumerateModules()[0].base;
const A = a => base.add(a - 0x400000);
const bind = {};
let curShape = null, depth = 0;

function readPath(p) {
  try { const s = p.readCString(); return (s && s.length > 0 && s.length < 128 && /^[\x20-\x7e]+$/.test(s)) ? s : null; } catch (e) { return null; }
}

Interceptor.attach(A(0x4658d0), {
  onEnter() {
    depth++;
    try {
      const ecx = this.context.ecx; let nm = null;
      for (const off of [0, 4, 8, 0xc, 0x10, 0x14, 0x18, 0x1c, 0x20]) {
        const c = readPath(ecx.add(off).readPointer());
        if (c && (c.indexOf('\\') >= 0 || c.indexOf('/') >= 0)) { nm = c; break; }
      }
      curShape = nm || ('@' + ecx.toString(16));
    } catch (e) { curShape = '(err)'; }
  },
  onLeave() { depth--; if (depth <= 0) { curShape = null; depth = 0; } }
});

Interceptor.attach(A(0x456070), {
  onEnter() {
    try {
      const nm = this.context.esp.add(4).readPointer().readCString();
      if (nm && nm.length < 128) {
        const s = (depth > 0 && curShape) ? curShape : '(no-shape)';
        (bind[s] || (bind[s] = {}))[nm] = (bind[s][nm] || 0) + 1;
      }
    } catch (e) {}
  }
});

console.log('[capture_textures] hooked at base ' + base);
setInterval(() => {
  send({ shapes: Object.keys(bind).length, bind });
}, 5000);

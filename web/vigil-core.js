/* vigil-core.js: the shared C engine (web/vigil.wasm, inlined by `make wasm` as web/vigil-wasm.js) for the page.
 * VC is null when the module is missing (not built yet): the page then runs as before, without "Today". */
(() => {
  const root = typeof window !== 'undefined' ? window : globalThis;
  const b64 = root.VIGIL_WASM;
  if (!b64) { root.VC = null; return; }
  const bytes = typeof atob === 'function' ? Uint8Array.from(atob(b64), c => c.charCodeAt(0)) : Buffer.from(b64, 'base64');
  const nosys = () => 52; // ENOSYS: the C never prints or opens files here
  const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), { wasi_snapshot_preview1: { fd_close: nosys, fd_seek: nosys, fd_write: nosys } });
  const X = inst.exports;
  X._initialize();
  const mem = () => new Uint8Array(X.memory.buffer);
  const str = p => { const m = mem(); let e = p; while (m[e]) e++; return new TextDecoder().decode(m.subarray(p, e)); };
  X.v_init(0);
  const table = what => Array.from({ length: X.v_n(what) }, (_, k) => ({ id: k, name: str(X.v_name(what, k)), about: str(X.v_about(what, k)), home: what === 0 ? X.v_home(k) : -1 }));
  const ROUTINES = table(0), WHIMS = table(1), STATES = table(2).map(s => s.name);
  const SHELL_FRAC = X.v_shell_frac();
  const byName = (list, name) => { const n = String(name).toLowerCase(); const r = list.find(x => x.name.toLowerCase() === n); return r ? r.id : -1; };
  root.VC = {
    ROUTINES, WHIMS, STATES, SHELL_FRAC,
    rt: name => byName(ROUTINES, name), whim: name => byName(WHIMS, name), st: name => STATES.indexOf(name),
    create: now => X.v_new(now),
    state: (i, st) => X.v_state(i, st), shown: (i, now) => X.v_shown(i, now),
    play: (i, rt) => X.v_play(i, rt), playWhim: (i, w, now) => X.v_whim(i, w, now),
    running: i => X.v_rt(i), whimNow: i => X.v_whim_now(i),
    pointer: (i, hover, lx, ly, lock) => X.v_pointer(i, hover ? 1 : 0, lx, ly, lock ? 1 : 0),
    carry: (i, on, mx, my) => X.v_carry(i, on ? 1 : 0, mx, my),
    held: (i, on, sx, sy) => X.v_held(i, on ? 1 : 0, sx, sy),
    press: (i, lx, ly, now) => X.v_press(i, lx, ly, now), ring: (i, now) => X.v_ring(i, now), defend: (i, dx, dy, now) => X.v_defend(i, dx, dy, now),
    turn: (i, dax, day) => X.v_turn(i, dax, day),
    // the black hole (web/blackhole-mock.html): its look's tunables (BhTune, by number), actions, axis, room round it
    tune: (k, v) => X.v_tune(k, v), tuneGet: k => X.v_tune_get(k), b2progress: () => X.v_b2_progress(),
    holeActs: mask => X.v_hole_acts(mask >>> 0), holePlay: (i, a) => X.v_hole_play(i, a), holeTilt: speed => X.v_hole_tilt(speed),
    pad: (i, p) => X.v_pad(i, p), holeView: (i, nx, ny, nz, now) => X.v_hole_view(i, nx, ny, nz, now),
    now: () => performance.now() / 1000,
    tick: (now, dt) => X.v_tick(now, dt),
    // slot i drawn into a px-square ImageData (straight alpha: the light alone, see-through where dark)
    render(i, px, now, glow = 1, core = .35) {
      const p = X.v_render(i, px, glow, core, now);
      if (!p) return null;
      return new ImageData(new Uint8ClampedArray(X.memory.buffer, p, px * px * 4).slice(), px, px);
    },
    pixels(i, px, now, glow = 1, core = .35) { const p = X.v_render(i, px, glow, core, now); return p ? new Uint8Array(X.memory.buffer, p, px * px * 4) : null; },
  };
})();

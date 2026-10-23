// SPDX-License-Identifier: AGPL-3.0-only
// Smoke test of the browser shim: loaded as an offline "game" (tests/run_page.sh), it checks the web
// APIs PokeRogue relies on and logs one line per check:
//   [test] PASS <name>   /   [test] FAIL <name>: <reason>
// and finally "[test] DONE passed=N failed=M", which the runner waits for.
// Needs no network: everything is served from this folder under the offline origin.

const results = [];
async function check(name, fn) {
  try {
    const r = await fn();
    if (r !== undefined && r !== true) throw new Error(typeof r === "string" ? r : JSON.stringify(r));
    results.push(true);
    console.log("[test] PASS " + name);
  } catch (e) {
    results.push(false);
    console.log("[test] FAIL " + name + ": " + (e && e.message ? e.message : e));
  }
}
function eq(a, b, what) {
  const x = JSON.stringify(a), y = JSON.stringify(b);
  if (x !== y) throw new Error((what ? what + ": " : "") + "got " + x + ", expected " + y);
}
const nextFrame = () => new Promise((r) => requestAnimationFrame(r));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function within(ms, p, what) {
  let timer;
  const timeout = new Promise((_, rej) => { timer = setTimeout(() => rej(new Error(what + " timed out")), ms); });
  try { return await Promise.race([p, timeout]); } finally { clearTimeout(timer); }
}

// 3x2 RGBA PNG: red, green, half-transparent blue columns.
const PNG_3x2 = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAMAAAACCAYAAACddGYaAAAAFElEQVR4nGP4z8DwHwwZ/jcwIHMAjhMK97bT1FAAAAAASUVORK5CYII=";

// 0.1 s of a 16-bit mono 22050 Hz sine as a WAV file.
function makeWav() {
  const rate = 22050, n = 2205, buf = new ArrayBuffer(44 + n * 2), v = new DataView(buf);
  const str = (o, s) => { for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); };
  str(0, "RIFF"); v.setUint32(4, 36 + n * 2, true); str(8, "WAVE");
  str(12, "fmt "); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
  v.setUint32(24, rate, true); v.setUint32(28, rate * 2, true); v.setUint16(32, 2, true); v.setUint16(34, 16, true);
  str(36, "data"); v.setUint32(40, n * 2, true);
  for (let i = 0; i < n; i++) v.setInt16(44 + i * 2, Math.round(Math.sin(i / 10) * 8000), true);
  return buf;
}

async function run() {
  // ------------------------------------------------------------ page and DOM
  await check("page: inline classic script ran", () => window.inlineScriptRan === true || "inline script did not run");
  await check("page: location is the offline origin", () => eq(location.origin, "https://offline.invalid"));
  await check("dom: parsed body, selectors, attributes", () => {
    const app = document.getElementById("app");
    if (!app) return "no #app";
    eq(app.querySelectorAll("span").length, 2, "spans");
    eq(document.querySelector("#app span.x").textContent, "world");
    eq(app.dataset.kind, "smoke");
    eq(app.classList.contains("b"), true, "classList");
    eq(document.title, "runtime smoke test", "title");
  });
  await check("dom: createElement, innerHTML, events", () => {
    const div = document.createElement("div");
    div.innerHTML = "<p id='p1'>one</p><p>two</p>";
    document.body.appendChild(div);
    eq(document.getElementById("p1").textContent, "one");
    let got = 0;
    div.addEventListener("custom", (e) => { got = e.detail; });
    div.dispatchEvent(new CustomEvent("custom", { detail: 7 }));
    eq(got, 7, "event detail");
    div.remove();
    eq(document.getElementById("p1"), null, "removed");
  });

  // ------------------------------------------------------------ core
  await check("core: timers and microtasks run in order", async () => {
    const order = [];
    setTimeout(() => order.push("timeout"), 0);
    queueMicrotask(() => order.push("microtask"));
    Promise.resolve().then(() => order.push("promise"));
    order.push("sync");
    await sleep(20);
    eq(order, ["sync", "microtask", "promise", "timeout"]);
  });
  await check("core: requestAnimationFrame gives increasing timestamps", async () => {
    const a = await nextFrame(), b = await nextFrame();
    if (!(b > a)) return "rAF times " + a + ", " + b;
  });
  await check("core: TextEncoder/TextDecoder round trip", () => {
    const s = "Pokémon ★ ポケモン";
    const b = new TextEncoder().encode(s);
    eq(b.length, 25, "UTF-8 length");
    eq(new TextDecoder().decode(b), s);
  });
  await check("core: btoa/atob", () => {
    eq(btoa("helloÿ"), "aGVsbG//");
    eq(atob("aGVsbG//"), "helloÿ");
  });
  await check("core: URL and URLSearchParams", () => {
    const u = new URL("../b/c.json?x=1&y=two#h", "https://example.org/a/d/e.html");
    eq(u.href, "https://example.org/a/b/c.json?x=1&y=two#h");
    eq(u.searchParams.get("y"), "two");
    eq(new URL("https://example.org:8080/p").host, "example.org:8080");
  });
  await check("core: crypto.getRandomValues and randomUUID", () => {
    const a = crypto.getRandomValues(new Uint8Array(32));
    if (a.every((x) => x === 0)) return "all zero";
    if (!/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(crypto.randomUUID())) return "bad UUID";
  });
  await check("core: Date and Intl basics", () => {
    if (!(Date.now() > Date.UTC(2024, 0, 1))) return "clock before 2024";
    eq(new Date(Date.UTC(2020, 1, 29)).toISOString(), "2020-02-29T00:00:00.000Z");
    eq(typeof new Intl.NumberFormat("en-US").format(1234.5), "string", "NumberFormat");
  });
  await check("core: structuredClone", () => {
    const o = { a: [1, { b: 2 }], m: new Map([["k", 1]]) };
    const c = structuredClone(o);
    eq(c.a, o.a);
    if (c.a === o.a || !(c.m instanceof Map) || c.m.get("k") !== 1) return "not a deep copy";
  });

  // ------------------------------------------------------------ network (served from this folder)
  await check("net: fetch JSON", async () => {
    const r = await fetch("data.json");
    eq(r.status, 200, "status");
    eq(await r.json(), { name: "smoke", list: [1, 2, 3], text: "Pokémon ★" });
  });
  await check("net: fetch of a missing file is a 404", async () => {
    const r = await fetch("does-not-exist.json");
    eq(r.ok, false, "ok");
    eq(r.status, 404, "status");
  });
  await check("net: XMLHttpRequest arraybuffer", async () => {
    const xhr = new XMLHttpRequest();
    xhr.open("GET", "data.json");
    xhr.responseType = "arraybuffer";
    await within(5000, new Promise((res, rej) => { xhr.onload = res; xhr.onerror = () => rej(new Error("xhr error")); xhr.send(); }), "xhr");
    eq(xhr.status, 200, "status");
    eq(JSON.parse(new TextDecoder().decode(xhr.response)).name, "smoke");
  });
  await check("net: dynamic import()", async () => {
    const m = await import("./mod.js");
    eq(m.answer, 42);
    eq(m.default(21), 42);
  });
  await check("net: import() of a bundler-style chunk (compiled code without source)", async () => {
    const m = await import("./assets/chunk-Ab_d-1xY.js");
    eq(m.twice(4), 8, "function");
    const c = new m.Counter();
    c.inc();
    eq(c.inc(), 2, "class");
    eq(typeof m.twice.toString(), "string", "toString");
    let stack = "";
    try { m.boom(); } catch (e) { stack = String(e.stack); }
    eq(/chunk-Ab_d-1xY\.js:\d+/.test(stack), true, "stack traces keep file and line: " + stack);
  });
  await check("gamefix: the sprite pipeline's palette code gives the same colours rewritten", async () => {
    const fixed = await import("./assets/sprite-Pk7rTst1.js"); // a chunk: src/gamefix.c rewrites it
    const text = await (await fetch("./assets/sprite-Pk7rTst1.js")).text();
    const plain = new Function(text.replace(/export function/g, "function") + "\nreturn { bindColors, batchColors, fieldBind, updateJumpIcons };")();
    class Pokemon {
      constructor(shiny, variant, key) { this.shiny = shiny, this.variant = variant, this.key = key; }
      getSprite() { return { texture: { key: this.key } }; }
    }
    const hex = (h) => ({ r: parseInt(h.slice(1, 3), 16), g: parseInt(h.slice(3, 5), 16), b: parseInt(h.slice(5, 7), 16), a: 255 });
    const cache = { pkmn_1: { 1: { "#102030": "#405060", "#a0b0c0": "#d0e0f0" } } };
    let zeros = 0; // colour arrays handed over as the shared zeros (no copy)
    const calls = (fn, ...args) => {
      const out = [], set = (n, v) => (v === globalThis.__pkrZero128 && zeros++, out.push(n + "=" + v.join(",")), p), p = { set4fv: set, set4iv: set };
      fn.apply(p, args);
      return out.join(" | ");
    };
    const binds = [[{}, false], [{ spriteColors: [[255, 0, 0, 255], [1, 2, 3, 4]] }, false], [{ fusionSpriteColors: [[5, 6, 7, 8]] }, false],
      [{ spriteColorsBase: [[9, 9, 9, 9]], spriteColors: [] }, true], [{ spriteColorsBase: [[9, 9, 9, 9]], spriteColors: [] }, false]];
    const batches = [{ pipelineData: {} }, { pipelineData: { shiny: true, spriteKey: "pkmn_1", variant: 1 } },
      { pipelineData: {}, parentContainer: new Pokemon(true, 1, "pkmn_1") }, { pipelineData: {}, parentContainer: new Pokemon(false, 1, "pkmn_1") },
      { pipelineData: { variant: 2 }, parentContainer: new Pokemon(true, 1, "pkmn_1") }];
    for (const [m, w] of binds) eq(calls(fixed.bindColors, m, w), calls(plain.bindColors, m, w), "onBind " + JSON.stringify(m) + " " + w);
    for (let pass = 0; pass < 2; pass++) // the second time, shiny colours come from gamefix's memo
      for (const b of batches) eq(calls(fixed.batchColors, b, Pokemon, cache, hex), calls(plain.batchColors, b, Pokemon, cache, hex), "onBatch " + JSON.stringify(b));
    if (!zeros) return "not rewritten (no [gamefix] line?)";
    if (!globalThis.__pkrMemo.has(cache.pkmn_1[1])) return "shiny colours not kept";
    // FieldSpritePipeline.onBind: binds of two sprites in a frame, then the arena changes and a new
    // frame binds. The uniforms after each bind are the same; repeated binds make fewer calls.
    let outside = 1, terrain = 2, arenaCalls = 0;
    const B = { currentBattle: { waveIndex: 7 }, waveCycleOffset: 3, arena: {
      isOutside: () => (arenaCalls++, outside === 1), getDayTint: () => [128, 128, 128],
      getDuskTint: () => (outside ? [113, 88, 101] : [0, 0, 0]), getNightTint: () => (outside ? [48, 48, 98] : [64, 64, 64]),
      get terrainType() { return terrain; } } };
    const over = () => [0, 0, 0], terrainColor = (t) => [[0, 0, 0], [232, 136, 200], [248, 248, 120]][t], now = () => 0.3;
    const fieldBinds = (fn, game) => {
      const states = [], u = {}, p = { game, currentShader: {} };
      let n = 0;
      for (const k of ["set1f", "setBoolean", "set3fv"]) p[k] = (name, v) => (n++, (u[name] = String(v)), p);
      const sprite = { pipelineData: { terrainColorRatio: 0.5 } }, other = { pipelineData: { ignoreTimeTint: true } };
      const bind = (o) => { fn.call(p, o, B, over, terrainColor, now); states.push(JSON.stringify(u)); };
      bind(sprite), bind(sprite), bind(other), bind(sprite);
      game.loop.frame++, outside = 0, terrain = 1;
      bind(sprite), bind(sprite);
      outside = 1, terrain = 2;
      return { states: states.join(" | "), n };
    };
    const ff = fieldBinds(fixed.fieldBind, { loop: { frame: 5 } }), fp = fieldBinds(plain.fieldBind, { loop: { frame: 5 } });
    eq(ff.states, fp.states, "field onBind");
    if (!(ff.n < fp.n)) return "field onBind: repeated binds not skipped (" + ff.n + " uniform calls, " + fp.n + " plain)";
    arenaCalls = 0;
    fieldBinds(fixed.fieldBind, { loop: { frame: 9 } });
    if (arenaCalls !== 2) return "arena tints not kept per frame (" + arenaCalls + " isOutside calls for 6 binds in 2 frames)";
    // PokemonIconAnimHelper's loop: live icons move the same; destroyed ones (no scene) are dropped.
    const helper = () => {
      const live = { scene: {}, y: 0 }, dead = { scene: undefined, y: 0 }, live2 = { scene: {}, y: 0 };
      const h = { icons: new Map([[live, { restY: 10, mode: 1 }], [dead, { restY: 20, mode: 2 }], [live2, { restY: 30, mode: 2 }]]),
        getCurrentOffset: (m) => -m };
      return { h, live, dead, live2 };
    };
    const hf = helper(), hp = helper();
    fixed.updateJumpIcons.call(hf.h);
    plain.updateJumpIcons.call(hp.h);
    eq([hf.live.y, hf.live2.y].join(), [hp.live.y, hp.live2.y].join(), "icon positions");
    if (hf.h.icons.size !== 2 || hf.h.icons.has(hf.dead) || hp.h.icons.size !== 3) return "destroyed icon not dropped (" + hf.h.icons.size + ")";
    // Icons in a hidden container are left as they are and set when it is shown, before drawing.
    {
      const P = globalThis.__pkrIcons, box = { visible: false }, outer = { visible: true };
      box.parentContainer = outer;
      const shown = { scene: {}, y: 0 }, hidden = { scene: {}, y: 0, parentContainer: box };
      let off = -1;
      const h = { icons: new Map([[shown, { restY: 10, mode: 1 }], [hidden, { restY: 20, mode: 1 }]]), getCurrentOffset: () => off };
      P.sync(h, true);
      if (shown.y !== 9 || hidden.y !== 0) return "hidden icon: shown " + shown.y + ", hidden " + hidden.y + " after a loop";
      off = -2;
      P.sync(h, true);
      P.flush();
      if (hidden.y !== 0) return "hidden icon moved while still hidden";
      box.visible = true;
      P.flush();
      if (hidden.y !== 18 || shown.y !== 8) return "hidden icon not set when shown: " + hidden.y;
    }
  });
  await check("gamefix: the starter select screen's slots and filters come out the same rewritten", async () => {
    const fixed = await import("./assets/starter-Pk7rTst2.js"); // a chunk: src/gamefix.c rewrites it
    const text = await (await fetch("./assets/starter-Pk7rTst2.js")).text();
    const plain = new Function(text.replace(/export (function|class)/g, "$1") + "\nreturn { useRegistry, makeSlots, StarterSelect };")();
    const species = Array.from({ length: 120 }, (_, i) => ({ id: i + 1, generation: 1 + (i % 9), types: [i % 18, (i * 7) % 18], favorite: i % 5 === 0 }));
    const registry = () => {
      const r = { calls: 0, getAllStarters() { r.calls++; return species.filter((s) => s.id % 3 !== 0).map((s) => s.id); },
        getSpecies: (id) => { const s = species[id - 1]; return { generation: s.generation, favorite: s.favorite, isOfType: (t) => s.types.includes(t) }; } };
      return r;
    };
    // the slots
    const slots = (mod) => {
      const r = registry();
      mod.useRegistry(r);
      class StarterContainer { constructor(id) { this.id = id; } }
      const out = mod.makeSlots((i) => ({ x: i % 9, y: Math.floor(i / 9) }), StarterContainer).map(([p, c]) => p.x + "," + p.y + ":" + c.id).join(" ");
      return { out, calls: r.calls };
    };
    const sf = slots(fixed), sp = slots(plain);
    eq(sf.out, sp.out, "starter slots");
    if (sf.calls !== 1) return "starter slots: getAllStarters called " + sf.calls + " times (plain " + sp.calls + ")";
    // the filters: a filter bar whose answer changes between filterings
    class FilterBar {
      constructor() { this.gens = [1, 2, 3, 5]; this.types = [2, 5, 9, 12]; this.fav = 0; this.calls = 0; }
      getVals(k) {
        this.calls++;
        if (k === 0) return this.gens.slice();
        if (k === 1) return this.types.map((t) => t + 1);
        return [{ val: "FAVORITE", state: this.fav }, { val: "WIN", state: 1 }];
      }
    }
    const filters = (mod) => {
      const r = registry();
      mod.useRegistry(r);
      const fb = new FilterBar(), ui = new mod.StarterSelect(fb), out = [];
      for (const [gens, fav] of [[[1, 2, 3, 5], 0], [[4, 6], 2], [[1, 2, 3, 4, 5, 6, 7, 8, 9], 1]]) {
        fb.gens = gens, fb.fav = fav;
        ui.filterStarters();
        out.push(ui.filteredStarterIds.join(","));
      }
      return { out: out.join(" | "), calls: fb.calls, own: Object.hasOwn(fb, "getVals") };
    };
    const ff = filters(fixed), fp = filters(plain);
    eq(ff.out, fp.out, "filtered starters");
    if (ff.calls !== 9) return "starter filters: getVals called " + ff.calls + " times for 3 filterings (plain " + fp.calls + ")";
    if (ff.own) return "starter filters: getVals not put back";
  });
  await check("net: Blob, object URLs and FileReader", async () => {
    const blob = new Blob(["abc", new Uint8Array([100, 101])], { type: "text/plain" });
    eq(blob.size, 5, "size");
    eq(await blob.text(), "abcde");
    const r = await fetch(URL.createObjectURL(blob));
    eq(await r.text(), "abcde", "blob: fetch");
    const fr = new FileReader();
    const done = new Promise((res) => { fr.onload = res; });
    fr.readAsText(blob);
    await within(2000, done, "FileReader");
    eq(fr.result, "abcde", "FileReader");
  });

  // ------------------------------------------------------------ storage
  await check("storage: localStorage and cookies", () => {
    localStorage.setItem("smoke", "1");
    eq(localStorage.getItem("smoke"), "1");
    eq(localStorage.smoke, "1", "property access");
    localStorage.removeItem("smoke");
    eq(localStorage.getItem("smoke"), null, "removed");
    document.cookie = "smoke=yes; path=/";
    if (!document.cookie.includes("smoke=yes")) return "cookie not set: " + document.cookie;
  });
  await check("storage: saved to the data folder by the writer thread", () => {
    const N = globalThis.__native, file = N.baseURL && String(N.baseURL).startsWith("https://offline.invalid") ? "localStorage-offline.json" : "localStorage.json";
    localStorage.setItem("smoke-saved", "on the card");
    globalThis.__rt.flushStorage(); // forced: waits for the writer thread
    const saved = JSON.parse(N.readFile(file) || "{}");
    eq(saved["smoke-saved"], "on the card", file);
    localStorage.removeItem("smoke-saved");
  });

  // ------------------------------------------------------------ images and Canvas 2D
  let img;
  await check("image: PNG decodes (worker thread)", async () => {
    img = new Image();
    const loaded = new Promise((res, rej) => { img.onload = res; img.onerror = () => rej(new Error("image error")); });
    img.src = PNG_3x2;
    await within(5000, loaded, "image load");
    eq([img.naturalWidth, img.naturalHeight], [3, 2]);
  });
  await check("canvas2d: fillRect, drawImage and getImageData", () => {
    const c = document.createElement("canvas");
    c.width = 8; c.height = 4;
    const ctx = c.getContext("2d");
    ctx.fillStyle = "#ffff00";
    ctx.fillRect(0, 0, 8, 4);
    ctx.drawImage(img, 0, 0);
    const d = ctx.getImageData(0, 0, 8, 1).data;
    eq(Array.from(d.slice(0, 4)), [255, 0, 0, 255], "red pixel");
    eq(Array.from(d.slice(4, 8)), [0, 255, 0, 255], "green pixel");
    eq(Array.from(d.slice(16, 20)), [255, 255, 0, 255], "background");
    const b = Array.from(d.slice(8, 12)); // half-transparent blue over yellow
    if (Math.abs(b[0] - 127) > 3 || Math.abs(b[2] - 128) > 3 || b[3] !== 255) return "blended pixel " + b;
  });
  await check("canvas2d: measureText", () => {
    const ctx = document.createElement("canvas").getContext("2d");
    ctx.font = "16px sans-serif";
    const m = ctx.measureText("Hello");
    if (typeof m.width !== "number" || !(m.width >= 0)) return "width " + m.width;
  });

  // ------------------------------------------------------------ WebGL
  await check("webgl: shaders compile and link (with a Cg-reserved identifier)", () => {
    const c = document.createElement("canvas");
    c.width = 960; c.height = 544;
    const gl = c.getContext("webgl");
    if (!gl) return "no context";
    // Names the first call that raised a GL error (vitaGL rejects some enums desktop GL accepts).
    const stale = [];
    for (let e; (e = gl.getError()) && stale.length < 8;) stale.push("0x" + e.toString(16));
    if (stale.length) console.log("[test] note: GL errors pending before the WebGL check: " + stale.join(" "));
    const ok = (step) => { const e = gl.getError(); if (e) throw new Error("GL error 0x" + e.toString(16) + " after " + step); };
    const sh = (type, src) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      ok("compileShader");
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
      return s;
    };
    const p = gl.createProgram();
    gl.attachShader(p, sh(gl.VERTEX_SHADER, "attribute vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }"));
    // "texture" is a reserved word in Cg; js/webgl.js renames it (as in Phaser's batch shader).
    gl.attachShader(p, sh(gl.FRAGMENT_SHADER, "precision mediump float; uniform vec4 color; " +
      "void main() { vec4 texture = color; gl_FragColor = texture; }"));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) return "link: " + gl.getProgramInfoLog(p);
    ok("linkProgram");
    gl.useProgram(p);
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(p, "pos");
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    ok("vertex setup");
    gl.uniform4f(gl.getUniformLocation(p, "color"), 0, 0.5, 1, 1);
    // Into a framebuffer object, as Phaser's render targets do (reading the screen's back buffer
    // is not reliable in Vita3K).
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 16, 16, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    const fb = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) return "framebuffer incomplete";
    ok("framebuffer setup");
    gl.viewport(0, 0, 16, 16);
    gl.clearColor(1, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    ok("uniform/viewport/clear");
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    ok("drawArrays");
    const px = new Uint8Array(4);
    gl.readPixels(8, 8, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    ok("readPixels");
    // On the Vita's GPU (vitaGL reports "SGX543MP4+") pixels are read back from GPU memory, which
    // Vita3K doesn't fill from its host renderer (always zeros): there only the calls are checked.
    const vitagl = /SGX543/.test(String(gl.getParameter(gl.RENDERER)));
    if (vitagl) console.log("[test] note: vitaGL: pixel readback not checked");
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.deleteFramebuffer(fb);
    gl.deleteTexture(tex);
    if (!vitagl && (px[0] > 5 || Math.abs(px[1] - 128) > 3 || px[2] < 250)) return "pixel " + Array.from(px);
  });

  // ------------------------------------------------------------ audio
  await check("audio: decodeAudioData decodes a WAV", async () => {
    const ctx = new AudioContext();
    const buf = await within(5000, ctx.decodeAudioData(makeWav()), "decode");
    if (Math.abs(buf.duration - 0.1) > 0.01) return "duration " + buf.duration;
    const src = ctx.createBufferSource();
    src.buffer = buf;
    src.connect(ctx.destination);
    src.start();
    await within(3000, new Promise((res) => { src.onended = res; }), "onended");
  });

  const failed = results.filter((r) => !r).length;
  console.log("[test] DONE passed=" + (results.length - failed) + " failed=" + failed);
}

run().catch((e) => console.log("[test] FAIL harness: " + e + "\n" + (e && e.stack)));

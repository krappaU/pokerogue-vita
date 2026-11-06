// SPDX-License-Identifier: AGPL-3.0-only
// Checks the native render fast paths (js/fastpath.js, src/fastpath.c) against Phaser's own code on
// a still scene built from what PokeRogue draws: containers (nested, scrolled, with alpha), images
// and sprites from an atlas (flips, tints, rotation, per-corner alpha), text, bitmap text, nine-slices, Graphics,
// rectangles, geometry masks, and a sprite pipeline that sets uniforms per object (as PokeRogue's
// sprite pipelines do).
// It runs the repo's verification probes (scripts/probe_verify_*.js, copied next to this page by the
// runner), which alternate native and Phaser code frame by frame and compare vertex data, uniform
// calls, draw calls and texture binds. Logs "[test] PASS/FAIL" lines and "[test] DONE ...".
import * as Phaser from "./phaser.js";
import * as starterChunk from "./starter-Pk7rTstB.js"; // a chunk: src/gamefix.c rewrites it

// The game's bundle assigns window.Phaser the same way; the runtime patches Phaser (pre-FX off,
// fast paths) the moment it is assigned (js/bootstrap.js).
globalThis.Phaser = Phaser;

const results = [];
function report(name, ok, detail) {
  results.push(ok);
  if (detail && detail.length > 400) detail = detail.slice(0, 400) + "…";
  console.log("[test] " + (ok ? "PASS " : "FAIL ") + name + (detail ? ": " + detail : ""));
}
function done() {
  const failed = results.filter((r) => !r).length;
  console.log("[test] DONE passed=" + (results.length - failed) + " failed=" + failed);
}

// A sprite pipeline that sets uniforms for every object it draws (like PokeRogue's SpritePipeline).
const FRAG = `
precision mediump float;
uniform sampler2D uMainSampler;
uniform vec4 uTone;
uniform float uFlash;
varying vec2 outTexCoord;
varying vec4 outTint;
void main() {
  vec4 c = texture2D(uMainSampler, outTexCoord) * outTint;
  gl_FragColor = vec4(mix(c.rgb, uTone.rgb, uTone.a) + uFlash, c.a);
}`;
class TonePipeline extends Phaser.Renderer.WebGL.Pipelines.SinglePipeline {
  constructor(game) { super({ game, fragShader: FRAG }); }
  onBind(go) {
    super.onBind(go);
    const t = (go && go.getData && go.getData("tone")) || [0, 0, 0, 0];
    this.set4f("uTone", t[0], t[1], t[2], t[3]);
    this.set1f("uFlash", go && go.getData ? go.getData("flash") || 0 : 0);
  }
}

class TestScene extends Phaser.Scene {
  constructor() { super("test"); }
  create() {
    // An atlas drawn on a canvas: 8 frames of 16x16 in distinct colours with a marked corner, so
    // flips and frame offsets change the output.
    const tex = this.textures.createCanvas("atlas", 128, 32);
    const ctx = tex.getContext();
    for (let i = 0; i < 8; i++) {
      ctx.fillStyle = `hsl(${i * 45}, 80%, 50%)`;
      ctx.fillRect(i * 16, 0, 16, 16);
      ctx.fillStyle = "#ffffff";
      ctx.fillRect(i * 16, 0, 4, 4);
      tex.add("f" + i, 0, i * 16, 0, 16, 16);
    }
    ctx.fillStyle = "#334455";
    ctx.fillRect(0, 16, 24, 16);
    ctx.fillStyle = "#aabbcc";
    ctx.fillRect(4, 20, 16, 8);
    tex.add("panel", 0, 0, 16, 24, 16);
    tex.refresh();

    this.renderer.pipelines.add("Tone", new TonePipeline(this.game));
    this.cameras.main.setBackgroundColor("#202830");

    // Loose sprites and images straight on the display list.
    for (let i = 0; i < 24; i++) {
      const s = (i % 2 ? this.add.sprite : this.add.image).call(this.add, 20 + i * 38, 30, "atlas", "f" + (i % 8));
      s.setScale(1 + (i % 3) * 0.5).setFlipX(i % 4 === 1).setFlipY(i % 5 === 2).setAngle(i * 7);
      if (i % 3 === 0) s.setTint(0xff8080, 0x80ff80, 0x8080ff, 0xffffff);
      if (i % 6 === 5) s.setAlpha(0.9, 0.6, 0.3, 1);
      s.setOrigin((i % 3) / 2, (i % 2) / 2);
    }
    // Containers: the native container walk (fast path 10) and container sprite shortcut (2).
    const outer = this.add.container(40, 80).setAlpha(0.8);
    for (let r = 0; r < 4; r++) {
      const row = this.add.container(0, r * 40).setScale(1 + r * 0.25).setAngle(r * 3);
      for (let i = 0; i < 12; i++) {
        const s = this.add.sprite(i * 24, 0, "atlas", "f" + ((i + r) % 8));
        if ((i + r) % 4 === 0) s.setTint(0xffd080);
        if (i % 5 === 3) s.setFlipX(true);
        if (i % 7 === 6) s.setAlpha(0.5);
        row.add(s);
      }
      outer.add(row);
    }
    // Scroll factors inside a container (the battle UI uses them), plus a scrolled camera below.
    const hud = this.add.container(600, 100);
    for (let i = 0; i < 6; i++) hud.add(this.add.image(i * 20, 0, "atlas", "f" + i).setScrollFactor(i % 2 ? 0 : 1));
    // Objects on the uniform-setting pipeline, inside and outside containers (Phaser's own path).
    const toned = this.add.container(600, 160);
    for (let i = 0; i < 8; i++) {
      const s = this.add.sprite(i * 22, 0, "atlas", "f" + i).setPipeline("Tone");
      s.setData("tone", [i / 8, 0.2, 1 - i / 8, (i % 4) / 4]).setData("flash", (i % 3) * 0.1);
      toned.add(s);
    }
    for (let i = 0; i < 4; i++) this.add.image(600 + i * 30, 200, "atlas", "f" + (7 - i)).setPipeline("Tone").setData("tone", [1, 0, 0, i / 4]);
    // Text (lazy text updates, fast path 4; style fast path 11) and nine-slices (fast path 5).
    this.add.text(40, 260, "PokeRogue Vita", { fontFamily: "sans-serif", fontSize: "20px", color: "#ffffff" });
    const box = this.add.container(40, 300);
    box.add(this.add.text(0, 0, "in a container", { fontSize: "14px", color: "#ffee88", stroke: "#000000", strokeThickness: 2 }));
    box.add(this.add.nineslice(200, 10, "atlas", "panel", 120, 40, 4, 4, 4, 4));
    this.add.nineslice(500, 300, "atlas", "panel", 200, 60, 6, 6, 6, 6).setTint(0x88ccff);

    // Graphics and shapes (fast paths 15, 16): fill/line styles + fillRect as PokeRogue's masks and
    // bars use them, a gradient, a path (Phaser's renderer), rectangles, and geometry masks on a
    // container and a sprite, nested.
    this.add.graphics().fillStyle(0x44aa66, 0.8).fillRect(760, 260, 60, 20).lineStyle(2, 0xffffff, 1)
      .fillGradientStyle(0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 1, 0.8, 0.6, 0.4).fillRect(830, 260, 60, 20);
    this.add.graphics().lineStyle(2, 0xffcc00, 1).strokeCircle(800, 330, 20).fillStyle(0x3366ff, 1).fillTriangle(840, 350, 880, 350, 860, 310);
    this.add.rectangle(760, 380, 50, 16, 0xcc4444, 0.9).setOrigin(0, 0);
    this.add.rectangle(830, 380, 50, 16, 0x44cccc).setStrokeStyle(2, 0xffffff);
    const maskShape = this.make.graphics().fillStyle(0xffffff).fillRect(40, 380, 200, 30);
    const masked = this.add.container(40, 370);
    for (let i = 0; i < 12; i++) masked.add(this.add.image(i * 20, 10 + (i % 3) * 8, "atlas", "f" + (i % 8)));
    masked.setMask(maskShape.createGeometryMask());
    const inner = this.make.graphics().fillStyle(0xffffff).fillRect(100, 385, 60, 40);
    masked.add(this.add.container(0, 0, [this.add.image(80, 20, "atlas", "f3").setScale(3).setMask(inner.createGeometryMask())]));
    this.add.image(300, 400, "atlas", "f5").setScale(4).setMask(this.make.graphics().fillRect(290, 390, 20, 20).createGeometryMask().setInvertAlpha(true));

    // Bitmap text (fast path 18; PokeRogue's item counts): a font of the atlas's 8 frames, plain, in a
    // scaled container, tinted with letter spacing, with a drop shadow, rotated with an origin, and one
    // long enough to flush mid-text (Phaser's batch holds 4096 quads), shadow included.
    this.cache.bitmapFont.add("digits", Phaser.GameObjects.RetroFont.Parse(this, {
      image: "atlas", width: 16, height: 16, chars: "01234567", charsPerRow: 8, spacing: { x: 0, y: 0 }, offset: { x: 0, y: 0 },
    }));
    this.add.bitmapText(300, 440, "digits", "0123 4567");
    box.add(this.add.bitmapText(0, 60, "digits", "7654", 8).setScale(1.5));
    this.add.bitmapText(500, 440, "digits", "1357").setTint(0xff8080, 0x80ff80, 0x8080ff, 0xffffff).setLetterSpacing(-2);
    this.add.bitmapText(620, 440, "digits", "26 04").setDropShadow(2, 2, 0x102030, 0.6).setAlpha(0.9);
    this.add.bitmapText(760, 470, "digits", "321").setOrigin(0.5).setAngle(-12).setScale(1.25);
    this.add.bitmapText(0, 500, "digits", "01234 567".repeat(560), 4).setDropShadow(1, 1, 0x000000, 0.5);

    this.cameras.main.setScroll(4, 2);
    this.frames = 0;
    this.game.events.on("postrender", () => this.frames++);
  }
}

const N = globalThis.__native;
// 128x64, transparent but for a 20x14 block at (70, 20) whose left column has alpha 128; 64x64 opaque.
const TRIM_PNG = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAIAAAABACAYAAADS1n9/AAADpElEQVR42u3SIavyCgCH8ZeBoFhsNpvNdtppp73tbfcTrKysDC4rgzEOLJyVnTA4rKwMm81ms9m8MBgIE0E2GAwERZAJz/0QN3n5/z7Cw/Prl4iIiIiIiIiIiIiIiIiIiIiIiIjI/9g/5t9+YXqU5icH84vK/OZk/nA2MxpzSWuu6Mw1F3PD1dxyN3c8zD1PswTziGHWDMyOoXljbPao6KsNYHl+YX1SWl8crG8q64eTlXG2ljTWitZa01kbLtaWq7Xjbu15WCVP6whWjWF1DKwbQ6tnbBka4OUGsD/9wv6itL852D9UdsbJXnK2VzT2mtbe0NlbLvaOq73nbpc87CNPuwa7w7BvDOyeoW0wtkca4OUGcL78wvmmdH44OBmVs+TkrDg7axpnQ+ts6ZwdF2fP1Sm5O0ceTs3T6cC5YTg9A8dg6IwYOxMN8HIDuN9+4f5QuhkHd0nlrji5a87uhsbd0ro7OnfPxS25ukfubs3D7Xi6N3B7DNdg4I4YuhPG7lQDvNwA3o9feBmlt+Tgrai8NSdvw9nb0ng7Wm9P55VcvCNXr+budTy8G0+vB8/A8EYMvAlDb8rYm2mAlxsgyPwiWFIGKw7BmirYcAq2nIMdTbCnDUq64MglqLkGHffgxiPoeQYGBCOMYMIgmDIMZoyDuQZ4uQHCpV+EK8pwzSHcUIVbTuGOc7inCUva8EgX1lzCjmt44x72PEKDZziCcIIRThmEM4bhnHG40AAvN0C08otoTRltOERbqmjHKdpzjkqa6Egb1XRRxyW6cY167pHBIxrxjCYQTTGiGYNozjBaMI7eNMDLDRCv/SLeUMZbDvGOKt5zikvO8ZEmrmnjji6+cYl7rrHBPR7xiCc84ynEM4x4ziBeMIzfGMfvGuDlBkg2fpFsKZMdh2RPlZSckiPnpKZJOtrkRpf0XBKDazLinkx4JFOeyQySOUayYJC8MUzeGScfGuDlBki3fpHuKNM9h7SkSo+c0ppz2tGkN9q0p0sNLumIazrhnk55pDOe6RzSBUb6xiB9Z5h+ME5/a4CXGyDb+UW2p8xKDtmRKqs5ZR3n7EaT9bSZQZeNuGQTrtmUezbjkc15ZgvI3jCydwbZB8PsN+PsjwZ4uQHyvV/kJWV+5JDXVHnHKb9xznua3KDNR3T5hEs+5ZrPuOdzHvmCZ/4G+TtG/sEg/80w/8M4/0sDiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIjIf/Qv9TgOfJxyyN0AAAAASUVORK5CYII=";
const OPAQUE_PNG = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAAgElEQVR42u3YMREAIAwEwciJiIhFDq7AAdSELShTsNXNR+ZYpzerju/1+wAAAAAAAAA+Buj+wds9AAAAAAAA8DOAEgQAAAAAAADsAUoQAAAAAAAAsAcoQQAAAAAAAMAeoAQBAAAAAAAAe4ASBAAAAAAAAOwBShAAAAAAAABoB7ABq8Jx8LibvVIAAAAASUVORK5CYII=";
const addBase64 = (scene, key, data) => new Promise((res) => {
  scene.textures.once("addtexture-" + key, res);
  scene.textures.addBase64(key, data);
});
const nextFrames = (game, n) => new Promise((res) => {
  let k = 0;
  const f = () => { if (++k >= n) { game.events.off("postrender", f); res(); } };
  game.events.on("postrender", f);
});

async function probe(game, file, fn, label) {
  const src = await (await fetch(file)).text();
  const armed = (0, eval)(src);
  const name = label ? file + " (" + label + ")" : file;
  if (armed !== "armed") return report(name, false, String(armed));
  await nextFrames(game, 14);
  const out = globalThis[fn]();
  // Every compared pair must be identical (and not empty, e.g. "identical (0 calls)" is suspicious).
  const parts = out.split(" | ").filter((p) => !/^\d+ functions$/.test(p));
  const bad = parts.filter((p) => !/: identical( \((\d+) calls\))?$/.test(p) || / \(0 calls\)$/.test(p));
  report(name, bad.length === 0 && parts.length === 4, bad.length ? out : parts[0]);
}

async function run() {
  const game = new Phaser.Game({
    type: Phaser.WEBGL, width: 960, height: 544, parent: document.body, banner: false,
    scene: TestScene, render: { antialias: false, pixelArt: true },
  });
  await new Promise((res) => game.events.once("ready", res));
  await nextFrames(game, 10);

  const rt = globalThis.__rt;
  report("phaser: runtime patches applied", !!rt.phaserPatched && !!rt.fastpath, "patched " + !!rt.phaserPatched);
  report("phaser: native container renderer active",
    Phaser.GameObjects.Container.prototype.renderWebGL === N.renderContainer);
  report("phaser: scene renders frames", game.scene.getScene("test").frames >= 10);
  // GL errors while frames render. Start-up ones are only noted: Phaser's framebuffer destroy()
  // (on the first resize) asks for attachments vitaGL doesn't track (GL_INVALID_OPERATION there).
  const gl = game.renderer.gl;
  const startup = [];
  for (let e; (e = gl.getError()) && startup.length < 8;) startup.push("0x" + e.toString(16));
  if (startup.length) console.log("[test] note: GL errors during start-up: " + startup.join(" "));
  await nextFrames(game, 10);
  const e = gl.getError();
  report("phaser: no GL errors while rendering", e === 0, e ? "0x" + e.toString(16) : "");

  await probe(game, "probe_verify_render.js", "__vfy");
  await probe(game, "probe_verify_batching.js", "__vfb");
  await probe(game, "probe_verify_uniforms.js", "__vfu");
  await probe(game, "probe_verify_bitmaptext.js", "__vbt");
  await probe(game, "probe_verify_tweens.js", "__vtw");
  report("phaser: native pipeline machinery and shapes active",
    !!(rt.pipelineSwaps && rt.graphicsSwap && rt.nineSliceSwap && rt.shapesSwap && rt.emitSwap && rt.bitmapTextSwap && rt.tweenSwap),
    ["pipelineSwaps", "graphicsSwap", "nineSliceSwap", "shapesSwap", "emitSwap", "bitmapTextSwap", "tweenSwap"].filter((k) => !rt[k]).join(" "));
  {
    const out = (0, eval)(await (await fetch("probe_verify_emit.js")).text());
    report("probe_verify_emit.js", /^identical: /.test(out), out.slice(0, 200));
  }
  // js/input.js: Gamepad.update skips a pad that hasn't changed since its last update. A sequence of
  // pad states (changes and repeats) goes through Phaser's own update and the patched one.
  {
    const [orig, fast] = rt.gamepadSwap || [];
    let detail = !orig ? "not installed" : "";
    if (orig) {
      const mk = () => {
        const mgr = new Phaser.Events.EventEmitter(), log = [];
        mgr.on("down", (pad, b, v) => log.push("down" + b.index + "=" + v)).on("up", (pad, b, v) => log.push("up" + b.index + "=" + v));
        const pad0 = { id: "pad", index: 0, timestamp: 1e12, axes: [0, 0, 0, 0], buttons: Array.from({ length: 18 }, () => ({ value: 0 })) };
        const gp = new Phaser.Input.Gamepad.Gamepad(mgr, pad0);
        gp.on("down", (i, v) => log.push("pad-down" + i + "=" + v)).on("up", (i, v) => log.push("pad-up" + i + "=" + v));
        return { gp, log };
      };
      const a = mk(), b = mk();
      const pad = { id: "pad", index: 0, timestamp: 1e12, axes: [0, 0, 0, 0], buttons: [] };
      const steps = [[0, 0, 0], [1, 1, 0], [1, 1, 0], [2, 1, 0.7], [2, 1, 0.7], [3, 0, -0.3], [3, 0, -0.3], [4, 0, 0]];
      for (const [seq, b0, ax] of steps) {
        pad._pkrSeq = seq, pad.timestamp += 16;
        pad.buttons = Array.from({ length: 18 }, (_, i) => ({ value: i === 0 ? b0 : i === 13 ? 1 - b0 : 0 }));
        pad.axes = [ax, 0, 0, -ax];
        fast.call(a.gp, pad);
        orig.call(b.gp, pad);
        const st = (g) => g.buttons.map((x) => x.value + "/" + x.pressed).join() + " " + g.axes.map((x) => x.value).join() + " " + g.leftStick.x + "," + g.rightStick.y;
        if (!detail && (st(a.gp) !== st(b.gp) || a.log.join() !== b.log.join())) detail = "step " + seq + ": " + st(a.gp) + " " + a.log.join() + " vs " + st(b.gp) + " " + b.log.join();
      }
      if (!detail && a.log.length < 4) detail = "too few events: " + a.log.join();
    }
    report("input: gamepad updates skipped while the pad is unchanged give Phaser's state and events", !detail, detail);
  }
  // js/fastpath.js 6b: a new Text makes no GPU texture just to delete it; it gets one when drawn.
  {
    const sc = game.scene.getScene("test"), gl = game.renderer.gl, mk = gl.createTexture;
    let made = 0;
    gl.createTexture = function () { made++; return mk.apply(this, arguments); };
    let t;
    try { t = sc.add.text(10, 10, "Gamefix", { fontFamily: "sans-serif", fontSize: "24px", color: "#ffffff" }); } finally { gl.createTexture = mk; }
    const atBuild = made, nullAfter = t.frame.source.glTexture === null;
    await nextFrames(game, 3);
    const drawn = !!t.frame.source.glTexture;
    t.destroy();
    const detail = !rt.textTextureSwap ? "not installed" : atBuild ? atBuild + " textures made while building" :
      !nullAfter ? "glTexture not null after the constructor" : !drawn ? "no texture after drawing" : "";
    report("fastpath: a new text makes no texture until drawn", !detail, detail);
  }
  // js/fastpath.js 4: measuring a new text makes no texture either, and a destroyed text's texture
  // goes to a new text drawn with the same inputs (not to one with other words), whose pixels
  // Phaser's own drawing gives the same.
  {
    const sc = game.scene.getScene("test"), gl = game.renderer.gl, mk = gl.createTexture;
    const style = { fontFamily: "sans-serif", fontSize: "24px", color: "#ffffff" };
    const make = (y, s) => sc.add.text(10, y, s, style).setShadow(2, 2, "#333333").setPadding(1);
    const pixels = (t) => Array.from(t.context.getImageData(0, 0, t.canvas.width, t.canvas.height).data).join();
    let made = 0, a;
    gl.createTexture = function () { made++; return mk.apply(this, arguments); };
    try { a = make(40, "Kept 1/2"); void a.width; } finally { gl.createTexture = mk; }
    const measured = a.width > 1 && !made && a.frame.source.glTexture === null;
    await nextFrames(game, 3);
    const texA = a.frame.source.glTexture, size = a.width + "x" + a.height, pixA = pixels(a);
    const reused0 = rt.textStats().reused;
    a.destroy();
    const b = make(40, "Kept 1/2"), c = make(80, "Kept 2/2");
    await nextFrames(game, 3);
    rt.textReuse = false;
    const d = make(120, "Kept 1/2");
    await nextFrames(game, 3);
    rt.textReuse = true;
    const detail = !rt.textCache ? "not installed" : !measured ? "measuring made a texture (" + made + ")"
      : !texA ? "no texture after drawing" : b.frame.source.glTexture !== texA ? "the kept texture wasn't reused"
      : c.frame.source.glTexture === texA || !c.frame.source.glTexture ? "a text with other words got it"
      : rt.textStats().reused !== reused0 + 1 ? "reuse count " + (rt.textStats().reused - reused0)
      : b.width + "x" + b.height !== size ? "size " + b.width + "x" + b.height + " vs " + size
      : pixels(d) !== pixA ? "Phaser's drawing of the same inputs differs" : "";
    b.destroy(), c.destroy(), d.destroy();
    report("fastpath: measuring makes no texture; a destroyed text's texture serves the same text", !detail, detail);
  }
  // js/fastpath.js 4b: rex's BBCodeText (the game's rich text), built the way PokeRogue builds its
  // option lists (constructor, setShadow, setLineSpacing, two setTexts, then its width), gives the
  // same sizes, lines and canvas pixels as rex's own code, with one draw per text; destroyed ones'
  // textures serve the same texts again.
  {
    const sc = game.scene.getScene("test");
    let detail = "";
    try {
      // Phaser's ES module build has no Phaser.Class (the game's full build does), which rex uses
      // to mix Phaser's components into its classes: the same as Phaser's Class.mixin.
      const mixin = (cls, list) => {
        for (const m of [].concat(list)) for (const k of Object.keys(m)) {
          const v = m[k];
          if (v && typeof v === "object" && (typeof v.get === "function" || typeof v.set === "function"))
            Object.defineProperty(cls.prototype, k, { get: v.get, set: v.set, configurable: true, enumerable: true });
          else cls.prototype[k] = v;
        }
      };
      const ns = globalThis.Phaser;
      if (!ns.Class) globalThis.Phaser = Object.assign({}, ns, { Class: { mixin } });
      // (a UMD script: evaluated as a classic script it sets globalThis.rexbbcodetextplugin)
      try { (0, eval)(await (await fetch("./rexbbcodetextplugin.js")).text()); } finally { globalThis.Phaser = ns; }
      game.plugins.install("rexBBCodeTextPlugin", globalThis.rexbbcodetextplugin, true);
    } catch (e) { detail = "plugin: " + e + " " + String(e.stack).slice(0, 300); }
    if (!detail && !rt.rexTextPatched) detail = "not patched";
    if (!detail) {
      const words = ["Leftovers", "All (12)", "[color=#e05040]Big Root[/color]", "↑", "Choice Specs", "Lum Berry"];
      const build = (i) => {
        const s = words[i];
        const t = sc.add.rexBBCodeText(20, 20 + i * 30, s, { fontFamily: "sans-serif", fontSize: "48px", color: "#f8f8f8", padding: { bottom: 6 }, maxLines: 1 });
        t.setScale(1 / 3).setShadow(4, 5, "#606060");
        t.setLineSpacing(20);
        if (i % 2) t.setText(s + " [color=#f89890](" + i + ")[/color]");
        t.setText("[shadow]" + t.text + "[/shadow]");
        return t;
      };
      const state = (t) => [t.width, t.height, t.displayWidth, t.displayOriginX, t.getWrappedText().join("|")].join(",");
      const pixels = (t) => Array.from(t.context.getImageData(0, 0, t.canvas.width, t.canvas.height).data).join();
      const round = async (lazy) => {
        rt.fastpath.set(lazy);
        const s0 = rt.rexTextStats();
        const ts = words.map((_, i) => build(i));
        const st = ts.map(state);
        await nextFrames(game, 3);
        const s1 = rt.rexTextStats();
        return { ts, st, px: ts.map(pixels), draws: s1.draws - s0.draws, updates: s1.updates - s0.updates, reused: s1.reused - s0.reused };
      };
      const ref = await round(false);
      ref.ts.forEach((t) => t.destroy());
      const a = await round(true);
      const texs = a.ts.map((t) => t.frame.source.glTexture);
      a.ts.forEach((t) => t.destroy());
      const b = await round(true);
      const reusedTex = b.ts.filter((t, i) => t.frame.source.glTexture === texs[i]).length;
      const st2 = b.ts.map(state);
      b.ts.forEach((t) => t.destroy());
      const bad = a.st.findIndex((s, i) => s !== ref.st[i]), badPx = a.px.findIndex((p, i) => p !== ref.px[i]);
      detail = ref.updates ? "rex's own code was patched while off (" + ref.updates + ")"
        : bad >= 0 ? "size or lines of \"" + words[bad] + "\": " + a.st[bad] + " vs " + ref.st[bad]
        : badPx >= 0 ? "pixels of \"" + words[badPx] + "\" differ"
        : a.draws !== words.length ? a.draws + " draws for " + words.length + " texts (" + a.updates + " changes)"
        : a.updates < 3 * words.length ? "only " + a.updates + " changes went through the patch"
        : b.reused !== words.length || reusedTex !== words.length ? "textures reused: " + b.reused + " (" + reusedTex + " the same)"
        : st2.some((s, i) => s !== ref.st[i]) ? "sizes after reuse differ" : "";
    }
    rt.fastpath.set(true);
    report("fastpath: rex BBCodeText drawn once, when rendered, the same as rex's own drawing", !detail, detail);
  }
  // js/fastpath.js 6c: a canvas texture reads its canvas back only when its pixel copy is used. Its
  // pixel methods give what Phaser's own code gives on this runtime, for a canvas blank when the
  // texture is made and for one with a drawing, before and after drawing more and update().
  {
    const sc = game.scene.getScene("test");
    let detail = rt.canvasCopySwap ? "" : "not installed";
    if (!detail) {
      const CP = Phaser.Display.Canvas.CanvasPool, px = (p) => (p ? [p.r, p.g, p.b, p.a].join(",") : "none");
      const run = (lazy, drawnFirst) => {
        rt.lazyCanvasCopy = lazy;
        const c = CP.create2D(sc, 64, 48), cx = c.getContext("2d"), key = "copy-test-" + lazy + drawnFirst;
        const draw = () => { cx.fillStyle = "#ff8000", cx.fillRect(0, 0, 64, 48), cx.fillStyle = "#0040ff", cx.fillRect(10, 5, 4, 4); };
        if (drawnFirst) draw();
        const s0 = rt.canvasCopyStats();
        const tex = sc.textures.addCanvas(key, c);
        const copiesWhenMade = rt.canvasCopyStats().copies - s0.copies;
        const out = [px(tex.getPixel(11, 6)), px(tex.getPixel(40, 40))];
        if (!drawnFirst) draw();
        cx.fillStyle = "#00ff00", cx.fillRect(40, 40, 2, 2);
        tex.update();
        out.push(px(tex.getPixel(11, 6)), px(tex.getPixel(40, 40)));
        sc.textures.remove(key);
        rt.lazyCanvasCopy = true;
        return { out: out.join(" "), copiesWhenMade };
      };
      for (const drawnFirst of [false, true]) {
        const ref = run(false, drawnFirst), got = run(true, drawnFirst);
        if (!detail && got.out !== ref.out) detail = (drawnFirst ? "drawn" : "blank") + " canvas: " + got.out + " vs Phaser's " + ref.out;
        if (!detail && got.copiesWhenMade) detail = "read back when made";
        if (!detail && !/0,255,0,255$/.test(got.out)) detail = "update() didn't read the canvas: " + got.out;
      }
    }
    report("fastpath: a canvas texture copies its pixels only when they are used", !detail, detail);
  }
  // js/fastpath.js 21: the input plugin's object list kept with sets gives Phaser's list, order and
  // input states through objects made interactive, destroyed, disabled and enabled again, and new ones.
  {
    const sc = game.scene.getScene("test");
    let detail = rt.inputListSwap ? "" : "not installed";
    if (!detail) {
      const run = async (on) => {
        rt.fastpath.set(on);
        const mk = (n) => sc.add.image(10 + n, 10, "atlas", 0).setName("i" + n).setInteractive();
        const objs = Array.from({ length: 30 }, (_, i) => mk(i));
        await nextFrames(game, 2);
        for (const i of [3, 7, 8, 20]) objs[i].destroy();
        objs[11].disableInteractive(), objs[12].removeInteractive();
        objs[12].setInteractive(); // removed, then made interactive again before the update
        const more = Array.from({ length: 5 }, (_, i) => mk(100 + i));
        more[2].destroy(); // queued and destroyed in the same frame
        await nextFrames(game, 2);
        objs[5].removeInteractive(), objs[5].setInteractive();
        await nextFrames(game, 2);
        const all = objs.concat(more);
        const out = sc.input._list.filter((o) => all.includes(o)).map((o) => o.name).join(",") + " | " +
          all.map((o) => (o.input ? (o.input.enabled ? "e" : "d") : "-")).join("");
        all.forEach((o) => o.scene && o.destroy());
        await nextFrames(game, 2);
        return out + " | left " + sc.input._list.filter((o) => all.includes(o)).length;
      };
      const ref = await run(false), got = await run(true);
      rt.fastpath.set(true);
      detail = got !== ref ? got + " vs Phaser's " + ref : "";
    }
    report("fastpath: the input plugin's object list kept with sets is Phaser's", !detail, detail);
  }
  // js/sprite_gc.js: the textures (and animations) it must never unload are found: shown by an object
  // in a container on the display list, by a sprite on no list, by an image only a root holds, and
  // played as an animation by a sprite on no list; one nothing uses is not.
  {
    const sc = game.scene.getScene("test");
    let detail = rt.spriteGcInUse ? "" : "not installed";
    if (!detail) {
      const img = sc.textures.get("atlas").getSourceImage();
      const keys = ["pkmn__a", "pkmn__b", "pkmn__c", "pkmn__d", "pkmn__e"];
      for (const k of keys) sc.textures.addImage(k, img);
      sc.anims.create({ key: "pkmn__c", frames: [{ key: "pkmn__c", frame: "__BASE" }], repeat: -1 });
      const box = sc.add.container(0, 0);
      box.add(sc.add.sprite(0, 0, "pkmn__a"));
      const b = sc.make.sprite({ key: "pkmn__b" }, false);
      const c = sc.make.sprite({ key: "atlas" }, false);
      c.play("pkmn__c");
      const e = sc.make.image({ key: "pkmn__e" }, false);
      const used = rt.spriteGcInUse(game, [[e]]);
      const got = ["a", "b", "c", "d", "e"].filter((k) => used.has("pkmn__" + k)).join("");
      detail = got !== "abce" ? "found " + got + " (want abce)" : "";
      box.destroy(), b.destroy(), c.destroy(), e.destroy();
      sc.anims.remove("pkmn__c");
      for (const k of keys) sc.textures.remove(k);
    }
    report("sprite_gc: textures in use are found on lists, off them and in animations", !detail, detail);
  }
  // gamefix.c "starter icons kept, not remade" (js/core.js __pkrStarterIcon): a starter slot's icon set
  // again comes out as the game's new one would: the slot's children in the same order, the icon's
  // place, size, look and frame, no tweens left on it, out of the icon animation helper.
  {
    const sc = game.scene.getScene("test");
    let detail = "";
    try {
      const fixed = starterChunk;
      const text = await (await fetch("./starter-Pk7rTstB.js")).text();
      const plain = new Function(text.replace(/export (function|class)/g, "$1") + "\nreturn { make };")();
      const species = (n) => ({ getIconAtlasKey: () => "atlas", getIconId: (female) => "f" + ((n + (female ? 4 : 0)) % 8) });
      const z = { getSpecies: (id) => species(id) };
      const run = (mod) => {
        const helper = { icons: new Map() };
        const saved = sc.ui;
        sc.ui = { handlers: [] };
        const { StarterContainer } = mod.make(sc, z);
        const slot = new StarterContainer(1);
        sc.ui.handlers.push({ starterContainers: [slot], iconAnimHandler: helper });
        const out = [];
        let made = 0;
        const add = sc.add.sprite;
        sc.add.sprite = function () { made++; return add.apply(this, arguments); };
        try {
          for (const [id, female] of [[1, false], [2, true], [3, false], [3, false]]) {
            slot.setSpecies(id, { shiny: false, formIndex: 0, female, variant: 0 });
            const i = slot.icon;
            out.push(slot.list.map((o) => o.name || o.type).join(",") + "|" + [i.x, i.y, i.scaleX, i.originX, i.alpha, i.visible,
              i.tintTopLeft, i.tintFill, i.texture.key, i.frame.name, i.angle, i.flipX].join(",") + "|tweens " +
              sc.tweens.getTweensOf(i).length + "|helper " + helper.icons.has(i));
            // what the screen does to an icon between refreshes: an upgrade animation, the helper,
            // the value check's alpha, a tint
            sc.tweens.add({ targets: i, y: 5, duration: 1000, yoyo: true, repeat: -1 });
            helper.icons.set(i, { mode: 3, restY: 2 });
            i.setAlpha(0.375).setTint(0x808080).setVisible(false);
          }
        } finally { sc.add.sprite = add; }
        slot.destroy();
        sc.ui = saved;
        return { out: out.join(" / "), made };
      };
      const p = run(plain), f = run(fixed);
      detail = f.out !== p.out ? f.out + " VS the game's " + p.out : f.made !== 1 ? "kept version made " + f.made + " sprites (the game " + p.made + ")" : "";
    } catch (e) { detail = String(e) + " " + (e && e.stack); }
    report("gamefix: a starter slot's kept icon is set as the game's new one", !detail, detail);
  }
  // js/fastpath.js 7b: a child taken out of a container (not destroyed) loses the DESTROY listener the
  // container gave it; Phaser 3.90 leaves it, one more each time the child is added back (log225).
  // The container's list and the child's destruction come out the same either way.
  {
    const sc = game.scene.getScene("test");
    let detail = rt.containerListenerFix ? "" : "not installed";
    if (!detail) {
      const run = (on) => {
        rt.fastpath.set(on);
        const box = sc.add.container(0, 0), other = sc.add.container(0, 0);
        const a = sc.add.image(0, 0, "atlas", 0).setName("a"), b = sc.add.image(0, 0, "atlas", 0).setName("b");
        for (let i = 0; i < 5; i++) { box.add([a, b]); box.removeAll(); }
        box.add([a, b]); box.remove(b);
        other.add(b);
        const listeners = a.listenerCount("destroy") + "/" + b.listenerCount("destroy");
        a.destroy();
        const out = box.list.map((o) => o.name).join(",") + " | " + other.list.map((o) => o.name).join(",");
        b.destroy();
        const after = box.list.length + "," + other.list.length;
        box.destroy(), other.destroy();
        return { listeners, out: out + " | " + after };
      };
      const ref = run(false), got = run(true);
      rt.fastpath.set(true);
      detail = got.out !== ref.out ? got.out + " vs Phaser's " + ref.out
        : got.listeners !== "1/1" ? "destroy listeners " + got.listeners + " (Phaser's " + ref.listeners + ")" : "";
    }
    report("fastpath: a container takes its DESTROY listener off a child it lets go", !detail, detail);
  }
  // js/lazy_ui.js: handlers PokeRogue's UI sets up at each reset are built on first use instead. A
  // never-built one is destroyed without its destroy() (which reads what setup() made); a used one is
  // built then, its objects in the order they would have had; a built one's destroy() runs.
  {
    const sc = game.scene.getScene("test");
    let detail = rt.lazyUiStats ? "" : "not installed";
    if (!detail) {
      const mk = () => {
        const ui = sc.add.container(0, 0), log = [];
        class Base { constructor(n) { this.n = n; } setup() { this.box = sc.make.image({ key: "atlas" }, false).setName(this.n); ui.add(this.box); } show() { log.push("show " + this.n); return this.box.name; } destroy() { log.push("destroy " + this.box.name); } }
        // (the names matter: lazy_ui defers by class name)
        const C = { GameChallengesUiHandler: class GameChallengesUiHandler extends Base {}, AchvsUiHandler: class AchvsUiHandler extends Base {},
          PartyUiHandler: class PartyUiHandler extends Base {} };
        ui.handlers = [new C.PartyUiHandler("party"), new C.GameChallengesUiHandler("challenges"), new C.AchvsUiHandler("achvs")];
        return { ui, log };
      };
      const order = (ui) => ui.list.map((o) => o.name).join(",");
      // reference: the same calls with the handlers set up as the game does, all at once
      const ref = mk();
      for (const h of ref.ui.handlers) h.setup();
      const s0 = rt.lazyUiStats();
      const a = mk();
      a.ui.setName("ui-test"); // what UI.setup does first: lazy_ui looks at the handlers then
      for (const h of a.ui.handlers) h.setup();
      const deferred = rt.lazyUiStats().deferred - s0.deferred;
      const shown = a.ui.handlers[2].show();
      let threw = "";
      try { a.ui.handlers[1].destroy(); a.ui.handlers[2].destroy(); a.ui.handlers[0].destroy(); } catch (e) { threw = String(e); }
      detail = deferred !== 2 ? deferred + " handlers deferred (want 2)" : shown !== "achvs" ? "show gave " + shown
        : threw ? "destroy threw: " + threw : a.log.join(";") !== "show achvs;destroy achvs;destroy party" ? "calls: " + a.log.join(";")
        : order(a.ui) !== "party,achvs" ? "order " + order(a.ui) + " (reference " + order(ref.ui) + ")" : "";
      a.ui.destroy(), ref.ui.destroy();
    }
    report("lazy_ui: deferred handlers built on use, destroyed unbuilt without their destroy()", !detail, detail);
  }
  // js/fastpath.js trimmed quads: images drawn without the transparent part of their quads (in a
  // container: src/fastpath.c; alone: batchFast) give the same frame as the whole quads. Whole
  // scales: where a pixel centre falls exactly on a texel edge (scale 1.25: every 5 pixels), either
  // texel is nearest and float rounding picks one, differently for a smaller quad.
  {
    const sc = game.scene.getScene("test"), r = game.renderer, gl = r.gl;
    await addBase64(sc, "trim-test", TRIM_PNG);
    const shown = sc.children.list.filter((o) => o.visible);
    shown.forEach((o) => o.setVisible(false));
    const cont = sc.add.container(0, 0);
    const a = sc.make.image({ x: 200, y: 110, key: "trim-test" }, false).setScale(3);
    const b = sc.make.image({ x: 500, y: 300, key: "trim-test" }, false).setFlipX(true).setScale(2, 3);
    cont.add([a, b]);
    const top = sc.add.image(700, 100, "trim-test").setScale(2).setAlpha(0.8);
    const frame = () => new Promise((res) => game.events.once("postrender", () => {
      const px = new Uint8Array(gl.drawingBufferWidth * gl.drawingBufferHeight * 4);
      gl.readPixels(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight, gl.RGBA, gl.UNSIGNED_BYTE, px);
      res(px);
    }));
    const pipe = r.pipelines.get("MultiPipeline");
    await nextFrames(game, 2);
    const on = await frame();
    const trimmed = Array.isArray(a.frame._pkrTrim) && pipe._pkrTrim === 2;
    pipe._pkrTrim = 0;
    await nextFrames(game, 2);
    const off = await frame();
    delete pipe._pkrTrim;
    let diff = 0, lit = 0;
    for (let i = 0; i < on.length; i += 4) { if (on[i] !== off[i] || on[i + 1] !== off[i + 1] || on[i + 2] !== off[i + 2]) diff++; if (off[i] > 60) lit++; }
    // Vita3K doesn't fill pixels read back from GPU memory: there only the trim itself is checked.
    const vitagl = /SGX543/.test(String(gl.getParameter(gl.RENDERER)));
    const detail = !rt.trimInfo ? "not installed" : !trimmed ? "no trim for the frame (" + JSON.stringify(a.frame._pkrTrim) + ")"
      : vitagl ? "" : !lit ? "nothing drawn" : diff ? diff + " pixels differ" : "";
    cont.destroy(), top.destroy();
    shown.forEach((o) => o.setVisible(true));
    report("fastpath: trimmed quads draw the same pixels", !detail, detail);
  }
  // js/webgl.js framebuffer attachments: Phaser's WebGLFramebufferWrapper.destroy deletes the
  // renderbuffer and texture getFramebufferAttachmentParameter names (review-2026-10.md finding 3).
  {
    const r = game.renderer, gl = r.gl, W = Phaser.Renderer.WebGL.Wrappers;
    let detail = !W || !W.WebGLFramebufferWrapper ? "no WebGLFramebufferWrapper" : "";
    if (!detail) {
      const mk = gl.createRenderbuffer, made = [];
      gl.createRenderbuffer = function () { const rb = mk.apply(this, arguments); made.push(rb); return rb; };
      let tex, fb;
      try {
        tex = r.createTexture2D(0, gl.NEAREST, gl.NEAREST, gl.CLAMP_TO_EDGE, gl.CLAMP_TO_EDGE, gl.RGBA, null, 64, 64);
        fb = new W.WebGLFramebufferWrapper(gl, 64, 64, tex, true);
      } finally { gl.createRenderbuffer = mk; }
      const rb = made[0], glTex = tex.webGLTexture;
      const before = !!rb && gl.isRenderbuffer(rb) && gl.isTexture(glTex);
      fb.destroy();
      detail = !before ? "no renderbuffer made (" + made.length + ")" : gl.isRenderbuffer(rb) ? "renderbuffer left after destroy"
        : gl.isTexture(glTex) ? "texture left after destroy" : "";
      tex.destroy(); // deleted already: a second delete does nothing
    }
    report("webgl: a destroyed framebuffer frees its renderbuffer and texture", !detail, detail);
  }
  // js/fastpath.js covered objects: an opaque image covering the camera hides the top-level
  // children before its own; the interactive ones still go in the camera's render list, in drawing
  // order (the input plugin sorts by it). Not when the cover isn't opaque.
  {
    const sc = game.scene.getScene("test"), cam = sc.cameras.main, sx = cam.scrollX, sy = cam.scrollY;
    await addBase64(sc, "cover-test", OPAQUE_PNG);
    cam.setScroll(0, 0); // (only an unscrolled camera is checked)
    const below = sc.add.container(0, 0), belowImg = sc.make.image({ x: 10, y: 10, key: "trim-test" }, false);
    below.add(belowImg);
    belowImg.setInteractive();
    const mid = sc.add.image(20, 20, "trim-test").setInteractive();
    const ui = sc.add.container(0, 0);
    const cover = sc.make.image({ x: 0, y: 0, key: "cover-test" }, false).setOrigin(0).setDisplaySize(cam.worldView.width, cam.worldView.height);
    ui.add(cover);
    const s0 = rt.coverStats ? rt.coverStats().skipped : 0;
    await nextFrames(game, 35); // (looked for every 30 frames)
    const s1 = rt.coverStats ? rt.coverStats().skipped : 0;
    const rl = cam.renderList, at = (o) => rl.indexOf(o);
    const listed = at(belowImg) >= 0 && at(belowImg) < at(mid) && at(mid) < at(cover);
    cover.setAlpha(0.99);
    await nextFrames(game, 3);
    const s2 = rt.coverStats ? rt.coverStats().skipped : 0;
    await nextFrames(game, 3);
    const s3 = rt.coverStats ? rt.coverStats().skipped : 0;
    const detail = !rt.coverStats ? "not installed" : !(s1 > s0) ? "nothing skipped under the cover"
      : !listed ? "skipped objects missing from the render list" : s3 !== s2 ? "skipped under a translucent cover" : "";
    below.destroy(), mid.destroy(), ui.destroy();
    cam.setScroll(sx, sy);
    report("fastpath: an opaque screen-covering image hides what is under it", !detail, detail);
  }
  // js/bootstrap.js: a destroyed timer event leaves its Clock's list; live and re-added ones keep running.
  {
    const clock = game.scene.getScene("test").time;
    let live = 0, readded = 0;
    const keep = clock.addEvent({ delay: 5, loop: true, callback: () => live++ });
    const gone = clock.addEvent({ delay: 5, loop: true, callback: () => {} });
    const once = clock.addEvent({ delay: 100000, callback: () => {} });
    const again = clock.addEvent({ delay: 5, loop: true, callback: () => {} });
    await nextFrames(game, 3);
    gone.destroy(), once.destroy(), again.destroy();
    clock.addEvent(again.reset({ delay: 5, loop: true, callback: () => readded++ }));
    await nextFrames(game, 5);
    const active = clock._active;
    const detail = !rt.timerSwap ? "not installed"
      : active.includes(gone) || active.includes(once) ? "destroyed events still in the clock's list"
      : !active.includes(keep) || !active.includes(again) ? "a live or re-added event left the list"
      : !(live > 3 && readded > 1) ? "events stopped running (" + live + ", " + readded + ")" : "";
    keep.remove(), again.remove();
    report("bootstrap: destroyed timer events leave the clock; live and re-added ones run", !detail, detail);
  }
  // js/input.js: DOMElement.preRender leaves out the display = "none" write when already "none".
  {
    const [orig, fast] = rt.domPreRenderSwap || [];
    let detail = !orig ? "not installed" : "";
    if (orig) {
      const run = (fn) => {
        const out = [];
        for (const [shown, display] of [[true, ""], [false, ""], [false, "none"], [true, "none"], [false, "block"]]) {
          const node = document.createElement("input");
          if (display) node.style.display = display;
          fn.call({ parentContainer: { willRender: () => shown }, node });
          out.push(node.style.display + "|" + node.style.cssText);
        }
        fn.call({ parentContainer: null, node: document.createElement("input") });
        return out.join(" ");
      };
      const x = run(fast), y = run(orig);
      if (x !== y) detail = x + " vs " + y;
    }
    report("input: DOMElement.preRender's style write left out when it changes nothing", !detail, detail);
  }
  for (const which of ["pipeline", "graphics", "nineslice", "shapes", "emit"]) {
    globalThis.__vfpWhich = which;
    await probe(game, "probe_verify_pipeline.js", "__vfp", which);
  }
  done();
}

run().catch((e) => { console.log("[test] FAIL harness: " + e + "\n" + (e && e.stack)); done(); });

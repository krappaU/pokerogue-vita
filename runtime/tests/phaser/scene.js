// SPDX-License-Identifier: AGPL-3.0-only
// Checks the native render fast paths (js/fastpath.js, src/fastpath.c) against Phaser's own code on
// a still scene built from what PokeRogue draws: containers (nested, scrolled, with alpha), images
// and sprites from an atlas (flips, tints, rotation, per-corner alpha), text, nine-slices, Graphics,
// rectangles, geometry masks, and a sprite pipeline that sets uniforms per object (as PokeRogue's
// sprite pipelines do).
// It runs the repo's verification probes (scripts/probe_verify_*.js, copied next to this page by the
// runner), which alternate native and Phaser code frame by frame and compare vertex data, uniform
// calls, draw calls and texture binds. Logs "[test] PASS/FAIL" lines and "[test] DONE ...".
import * as Phaser from "./phaser.js";

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

    this.cameras.main.setScroll(4, 2);
    this.frames = 0;
    this.game.events.on("postrender", () => this.frames++);
  }
}

const N = globalThis.__native;
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
  report("phaser: native pipeline machinery and shapes active",
    !!(rt.pipelineSwaps && rt.graphicsSwap && rt.nineSliceSwap && rt.shapesSwap),
    ["pipelineSwaps", "graphicsSwap", "nineSliceSwap", "shapesSwap"].filter((k) => !rt[k]).join(" "));
  for (const which of ["pipeline", "graphics", "nineslice", "shapes"]) {
    globalThis.__vfpWhich = which;
    await probe(game, "probe_verify_pipeline.js", "__vfp", which);
  }
  done();
}

run().catch((e) => { console.log("[test] FAIL harness: " + e + "\n" + (e && e.stack)); done(); });

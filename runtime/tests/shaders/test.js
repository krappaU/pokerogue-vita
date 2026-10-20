// SPDX-License-Identifier: AGPL-3.0-only
// js/webgl.js rewrites PokeRogue's and Phaser's fragment shaders for the Vita's shader compiler
// (rewriteShader). This page renders each shader as the game ships it and as rewritten, with the
// uniform settings the game uses (variants, fusion colours, tera, day/night, terrain, tone, shadow),
// and compares the pixels byte for byte. Shaders: PokeRogue's sprite and field shaders
// (third_party/test/pokerogue, tests/fetch_test_deps.sh) and Phaser's multi-texture shader, all with
// 4 texture units as the runtime gives Phaser. Logs "[test] PASS/FAIL" lines and "[test] DONE ...".
import * as Phaser from "./phaser.js";

const rt = globalThis.__rt;
const results = [];
function report(name, ok, detail) {
  results.push(ok);
  console.log("[test] " + (ok ? "PASS " : "FAIL ") + name + (detail ? ": " + detail : ""));
}
function done() {
  const failed = results.filter((r) => !r).length;
  console.log("[test] DONE passed=" + (results.length - failed) + " failed=" + failed);
}

const W = 96, H = 32, CELL = 16, UNITS = 4;
const canvas = document.createElement("canvas");
canvas.width = W, canvas.height = H;
const gl = canvas.getContext("webgl");

// 4 textures, 8x8: a fixed palette (the colours the variant tests swap) and pseudo-random texels,
// some transparent or half transparent.
const PALETTE = [[200, 40, 40], [40, 200, 40], [40, 40, 200], [220, 220, 40]];
let seed = 12345;
const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
const textures = [];
for (let t = 0; t < UNITS; t++) {
  const px = new Uint8Array(8 * 8 * 4);
  for (let i = 0; i < 64; i++) {
    const c = (i + t) % 3 === 0 ? PALETTE[(i + t) % 4] : [rnd() * 256 | 0, rnd() * 256 | 0, rnd() * 256 | 0];
    px.set([c[0], c[1], c[2], i % 11 === 0 ? 0 : i % 7 === 0 ? 128 : 255], i * 4);
  }
  const tex = gl.createTexture();
  gl.activeTexture(gl.TEXTURE0 + t);
  gl.bindTexture(gl.TEXTURE_2D, tex);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 8, 8, 0, gl.RGBA, gl.UNSIGNED_BYTE, px);
  for (const [k, v] of [[gl.TEXTURE_MIN_FILTER, gl.NEAREST], [gl.TEXTURE_MAG_FILTER, gl.NEAREST],
    [gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE], [gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE]]) gl.texParameteri(gl.TEXTURE_2D, k, v);
  textures.push(tex);
}

// The target: an off-screen framebuffer (the screen's is scaled by the runtime).
const target = gl.createTexture();
gl.activeTexture(gl.TEXTURE0 + UNITS);
gl.bindTexture(gl.TEXTURE_2D, target);
gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, W, H, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
const fb = gl.createFramebuffer();
gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, target, 0);

// 12 quads of 16x16: texture unit = i % 4, tint effect 0, 1, 2 per row of four; tints per corner.
const STRIDE = 10; // inPosition 2, inTexCoord 2, inTexId 1, inTintEffect 1, inTint 4
const verts = [];
for (let i = 0; i < 12; i++) {
  const x = (i % 6) * CELL, y = Math.floor(i / 6) * CELL, unit = i % 4, effect = Math.floor(i / 4);
  const corner = (cx, cy, k) => [x + cx * CELL, y + cy * CELL, cx, cy, unit, effect,
    0.2 + 0.2 * k, 1 - 0.15 * k, 0.5 + 0.1 * (i % 5), k === 3 ? 0.5 : 1];
  const [a, b, c, d] = [corner(0, 0, 0), corner(1, 0, 1), corner(0, 1, 2), corner(1, 1, 3)];
  verts.push(...a, ...b, ...c, ...b, ...d, ...c);
}
const vbuf = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, vbuf);
gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(verts), gl.STATIC_DRAW);
// The same quads with tint effects 0.99 and 1.99: what the Vita's GPU made of 1 and 2 (the stats
// graph showed a texture inside a solid-colour polygon).
const roughBuf = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, roughBuf);
gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(verts.map((v, i) => i % STRIDE === 5 && v > 0 ? v - 0.01 : v)), gl.STATIC_DRAW);

function compile(vsrc, fsrc, fix) {
  rt.shaderFix = fix;
  const shader = (type, src) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error("compile: " + gl.getShaderInfoLog(s));
    return s;
  };
  const fs = shader(gl.FRAGMENT_SHADER, fsrc);
  const p = gl.createProgram();
  gl.attachShader(p, shader(gl.VERTEX_SHADER, vsrc));
  gl.attachShader(p, fs);
  gl.linkProgram(p);
  rt.shaderFix = true;
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error("link: " + gl.getProgramInfoLog(p));
  p.fragSource = gl.getShaderSource(fs); // what the GL got, after js/webgl.js's changes
  return p;
}

const SPRITE_DEFAULTS = {
  time: ["1f", 0.1], dayTint: ["3f", [0.6, 0.5, 0.4]], duskTint: ["3f", [0.9, 0.4, 0.3]],
  nightTint: ["3f", [0.3, 0.3, 0.6]], fieldScale: ["1f", 6], texFrameUv: ["2f", [0, 0]], size: ["2f", [16, 16]],
  texSize: ["2f", [8, 8]], relPosition: ["2f", [8, 8]], vCutoff: ["1f", 1], isOutside: ["1i", 1],
};
function render(p, uniforms, buf = vbuf) {
  gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
  gl.viewport(0, 0, W, H);
  gl.disable(gl.BLEND);
  gl.clearColor(0, 0, 0, 0);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.useProgram(p);
  for (let t = 0; t < UNITS; t++) {
    gl.activeTexture(gl.TEXTURE0 + t);
    gl.bindTexture(gl.TEXTURE_2D, textures[t]);
  }
  const set = (name, type, v) => {
    const loc = gl.getUniformLocation(p, name);
    if (!loc) return;
    if (type === "1f") gl.uniform1f(loc, v);
    else if (type === "1i") gl.uniform1i(loc, v);
    else if (type === "2f") gl.uniform2fv(loc, v);
    else if (type === "3f") gl.uniform3fv(loc, v);
    else if (type === "4f") gl.uniform4fv(loc, v);
    else if (type === "4fv") gl.uniform4fv(loc, new Float32Array(v));
    else if (type === "4iv") gl.uniform4iv(loc, new Int32Array(v));
  };
  gl.uniform1iv(gl.getUniformLocation(p, "uMainSampler"), [0, 1, 2, 3]);
  gl.uniformMatrix4fv(gl.getUniformLocation(p, "uProjectionMatrix"), false,
    new Float32Array([2 / W, 0, 0, 0, 0, 2 / H, 0, 0, 0, 0, 0, 0, -1, -1, 0, 1]));
  set("uResolution", "2f", [W, H]);
  set("uRoundPixels", "1i", 0);
  // Every uniform the shaders have, zeroed first (a program keeps its values between cases).
  for (const [n, t] of [["time", "1f"], ["ignoreTimeTint", "1i"], ["isOutside", "1i"], ["teraTime", "1f"],
    ["hasShadow", "1i"], ["yCenter", "1i"], ["fieldScale", "1f"], ["vCutoff", "1f"], ["yOffset", "1f"],
    ["yShadowOffset", "1f"], ["terrainColorRatio", "1f"]]) set(n, t, 0);
  for (const n of ["overrideTint", "dayTint", "duskTint", "nightTint", "teraColor", "terrainColor"]) set(n, "3f", [0, 0, 0]);
  for (const n of ["relPosition", "texFrameUv", "size", "texSize"]) set(n, "2f", [0, 0]);
  set("tone", "4f", [0, 0, 0, 0]);
  for (const n of ["baseVariantColors", "variantColors", "spriteColors"]) set(n, "4fv", new Array(128).fill(0));
  set("fusionSpriteColors", "4iv", new Array(128).fill(0));
  for (const [n, [t, v]] of Object.entries(uniforms)) set(n, t, v);

  gl.bindBuffer(gl.ARRAY_BUFFER, buf);
  const attrs = [["inPosition", 2, 0], ["inTexCoord", 2, 2], ["inTexId", 1, 4], ["inTintEffect", 1, 5], ["inTint", 4, 6]];
  for (const [name, size, off] of attrs) {
    const loc = gl.getAttribLocation(p, name);
    if (loc < 0) continue;
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, size, gl.FLOAT, false, STRIDE * 4, off * 4);
  }
  gl.drawArrays(gl.TRIANGLES, 0, verts.length / STRIDE);
  const out = new Uint8Array(W * H * 4);
  gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, out);
  for (const [name] of attrs) {
    const loc = gl.getAttribLocation(p, name);
    if (loc >= 0) gl.disableVertexAttribArray(loc);
  }
  return out;
}
function diff(a, b) {
  let n = 0, max = 0;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) n++, max = Math.max(max, Math.abs(a[i] - b[i]));
  return { n, max };
}

const c = (rgb) => rgb.map((v) => v / 255);
const variants = {
  baseVariantColors: ["4fv", [...c(PALETTE[0]), 1, ...c(PALETTE[1]), 1, ...new Array(120).fill(0)]],
  variantColors: ["4fv", [0.1, 0.9, 0.9, 1, 0.9, 0.1, 0.5, 1, ...new Array(120).fill(0)]],
  spriteColors: ["4fv", [...c(PALETTE[2]), 1, ...c(PALETTE[3]), 1, ...new Array(120).fill(0)]],
  fusionSpriteColors: ["4iv", [255, 128, 0, 255, 10, 20, 30, 255, ...new Array(120).fill(0)]],
};
const CASES = {
  sprite: {
    plain: {},
    variants,
    tera: { teraColor: ["3f", [0.8, 0.3, 0.5]], teraTime: ["1f", 0.37] },
    "tera, variants": { ...variants, teraColor: ["3f", [1, 1, 1]], teraTime: ["1f", 0.6] },
    "dusk, inside": { time: ["1f", 0.3], isOutside: ["1i", 0] },
    "night, outside": { time: ["1f", 0.6] },
    "late, outside": { time: ["1f", 0.9] },
    "override tint": { overrideTint: ["3f", [0.2, 0.9, 0.4]] },
    "no time tint": { ignoreTimeTint: ["1i", 1] },
    tone: { tone: ["4f", [10, 20, 30, 0.5]] },
    shadow: { hasShadow: ["1i", 1], vCutoff: ["1f", 0.5], yCenter: ["1i", 1] },
    "shadow, bottom": { hasShadow: ["1i", 1], vCutoff: ["1f", 0.75], relPosition: ["2f", [8, 0]] },
  },
  field: {
    plain: {},
    "dusk, inside": { time: ["1f", 0.4], isOutside: ["1i", 0] },
    "night, outside": { time: ["1f", 0.8] },
    "override tint": { overrideTint: ["3f", [0.2, 0.9, 0.4]] },
    "no time tint": { ignoreTimeTint: ["1i", 1] },
    terrain: { terrainColorRatio: ["1f", 0.5], terrainColor: ["3f", [0.3, 0.7, 0.2]] },
    "terrain, no time tint": { terrainColorRatio: ["1f", 1], terrainColor: ["3f", [0.9, 0.2, 0.6]], ignoreTimeTint: ["1i", 1] },
  },
  multi: { plain: {} },
};

// Phaser's own shader sources aren't exported by its ES module build: they sit in phaser.js as
// ["#define SHADER_NAME <name>", "line", ...].join("\n").
async function phaserShader(name) {
  const js = await (await fetch("phaser.js")).text();
  const at = js.indexOf('["#define SHADER_NAME ' + name + '"');
  const end = js.indexOf('].join("\\n")', at);
  if (at < 0 || end < 0) throw new Error("no " + name + " in phaser.js");
  return JSON.parse(js.slice(at, end + 1)).join("\n");
}

async function run() {
  if (!rt || !rt.rewriteShader) throw new Error("js/webgl.js has no rewriteShader");
  const text = async (f) => (await fetch("pokerogue/" + f)).text();
  const parse = (src) => Phaser.Renderer.WebGL.Utils.parseFragmentShaderMaxTextures(src, UNITS);
  const spriteVert = await text("sprite-shader.vert");
  const shaders = {
    sprite: [spriteVert, parse(await text("sprite-frag-shader.frag")), ["tint effect", "colour loops (2)", "tera", "day/night"]],
    field: [spriteVert, parse(await text("field-sprite-frag-shader.frag")), ["tint effect", "texture pick (4)", "day/night", "terrain"]],
    multi: [await phaserShader("PHASER_MULTI_VS"), parse(await phaserShader("PHASER_MULTI_FS")), ["tint effect", "texture pick (4)"]],
  };
  const marks = { "tint effect": /pkr_tintEffect = floor\(outTintEffect \+ 0\.5\)/, "colour loops (2)": /\[0\]\.a != 0\.0|\[0\]\[3\] != 0\.0/, tera: /if \(all\(lessThan\(vec3\(0\.0\), teraColor\)\)\) \{ if/,
    "day/night": /if \(!ignoreTimeTint\) \{ if/, terrain: /if \(terrainColorRatio > 0\.0 &&[^{]*\) \{ if/, "texture pick (4)": /vec4 pkr_t3 = /};
  for (const [name, [vsrc, fsrc, expected]] of Object.entries(shaders)) {
    let orig, fixed;
    try {
      orig = compile(vsrc, fsrc, false);
      fixed = compile(vsrc, fsrc, true);
    } catch (e) {
      report(name + ": compiles", false, String(e && e.message || e));
      continue;
    }
    const missing = expected.filter((r) => !marks[r].test(fixed.fragSource) || marks[r].test(orig.fragSource));
    report(name + ": rewrites applied (" + expected.join(", ") + ")", !missing.length, missing.length ? "missing: " + missing.join(", ") : "");
    const exact = render(fixed, SPRITE_DEFAULTS), rough = render(fixed, SPRITE_DEFAULTS, roughBuf);
    const before = diff(render(orig, SPRITE_DEFAULTS), render(orig, SPRITE_DEFAULTS, roughBuf)).n;
    report(name + ": tint effects 0.99/1.99 draw as 1/2", !diff(exact, rough).n && before > 0,
      diff(exact, rough).n ? diff(exact, rough).n + " bytes differ" : before ? "" : "the original shader didn't mind either");
    let reference = null;
    for (const [label, uniforms] of Object.entries(CASES[name])) {
      const u = { ...SPRITE_DEFAULTS, ...uniforms };
      const a = render(orig, u), b = render(fixed, u), d = diff(a, b);
      const lit = a.some((v, i) => i % 4 === 3 && v !== 0);
      report(name + ", " + label + ": same pixels", d.n === 0 && lit,
        d.n ? d.n + " bytes differ (max " + d.max + ")" : lit ? "" : "nothing drawn");
      if (!reference) reference = a;
      else if (!diff(reference, a).n) report(name + ", " + label + ": the settings change the picture", false, "same as plain");
    }
  }
  done();
}
run().catch((e) => { report("harness", false, e + "\n" + (e && e.stack)); done(); });

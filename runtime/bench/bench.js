// SPDX-License-Identifier: AGPL-3.0-only
// Shader benchmark for the dev build (config.txt bench=shaders, instead of the game): draws the full
// 960x544 screen with each of the game's expensive fragment shaders, as shipped, as rewritten by
// js/webgl.js, and with one part removed at a time, and logs the GPU time per full screen. The
// variants are for measuring only (the removed parts change the picture). When done, the log goes
// to the log server by itself (devtools.c, marked "shader benchmark").
// Shaders: PokeRogue's sprite and field shaders (pokerogue/, scripts/fetch_pokerogue_shaders.sh)
// and Phaser 3.90's multi-texture shader (MIT, copied below), with 4 texture units as in the game.

const N = globalThis.__native, rt = globalThis.__rt;
const W = 960, H = 544, UNITS = 4, RUNS = 12;
const log = (s) => console.log("[bench] " + s);
const nextFrame = () => new Promise((r) => requestAnimationFrame(r));

const PHASER_MULTI_VS = `precision mediump float;
uniform mat4 uProjectionMatrix;
uniform vec2 uResolution;
attribute vec2 inPosition;
attribute vec2 inTexCoord;
attribute float inTexId;
attribute float inTintEffect;
attribute vec4 inTint;
varying vec2 outTexCoord;
varying float outTexId;
varying float outTintEffect;
varying vec4 outTint;
void main ()
{
    gl_Position = uProjectionMatrix * vec4(inPosition, 1.0, 1.0);
    outTexCoord = inTexCoord;
    outTexId = inTexId;
    outTint = inTint;
    outTintEffect = inTintEffect;
}`;
const PHASER_MULTI_FS = `#define SHADER_NAME PHASER_MULTI_FS
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uMainSampler[%count%];
varying vec2 outTexCoord;
varying float outTexId;
varying float outTintEffect;
varying vec4 outTint;
void main ()
{
    vec4 texture;
    %forloop%
    vec4 texel = vec4(outTint.bgr * outTint.a, outTint.a);
    vec4 color = texture * texel;
    if (outTintEffect == 1.0)
    {
        color.rgb = mix(texture.rgb, outTint.bgr * outTint.a, texture.a);
    }
    else if (outTintEffect == 2.0)
    {
        color = texel;
    }
    gl_FragColor = color;
}`;
// Phaser's WebGL Utils.parseFragmentShaderMaxTextures.
function parse(src) {
  let loop = "";
  for (let i = 0; i < UNITS; i++) {
    if (i > 0) loop += "\n\telse ";
    if (i < UNITS - 1) loop += "if (outTexId < " + i + ".5)";
    loop += "\n\t{\n\t\ttexture = texture2D(uMainSampler[" + i + "], outTexCoord);\n\t}";
  }
  return src.replace(/%count%/gi, String(UNITS)).replace(/%forloop%/gi, loop);
}

// ---------------------------------------------------------------- variants (text surgery)

function blockEnd(src, open) {
  for (let i = open, depth = 0; i < src.length; i++) {
    if (src[i] === "{") depth++;
    else if (src[i] === "}" && --depth === 0) return i + 1;
  }
  return -1;
}
// Removes every statement that starts where re matches and runs to its block's end, with the
// else / else if blocks that follow it.
function remove(re) {
  return (src) => {
    const g = new RegExp(re.source, "g");
    let m, out = src, n = 0;
    while ((m = g.exec(out))) {
      let end = blockEnd(out, out.indexOf("{", m.index));
      for (let e; end > 0 && (e = /^\s*else\b/.exec(out.slice(end)));) end = blockEnd(out, out.indexOf("{", end + e[0].length));
      if (end < 0) throw new Error("unbalanced braces after " + re);
      out = out.slice(0, m.index) + out.slice(end);
      g.lastIndex = m.index;
      n++;
    }
    if (!n) throw new Error("not in this shader: " + re);
    return out;
  };
}
const replace = (re, to) => (src) => {
  if (!re.test(src)) throw new Error("not in this shader: " + re);
  return src.replace(new RegExp(re.source, "g"), to);
};
const all = (...fs) => (src) => fs.reduce((s, f) => f(s), src);
const ONE_TEXTURE = replace(/if \(outTexId < 0\.5\)[\s\S]*?texture = texture2D\(uMainSampler\[3\], outTexCoord\);\s*\}/,
  "texture = texture2D(uMainSampler[0], outTexCoord);");
const NO_DAYNIGHT = remove(/if \(color\.a > 0\.0 && !ignoreTimeTint\)/);
const NO_TINT_MODES = remove(/if \(outTintEffect == 1\.0\)/);
const MEDIUMP = replace(/precision highp float;/, "precision mediump float;");
const MAIN_ONLY = (body) => (src) => src.replace(/void main\s*\(\s*\)\s*\{[\s\S]*$/, "void main() { " + body + " }");
const TEXTURE_ONLY = MAIN_ONLY("gl_FragColor = texture2D(uMainSampler[0], outTexCoord);");
const CONSTANT = MAIN_ONLY("gl_FragColor = vec4(1.0, 0.5, 0.25, 1.0);");

const FIELD = [
  ["as shipped", null, false],
  ["rewritten + variants (shaderfix)", null, true],
  ["one texture (no if-chain)", ONE_TEXTURE],
  ["no day/night tint", NO_DAYNIGHT],
  ["no terrain", remove(/if \(\s*terrainColorRatio > 0\.0/)],
  ["no tint modes", NO_TINT_MODES],
  ["mediump floats", MEDIUMP],
  ["texture x tint only (all of the above)", all(ONE_TEXTURE, NO_DAYNIGHT, remove(/if \(\s*terrainColorRatio > 0\.0/), NO_TINT_MODES)],
];
const NO_LOOPS = remove(/for \(int i = 0; i < 32; i\+\+\)/);
const NO_TERA = remove(/if \(color\.a > 0\.0 && all\(lessThan\(vec3\(0\.0\), teraColor\)\)\)/);
const NO_SHADOW = remove(/if \(hasShadow\)/);
const NO_TONE = all(replace(/float luma = dot\(color\.rgb, lumaF\);/, ""), replace(/color\.rgb = mix\(color\.rgb, vec3\(luma\), tone\.w\);/, ""),
  replace(/color\.rgb \+= tone\.rgb \* \(color\.a \/ 255\.0\);/, ""));
const SPRITE = [
  ["as shipped", null, false],
  ["rewritten + variants (shaderfix)", null, true],
  ["no colour loops", NO_LOOPS],
  ["no tera", NO_TERA],
  ["no day/night tint", NO_DAYNIGHT],
  ["no shadow", NO_SHADOW],
  ["no grey/tone", NO_TONE],
  ["no tint modes", NO_TINT_MODES],
  ["mediump floats", MEDIUMP],
  ["texture x tint only (all of the above)", all(NO_LOOPS, NO_TERA, NO_DAYNIGHT, NO_SHADOW, NO_TONE, NO_TINT_MODES)],
];
const MULTI = [
  ["as shipped", null, false],
  ["rewritten (shaderfix)", null, true],
  ["one texture (no if-chain)", ONE_TEXTURE],
  ["no tint modes", NO_TINT_MODES],
  ["mediump floats", MEDIUMP],
];
const FLOOR = [["texture only", TEXTURE_ONLY], ["constant colour, no texture", CONSTANT]];

// ---------------------------------------------------------------- drawing

const canvas = document.createElement("canvas");
canvas.width = W, canvas.height = H;
const gl = canvas.getContext("webgl");

// 4 textures of 256x256 in 4x4 blocks (pixel art magnified ~3.75x on screen, like the arena), some
// blocks transparent.
let seed = 4321;
const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
for (let t = 0; t < UNITS; t++) {
  const px = new Uint8Array(256 * 256 * 4);
  for (let by = 0; by < 64; by++) {
    for (let bx = 0; bx < 64; bx++) {
      const c = [rnd() * 256 | 0, rnd() * 256 | 0, rnd() * 256 | 0, rnd() < 0.3 ? 0 : 255];
      for (let y = 0; y < 4; y++) for (let x = 0; x < 4; x++) px.set(c, ((by * 4 + y) * 256 + bx * 4 + x) * 4);
    }
  }
  gl.activeTexture(gl.TEXTURE0 + t);
  gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 256, 256, 0, gl.RGBA, gl.UNSIGNED_BYTE, px);
  for (const [k, v] of [[gl.TEXTURE_MIN_FILTER, gl.NEAREST], [gl.TEXTURE_MAG_FILTER, gl.NEAREST],
    [gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE], [gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE]]) gl.texParameteri(gl.TEXTURE_2D, k, v);
}
// One full-screen quad: texture unit 0, tint effect 0, white tint (the arena backdrop in battle).
const vbuf = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, vbuf);
const corner = (x, y) => [x * W, y * H, x, y, 0, 0, 1, 1, 1, 1];
gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([...corner(0, 0), ...corner(1, 0), ...corner(0, 1),
  ...corner(1, 0), ...corner(1, 1), ...corner(0, 1)]), gl.STATIC_DRAW);

function compile(vsrc, fsrc, fix) {
  rt.shaderFix = fix;
  try {
    const shader = (type, src) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      return s;
    };
    const p = gl.createProgram();
    gl.attachShader(p, shader(gl.VERTEX_SHADER, vsrc));
    gl.attachShader(p, shader(gl.FRAGMENT_SHADER, fsrc));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error("doesn't link: " + gl.getProgramInfoLog(p));
    return p;
  } finally {
    rt.shaderFix = N.shaderFix !== false;
  }
}

// A battle outside at daytime with an ordinary Pokémon: day tint, no palettes, no tera, shadow on.
const UNIFORMS = [
  ["time", "1f", 0.1], ["isOutside", "1i", 1], ["ignoreTimeTint", "1i", 0], ["dayTint", "3f", [0.6, 0.5, 0.4]],
  ["duskTint", "3f", [0.9, 0.4, 0.3]], ["nightTint", "3f", [0.3, 0.3, 0.6]], ["overrideTint", "3f", [0, 0, 0]],
  ["teraColor", "3f", [0, 0, 0]], ["teraTime", "1f", 0], ["hasShadow", "1i", 1], ["yCenter", "1i", 0],
  ["fieldScale", "1f", 6], ["vCutoff", "1f", 1], ["relPosition", "2f", [480, 400]], ["size", "2f", [256, 256]],
  ["texSize", "2f", [256, 256]], ["texFrameUv", "2f", [0, 0]], ["yOffset", "1f", 0], ["yShadowOffset", "1f", 0],
  ["tone", "4f", [0, 0, 0, 0]], ["terrainColorRatio", "1f", 0], ["terrainColor", "3f", [0, 0, 0]],
  ["baseVariantColors", "4fv", new Array(128).fill(0)], ["variantColors", "4fv", new Array(128).fill(0)],
  ["spriteColors", "4fv", new Array(128).fill(0)], ["fusionSpriteColors", "4iv", new Array(128).fill(0)],
];
function setup(p) {
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.viewport(0, 0, W, H);
  gl.enable(gl.BLEND);
  gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA); // Phaser's normal blend mode
  gl.useProgram(p);
  const loc = (n) => gl.getUniformLocation(p, n);
  if (loc("uMainSampler")) gl.uniform1iv(loc("uMainSampler"), [0, 1, 2, 3]);
  gl.uniformMatrix4fv(loc("uProjectionMatrix"), false, new Float32Array([2 / W, 0, 0, 0, 0, -2 / H, 0, 0, 0, 0, 0, 0, -1, 1, 0, 1]));
  if (loc("uResolution")) gl.uniform2fv(loc("uResolution"), [W, H]);
  if (loc("uRoundPixels")) gl.uniform1i(loc("uRoundPixels"), 0);
  for (const [n, t, v] of UNIFORMS) {
    const l = loc(n);
    if (!l) continue;
    if (t === "1f") gl.uniform1f(l, v);
    else if (t === "1i") gl.uniform1i(l, v);
    else if (t === "2f") gl.uniform2fv(l, v);
    else if (t === "3f") gl.uniform3fv(l, v);
    else if (t === "4f") gl.uniform4fv(l, v);
    else if (t === "4fv") gl.uniform4fv(l, new Float32Array(v));
    else gl.uniform4iv(l, new Int32Array(v));
  }
  gl.bindBuffer(gl.ARRAY_BUFFER, vbuf);
  for (const [name, size, off] of [["inPosition", 2, 0], ["inTexCoord", 2, 2], ["inTexId", 1, 4], ["inTintEffect", 1, 5], ["inTint", 4, 6]]) {
    const l = gl.getAttribLocation(p, name);
    if (l < 0) continue;
    gl.enableVertexAttribArray(l);
    gl.vertexAttribPointer(l, size, gl.FLOAT, false, 40, off * 4);
  }
}
// ms of GPU time for one full-screen draw: each draw is waited for alone (glFinish before and after),
// so the time includes the wait's own scene overhead (the "empty" line).
function timeDraws(draw) {
  for (let i = 0; i < 3; i++) draw();
  const t = [];
  for (let i = 0; i < RUNS; i++) {
    gl.finish();
    const t0 = performance.now();
    draw();
    gl.finish();
    t.push(performance.now() - t0);
  }
  t.sort((a, b) => a - b);
  return { median: t[RUNS >> 1], min: t[0], max: t[RUNS - 1] };
}
const fmt = (r) => r.median.toFixed(1) + " ms (min " + r.min.toFixed(1) + ", max " + r.max.toFixed(1) + ")";

const show = (text) => N.showMessage && N.showMessage("Shader benchmark\n\n" + text);

async function bench(group, vsrc, fsrc, variants) {
  const results = [];
  for (const [i, [label, change, fix]] of variants.entries()) {
    show(group + ", " + (i + 1) + " of " + variants.length + ":\n" + label);
    await nextFrame();
    let p;
    try {
      p = compile(vsrc, change ? change(fsrc) : fsrc, !!fix);
    } catch (e) {
      log(group + " | " + label + ": skipped (" + (e && e.message || e) + ")");
      continue;
    }
    setup(p);
    const r = timeDraws(() => gl.drawArrays(gl.TRIANGLES, 0, 6));
    const err = gl.getError();
    log(group + " | " + label + ": " + fmt(r) + (err ? " (GL error " + err + ")" : ""));
    results.push([label, r.median]);
    gl.deleteProgram(p);
  }
  return results;
}

async function run() {
  log("start: one full screen (" + W + "x" + H + ") per draw, alpha blending on, median of " + RUNS);
  await nextFrame();
  const empty = timeDraws(() => {});
  log("empty (no draw, the wait's own overhead): " + fmt(empty));
  const text = async (f) => (await fetch("pokerogue/" + f)).text();
  const spriteVert = await text("sprite-shader.vert");
  const summary = [];
  for (const [group, vsrc, fsrc, variants] of [
    ["field", spriteVert, parse(await text("field-sprite-frag-shader.frag")), FIELD],
    ["sprite", spriteVert, parse(await text("sprite-frag-shader.frag")), SPRITE],
    ["phaser multi", PHASER_MULTI_VS, parse(PHASER_MULTI_FS), MULTI],
    ["floor", PHASER_MULTI_VS, parse(PHASER_MULTI_FS), FLOOR],
  ]) {
    const r = await bench(group, vsrc, fsrc, variants);
    summary.push(group + ": " + r.map(([l, ms]) => l + " " + ms.toFixed(1)).join(", "));
  }
  for (const s of summary) log("summary " + s);
  log("done");
  finish("Done. The log goes to the log server (devserver= in config.txt).");
}
function finish(text) {
  show(text + "\n\nRemove bench=shaders from config.txt to play again.\nSTART + SELECT quits.");
  if (N.devSendBenchLog) N.devSendBenchLog();
}
run().catch((e) => {
  log("failed: " + e + "\n" + (e && e.stack));
  finish("Failed: " + e);
});

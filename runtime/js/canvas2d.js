// SPDX-License-Identifier: AGPL-3.0-only
// CanvasRenderingContext2D on the native plutovg surface (src/canvas2d.c). State strings
// (fillStyle, font, ...) are parsed here; the native side gets plain numbers.
// Shadows (Phaser uses them for text drop shadows) are drawn as an offset copy without blur.
(function (g) {
  "use strict";
  const N = g.__native;

  // ---------------------------------------------------------------- colors
  const NAMED = {
    black: "#000000", white: "#ffffff", red: "#ff0000", green: "#008000", blue: "#0000ff", yellow: "#ffff00",
    cyan: "#00ffff", aqua: "#00ffff", magenta: "#ff00ff", fuchsia: "#ff00ff", gray: "#808080", grey: "#808080",
    silver: "#c0c0c0", maroon: "#800000", olive: "#808000", lime: "#00ff00", navy: "#000080", purple: "#800080",
    teal: "#008080", orange: "#ffa500", pink: "#ffc0cb", brown: "#a52a2a", gold: "#ffd700",
    darkgray: "#a9a9a9", darkgrey: "#a9a9a9", lightgray: "#d3d3d3", lightgrey: "#d3d3d3",
  };
  const colorCache = new Map();
  function parseColor(c) {
    if (typeof c !== "string") return c && c._color ? c._color : [0, 0, 0, 1];
    let v = colorCache.get(c);
    if (v) return v;
    let s = c.trim().toLowerCase();
    if (s === "transparent") v = [0, 0, 0, 0];
    else {
      if (NAMED[s]) s = NAMED[s];
      let m;
      if (s[0] === "#") {
        let h = s.slice(1);
        if (h.length === 3 || h.length === 4) h = [...h].map((x) => x + x).join("");
        const n = parseInt(h, 16);
        v = h.length === 8 ? [(n >>> 24) / 255, ((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255]
          : [(n >> 16) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255, 1];
      } else if ((m = /^rgba?\(\s*([\d.]+)%?[\s,]+([\d.]+)%?[\s,]+([\d.]+)%?(?:[\s,/]+([\d.]+)(%?))?\s*\)$/.exec(s))) {
        v = [m[1] / 255, m[2] / 255, m[3] / 255, m[4] === undefined ? 1 : m[5] ? m[4] / 100 : +m[4]];
      } else if ((m = /^hsla?\(\s*([\d.]+)[\s,]+([\d.]+)%[\s,]+([\d.]+)%(?:[\s,/]+([\d.]+))?\s*\)$/.exec(s))) {
        const h = m[1] / 360, sat = m[2] / 100, l = m[3] / 100;
        const q = l < 0.5 ? l * (1 + sat) : l + sat - l * sat, p = 2 * l - q;
        const f = (t) => { t = (t + 1) % 1; return t < 1 / 6 ? p + (q - p) * 6 * t : t < 0.5 ? q : t < 2 / 3 ? p + (q - p) * (2 / 3 - t) * 6 : p; };
        v = [f(h + 1 / 3), f(h), f(h - 1 / 3), m[4] === undefined ? 1 : +m[4]];
      } else {
        v = [0, 0, 0, 1];
      }
    }
    if (colorCache.size > 512) colorCache.clear();
    colorCache.set(c, v);
    return v;
  }

  // Gradients and patterns are approximated by a flat color (the first stop).
  class CanvasGradient { constructor() { this._color = null; } addColorStop(o, c) { if (!this._color) this._color = parseColor(c); } }
  class CanvasPattern { constructor() { this._color = [0, 0, 0, 0]; } setTransform() {} }

  // "italic bold 16px/1.2 'emerald', sans-serif". Results are cached: Phaser sets the same font
  // strings over and over (parsing was 8% of the game's UI setup). Treat results as read-only.
  const fontCache = new Map();
  function parseFont(f) {
    let r = fontCache.get(f);
    if (r === undefined) {
      r = parseFontUncached(f);
      if (fontCache.size > 500) fontCache.clear();
      fontCache.set(f, r);
    }
    return r;
  }
  function parseFontUncached(f) {
    const m = /^\s*(.*?)\s*([\d.]+)(px|pt|em|rem)(?:\s*\/\s*[\w.%]+)?\s+(.+)$/.exec(f);
    if (!m) return null;
    const pre = m[1].toLowerCase();
    let size = +m[2];
    if (m[3] === "pt") size *= 4 / 3;
    else if (m[3] === "em" || m[3] === "rem") size *= 16;
    const families = m[4].split(",").map((x) => x.trim().replace(/^["']|["']$/g, ""));
    return { size, families, bold: /bold|[6-9]00/.test(pre), italic: /italic|oblique/.test(pre) };
  }

  const COMPOSITE = { "clear": 0, "copy": 1, "destination": 2, "source-over": 3, "destination-over": 4, "source-in": 5,
    "destination-in": 6, "source-out": 7, "destination-out": 8, "source-atop": 9, "destination-atop": 10, "xor": 11 };
  const CAPS = { butt: 0, round: 1, square: 2 };
  const JOINS = { miter: 0, round: 1, bevel: 2 };

  class ImageData {
    constructor(a, b, c) {
      if (a instanceof Uint8ClampedArray) { this.data = a; this.width = b; this.height = c ?? a.length / 4 / b; }
      else { this.width = a; this.height = b; this.data = new Uint8ClampedArray(a * b * 4); }
      this.colorSpace = "srgb";
    }
  }

  class TextMetrics {
    constructor(w, asc, desc, left, right) {
      this.width = w;
      this.actualBoundingBoxAscent = asc;
      this.actualBoundingBoxDescent = desc;
      this.actualBoundingBoxLeft = left;
      this.actualBoundingBoxRight = right;
      this.fontBoundingBoxAscent = asc;
      this.fontBoundingBoxDescent = desc;
      this.emHeightAscent = asc;
      this.emHeightDescent = desc;
      this.alphabeticBaseline = 0;
      this.hangingBaseline = asc * 0.8;
      this.ideographicBaseline = -desc;
    }
  }

  const DEFAULTS = {
    fillStyle: "#000000", strokeStyle: "#000000", globalAlpha: 1, globalCompositeOperation: "source-over",
    lineWidth: 1, lineCap: "butt", lineJoin: "miter", miterLimit: 10, font: "10px sans-serif",
    textAlign: "start", textBaseline: "alphabetic", direction: "inherit", letterSpacing: "0px",
    shadowColor: "rgba(0, 0, 0, 0)", shadowBlur: 0, shadowOffsetX: 0, shadowOffsetY: 0,
    imageSmoothingEnabled: true, imageSmoothingQuality: "low", filter: "none", lineDashOffset: 0,
  };

  class CanvasRenderingContext2D {
    constructor(canvas, attrs) {
      this.canvas = canvas;
      this._n = canvas._native;
      this._s = { ...DEFAULTS, dash: [] };
      this._stack = [];
      this._fontInfo = null;
      this._applyFont();
    }
    getContextAttributes() { return { alpha: true, desynchronized: false, colorSpace: "srgb", willReadFrequently: false }; }
    _resized() {
      // Resizing a canvas resets its context state.
      this._s = { ...DEFAULTS, dash: [] };
      this._stack = [];
      this._applyFont();
    }

    // -- state
    save() { this._stack.push({ ...this._s }); this._n.save(); }
    restore() {
      const s = this._stack.pop();
      if (!s) return;
      this._n.restore();
      const fontChanged = s.font !== this._s.font;
      this._s = s;
      if (fontChanged) this._applyFont();
    }
    reset() { this._resized(); this._n.setTransform(1, 0, 0, 1, 0, 0); this.clearRect(0, 0, this.canvas.width, this.canvas.height); }

    get fillStyle() { return this._s.fillStyle; }
    set fillStyle(v) { this._s.fillStyle = v; }
    get strokeStyle() { return this._s.strokeStyle; }
    set strokeStyle(v) { this._s.strokeStyle = v; }
    get globalAlpha() { return this._s.globalAlpha; }
    set globalAlpha(v) { v = +v; if (v >= 0 && v <= 1) { this._s.globalAlpha = v; this._n.setAlpha(v); } }
    get globalCompositeOperation() { return this._s.globalCompositeOperation; }
    set globalCompositeOperation(v) { if (v in COMPOSITE) { this._s.globalCompositeOperation = v; this._n.setComposite(COMPOSITE[v]); } }
    get lineWidth() { return this._s.lineWidth; }
    set lineWidth(v) { v = +v; if (v > 0) { this._s.lineWidth = v; this._n.setLineWidth(v); } }
    get lineCap() { return this._s.lineCap; }
    set lineCap(v) { if (v in CAPS) { this._s.lineCap = v; this._n.setLineCap(CAPS[v]); } }
    get lineJoin() { return this._s.lineJoin; }
    set lineJoin(v) { if (v in JOINS) { this._s.lineJoin = v; this._n.setLineJoin(JOINS[v]); } }
    get miterLimit() { return this._s.miterLimit; }
    set miterLimit(v) { this._s.miterLimit = +v; this._n.setMiterLimit(+v); }
    get font() { return this._s.font; }
    set font(v) {
      v = String(v);
      if (v === this._s.font) return; // already applied
      if (parseFont(v)) { this._s.font = v; this._applyFont(); }
    }
    get textAlign() { return this._s.textAlign; }
    set textAlign(v) { this._s.textAlign = v; }
    get textBaseline() { return this._s.textBaseline; }
    set textBaseline(v) { this._s.textBaseline = v; }
    get direction() { return this._s.direction; }
    set direction(v) { this._s.direction = v; }
    get letterSpacing() { return this._s.letterSpacing; }
    set letterSpacing(v) { this._s.letterSpacing = v; }
    get shadowColor() { return this._s.shadowColor; }
    set shadowColor(v) { this._s.shadowColor = v; }
    get shadowBlur() { return this._s.shadowBlur; }
    set shadowBlur(v) { this._s.shadowBlur = +v; }
    get shadowOffsetX() { return this._s.shadowOffsetX; }
    set shadowOffsetX(v) { this._s.shadowOffsetX = +v; }
    get shadowOffsetY() { return this._s.shadowOffsetY; }
    set shadowOffsetY(v) { this._s.shadowOffsetY = +v; }
    get imageSmoothingEnabled() { return this._s.imageSmoothingEnabled; }
    set imageSmoothingEnabled(v) { this._s.imageSmoothingEnabled = !!v; }
    get imageSmoothingQuality() { return this._s.imageSmoothingQuality; }
    set imageSmoothingQuality(v) { this._s.imageSmoothingQuality = v; }
    get filter() { return this._s.filter; }
    set filter(v) { this._s.filter = v; }
    get lineDashOffset() { return this._s.lineDashOffset; }
    set lineDashOffset(v) { this._s.lineDashOffset = +v; }
    get fontKerning() { return "auto"; }
    set fontKerning(v) {}
    setLineDash(d) { this._s.dash = [...d]; }
    getLineDash() { return [...this._s.dash]; }

    _applyFont() {
      const f = parseFont(this._s.font) || parseFont(DEFAULTS.font);
      this._fontInfo = f;
      for (const fam of f.families) if (this._n.setFont(fam, f.size, f.bold, f.italic)) return;
    }

    // -- transforms
    setTransform(a, b, c, d, e, f) {
      if (typeof a === "object" && a) ({ a, b, c, d, e, f } = a);
      if (a === undefined) [a, b, c, d, e, f] = [1, 0, 0, 1, 0, 0];
      this._n.setTransform(a, b, c, d, e, f);
    }
    resetTransform() { this._n.setTransform(1, 0, 0, 1, 0, 0); }
    getTransform() { const [a, b, c, d, e, f] = this._n.getTransform(); return { a, b, c, d, e, f, m11: a, m12: b, m21: c, m22: d, m41: e, m42: f, is2D: true, isIdentity: a === 1 && !b && !c && d === 1 && !e && !f }; }
    transform(a, b, c, d, e, f) { this._n.transform(a, b, c, d, e, f); }
    translate(x, y) { this._n.translate(x, y); }
    scale(x, y) { this._n.scale(x, y); }
    rotate(r) { this._n.rotate(r); }

    // -- drawing
    _fill() { const c = parseColor(this._s.fillStyle); this._n.setFill(c[0], c[1], c[2], c[3]); }
    _stroke() { const c = parseColor(this._s.strokeStyle); this._n.setStroke(c[0], c[1], c[2], c[3]); }
    _shadow() { const c = parseColor(this._s.shadowColor); return c[3] > 0 && (this._s.shadowOffsetX || this._s.shadowOffsetY || this._s.shadowBlur) ? c : null; }
    // Run draw() once offset with the shadow color, then normally.
    _withShadow(draw, isStroke) {
      const sc = this._shadow();
      if (sc) {
        this._n.save();
        // Shadow offsets are in device space, independent of the current transform.
        const [a, b, c, d, e, f] = this._n.getTransform();
        this._n.setTransform(a, b, c, d, e + this._s.shadowOffsetX, f + this._s.shadowOffsetY);
        if (isStroke) this._n.setStroke(sc[0], sc[1], sc[2], sc[3]); else this._n.setFill(sc[0], sc[1], sc[2], sc[3]);
        draw(true);
        this._n.restore();
      }
      if (isStroke) this._stroke(); else this._fill();
      draw(false);
    }
    clearRect(x, y, w, h) { this._n.clearRect(x, y, w, h); }
    fillRect(x, y, w, h) { this._withShadow(() => this._n.fillRect(x, y, w, h), false); }
    strokeRect(x, y, w, h) { this._withShadow(() => this._n.strokeRect(x, y, w, h), true); }
    beginPath() { this._n.beginPath(); }
    closePath() { this._n.closePath(); }
    moveTo(x, y) { this._n.moveTo(x, y); }
    lineTo(x, y) { this._n.lineTo(x, y); }
    quadraticCurveTo(a, b, c, d) { this._n.quadraticCurveTo(a, b, c, d); }
    bezierCurveTo(a, b, c, d, e, f) { this._n.bezierCurveTo(a, b, c, d, e, f); }
    rect(x, y, w, h) { this._n.rect(x, y, w, h); }
    roundRect(x, y, w, h) { this._n.rect(x, y, w, h); }
    arc(x, y, r, a0, a1, ccw) { this._n.arc(x, y, r, a0, a1, !!ccw); }
    arcTo(x1, y1, x2, y2) { this._n.lineTo(x1, y1); this._n.lineTo(x2, y2); }
    ellipse(x, y, rx, ry) { this._n.ellipse(x, y, rx, ry); }
    fill() { this._withShadow(() => this._n.fill(), false); }
    stroke() { this._withShadow(() => this._n.stroke(), true); }
    clip() { this._n.clip(); }
    isPointInPath() { return false; }
    isPointInStroke() { return false; }

    // -- text
    _textPos(text, x, y) {
      const [w, asc, desc] = this._n.measureText(text);
      const al = this._s.textAlign;
      if (al === "center") x -= w / 2;
      else if (al === "right" || al === "end") x -= w;
      const bl = this._s.textBaseline;
      if (bl === "top" || bl === "hanging") y += asc;
      else if (bl === "middle") y += (asc - desc) / 2;
      else if (bl === "bottom" || bl === "ideographic") y -= desc;
      return [x, y];
    }
    fillText(text, x, y) {
      if (this._n.measureOnly) return;
      text = String(text);
      const [tx, ty] = this._textPos(text, x, y);
      this._withShadow(() => this._n.fillText(text, tx, ty), false);
    }
    strokeText(text, x, y) {
      if (this._n.measureOnly) return;
      text = String(text);
      const [tx, ty] = this._textPos(text, x, y);
      this._withShadow(() => this._n.strokeText(text, tx, ty), true);
    }
    measureText(text) {
      const [w, asc, desc, left, right] = this._n.measureText(String(text));
      return new TextMetrics(w, asc, desc, left, right);
    }

    // -- images
    drawImage(img, a, b, c, d, e, f, gg, h) {
      const src = img && (img._native || (img.canvas && img.canvas._native));
      if (!src) return;
      const iw = img.naturalWidth || img.width, ih = img.naturalHeight || img.height;
      let sx = 0, sy = 0, sw = iw, sh = ih, dx, dy, dw, dh;
      if (c === undefined) { dx = a; dy = b; dw = iw; dh = ih; }
      else if (e === undefined) { dx = a; dy = b; dw = c; dh = d; }
      else { sx = a; sy = b; sw = c; sh = d; dx = e; dy = f; dw = gg; dh = h; }
      this._n.drawImage(src, sx, sy, sw, sh, dx, dy, dw, dh);
    }
    createImageData(w, h) { return w instanceof ImageData ? new ImageData(w.width, w.height) : new ImageData(Math.abs(w), Math.abs(h)); }
    getImageData(x, y, w, h) {
      try {
        return new ImageData(new Uint8ClampedArray(this._n.getImageData(x, y, w, h)), w, h);
      } catch (e) {
        // A large copy (Phaser's CanvasTexture keeps one: 7 MB for the stat change animation) that
        // found the heap full of garbage: collect it and try once more (log142's hang).
        if (!(e instanceof InternalError) || !/out of memory/.test(e.message)) throw e;
        N.log("[canvas] getImageData " + w + "x" + h + ": out of memory, collecting and retrying");
        N.gc();
        return new ImageData(new Uint8ClampedArray(this._n.getImageData(x, y, w, h)), w, h);
      }
    }
    putImageData(img, dx, dy) { this._n.putImageData(img.data, img.width, img.height, dx, dy); }
    createLinearGradient() { return new CanvasGradient(); }
    createRadialGradient() { return new CanvasGradient(); }
    createConicGradient() { return new CanvasGradient(); }
    createPattern() { return new CanvasPattern(); }
    drawFocusIfNeeded() {}
    scrollPathIntoView() {}
  }

  Object.assign(g, { CanvasRenderingContext2D, CanvasGradient, CanvasPattern, ImageData, TextMetrics });
  g.__rt.parseColor = parseColor;
})(globalThis);

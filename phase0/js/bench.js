// SPDX-License-Identifier: AGPL-3.0-only
// Phase 0 benchmark scene: a synthetic stand-in for a PokeRogue battle screen.
// PokeRogue renders a 1920x1080 canvas with a 320x180 logical layout scaled 6x, built from
// containers of small sprites (info boxes, HP bars, item icons, party icons), two animated
// Pokemon sprites with shadows, a few text labels and a typewriter message box.
//
// Scenarios (switched by the host via globalThis.__bench.start(name)):
//   idle    battle screen, nothing moving except the two Pokemon idle animations
//   battle  idle + tweens (HP bar drain, sprite bob), typewriter text, icon pulses
//   heavy   battle with ~3x the UI objects (busy menus / big item stacks)

(function (g) {
  "use strict";
  const W = 320, H = 180, S = 6;
  const bench = { ready: false, scenario: null, error: null };
  g.__bench = bench;

  function img(w, h) {
    const i = new Image();
    i.src = "bench://img#" + w + "x" + h;
    return i;
  }

  class BattleBench extends Phaser.Scene {
    constructor() { super({ key: "bench" }); }

    create() {
      const tex = this.textures;
      tex.addImage("bg", img(320, 180));
      tex.addImage("ui", img(512, 512));
      tex.addImage("items", img(512, 512));
      tex.addImage("mon", img(1024, 1024));
      const ui = tex.get("ui"), items = tex.get("items"), mon = tex.get("mon");
      for (let i = 0; i < 64; i++) ui.add("ui" + i, 0, (i % 8) * 64, Math.floor(i / 8) * 64, 48, 16);
      for (let i = 0; i < 256; i++) items.add("it" + i, 0, (i % 16) * 32, Math.floor(i / 16) * 32, 32, 32);
      for (let i = 0; i < 32; i++) mon.add("f" + i, 0, (i % 8) * 128, Math.floor(i / 8) * 128, 96, 96);
      const frames = [];
      for (let i = 0; i < 32; i++) frames.push({ key: "mon", frame: "f" + i });
      this.anims.create({ key: "idle", frames, frameRate: 12, repeat: -1 });

      this.root = this.add.container(0, 0).setScale(S);
      this.root.add(this.add.image(0, 0, "bg").setOrigin(0));
      // Field: bases, shadows, Pokemon
      this.mons = [];
      for (const [x, y] of [[236, 84], [84, 140]]) {
        this.root.add(this.add.image(x, y, "ui", "ui1").setOrigin(0.5));
        this.root.add(this.add.image(x, y - 2, "ui", "ui2").setAlpha(0.5));
        const m = this.add.sprite(x, y - 30, "mon").play("idle");
        this.mons.push(m);
        this.root.add(m);
      }
      this.uiObjects = [];
      this.texts = [];
      this.buildUI(1);
      this.message = this.add.text(12, 148, "", { fontFamily: "emerald", fontSize: "16px", color: "#f8f8f8" });
      this.root.add(this.message);
      bench.ready = true;
    }

    // One "unit" ~= one battle screen's worth of HUD.
    buildUI(units) {
      for (let u = 0; u < units; u++) {
        // Two Pokemon info boxes: frame, name, level, HP bar, status, gender, types, ...
        for (let b = 0; b < 2; b++) {
          const box = this.add.container(b ? 0 : 200, b ? 100 : 20);
          box.add(this.add.image(0, 0, "ui", "ui3").setOrigin(0));
          for (let i = 0; i < 12; i++) box.add(this.add.image(4 + i * 6, 10, "ui", "ui" + (8 + i)).setOrigin(0));
          const hp = this.add.image(30, 18, "ui", "ui30").setOrigin(0);
          box.add(hp);
          this.uiObjects.push(hp);
          const t = this.add.text(4, 2, "PIKACHU  Lv.100", { fontFamily: "emerald", fontSize: "8px" });
          box.add(t);
          this.texts.push(t);
          this.root.add(box);
        }
        // Modifier (item) bars: player and enemy, ~40 icons each, each with a stack-count text
        for (let side = 0; side < 2; side++) {
          const bar = this.add.container(side ? 200 : 0, side ? 2 : 2);
          for (let i = 0; i < 40; i++) {
            const icon = this.add.image((i % 20) * 6, Math.floor(i / 20) * 8, "items", "it" + ((i * 7) % 256)).setScale(0.5);
            bar.add(icon);
            this.uiObjects.push(icon);
          }
          for (let i = 0; i < 6; i++) {
            const t = this.add.text(i * 18, 12, "x" + (i + 2), { fontFamily: "pkmnems", fontSize: "6px" });
            bar.add(t);
            this.texts.push(t);
          }
          this.root.add(bar);
        }
        // Message window, party icons, command/fight menu
        const win = this.add.container(0, 140);
        for (let i = 0; i < 20; i++) win.add(this.add.image(i * 16, 0, "ui", "ui" + (40 + (i % 8))).setOrigin(0));
        for (let i = 0; i < 6; i++) win.add(this.add.sprite(220 + i * 16, 4, "mon", "f" + i).setScale(0.25));
        for (let i = 0; i < 4; i++) {
          const t = this.add.text(200 + (i % 2) * 50, 12 + Math.floor(i / 2) * 12, "MOVE " + i, { fontFamily: "emerald", fontSize: "8px" });
          win.add(t);
          this.texts.push(t);
        }
        this.root.add(win);
      }
    }

    startScenario(name) {
      this.tweens.killAll();
      this.time.removeAllEvents();
      if (name === "heavy") this.buildUI(2);
      if (name === "battle" || name === "heavy") {
        // HP bar drain + Pokemon bob, like a hit animation
        for (const hp of this.uiObjects.slice(0, 4)) {
          this.tweens.add({ targets: hp, scaleX: 0.2, duration: 1000, yoyo: true, repeat: -1 });
        }
        for (const m of this.mons) {
          this.tweens.add({ targets: m, y: m.y - 4, duration: 400, yoyo: true, repeat: -1, ease: "Sine.easeInOut" });
        }
        // Item icon pulse (shiny/new-item highlight)
        this.tweens.add({ targets: this.uiObjects.slice(10, 30), alpha: 0.4, duration: 600, yoyo: true, repeat: -1 });
        // Typewriter message: one character every 2 frames, like the battle message handler
        const msg = "A wild RAYQUAZA appeared! Go! PIKACHU! PIKACHU used THUNDERBOLT! It's super effective!";
        let n = 0;
        this.time.addEvent({
          delay: 33, loop: true,
          callback: () => { n = (n + 1) % msg.length; this.message.setText(msg.slice(0, n)); },
        });
        // Level/HP number text changes now and then
        this.time.addEvent({
          delay: 250, loop: true,
          callback: () => { const t = this.texts[0]; t.setText("PIKACHU  Lv." + (1 + ((Math.random() * 100) | 0))); },
        });
      }
    }
  }

  bench.start = function (name) {
    bench.scenario = name;
    bench.game.scene.getScene("bench").startScenario(name);
  };
  bench.objectCount = function () {
    let n = 0;
    const walk = (o) => { n++; if (o.list) o.list.forEach(walk); };
    bench.game.scene.getScene("bench").children.list.forEach(walk);
    return n;
  };
  // Per-frame breakdown: wrap a few Phaser entry points with timers.
  // enableProfile() before ticking, profileReport(frames) after.
  const prof = {};
  function wrap(obj, name, label) {
    const orig = obj[name];
    prof[label] = { ms: 0, calls: 0 };
    obj[name] = function () {
      const t0 = g.__now();
      try { return orig.apply(this, arguments); } finally { prof[label].ms += g.__now() - t0; prof[label].calls++; }
    };
  }
  let profiled = false;
  bench.enableProfile = function () {
    for (const k in prof) { prof[k].ms = 0; prof[k].calls = 0; }
    // batchSprite is swapped by __fastpath.set(), so wrap whichever version is current.
    wrap(Phaser.Renderer.WebGL.Pipelines.MultiPipeline.prototype, "batchSprite", "    batchSprite (per-sprite vertex math)");
    if (profiled) return;
    profiled = true;
    const game = bench.game;
    const P = Phaser.Renderer.WebGL.Pipelines;
    wrap(game.loop, "callback", "frame (game.step)");
    wrap(game.scene, "update", "  scene update (logic, tweens, timers, anims)");
    wrap(game.scene, "render", "  scene render (walk display list + batch)");
    wrap(P.MultiPipeline.prototype, "flush", "    pipeline flush (buffer upload + draw)");
    wrap(Phaser.GameObjects.Text.prototype, "updateText", "    Text.updateText (re-render label)");
    wrap(game.renderer, "preRender", "  renderer.preRender");
    wrap(game.renderer, "postRender", "  renderer.postRender");
  };
  bench.profileReport = function (frames) {
    const total = prof["frame (game.step)"].ms;
    return Object.entries(prof).map(([k, v]) =>
      "  " + k.padEnd(48) + (v.ms / frames).toFixed(2).padStart(7) + " ms/frame  " +
      (100 * v.ms / total).toFixed(0).padStart(3) + "%  " + (v.calls / frames).toFixed(1).padStart(6) + " calls/frame"
    ).join("\n") + "\n  (timer overhead inflates call-heavy rows)";
  };

  // Render the current (frozen) scene state once and capture every vertex flushed.
  function captureRender() {
    const chunks = [];
    const MP = Phaser.Renderer.WebGL.Pipelines.MultiPipeline.prototype;
    const flush = MP.flush;
    MP.flush = function () {
      if (this.vertexCount) chunks.push(this.vertexViewU32.slice(0, this.vertexCount * 7));
      return flush.apply(this, arguments);
    };
    const r = bench.game.renderer;
    r.preRender();
    bench.game.scene.render(r);
    r.postRender();
    MP.flush = flush;
    const n = chunks.reduce((t, c) => t + c.length, 0);
    const u = new Uint32Array(n);
    let o = 0;
    for (const c of chunks) { u.set(c, o); o += c.length; }
    return u;
  }
  // Compare JS vs native batching on identical scene state. Slot 6 of each vertex is the
  // packed tint (compared bit-exact), the rest are floats.
  bench.verifyFastpath = function () {
    __fastpath.set(false);
    const a = captureRender();
    __fastpath.set(true);
    const b = captureRender();
    __fastpath.set(false);
    if (a.length !== b.length) return "  fastpath check: LENGTH MISMATCH " + a.length + " vs " + b.length;
    const fa = new Float32Array(a.buffer), fb = new Float32Array(b.buffer);
    let maxDiff = 0, tintMismatch = 0;
    for (let i = 0; i < a.length; i++) {
      if (i % 7 === 6) { if (a[i] !== b[i]) tintMismatch++; continue; }
      const d = Math.abs(fa[i] - fb[i]);
      if (!(d <= maxDiff)) maxDiff = d;
    }
    return "  fastpath check: " + a.length / 7 + " vertices, max float diff " + maxDiff + ", tint mismatches " + tintMismatch;
  };

  bench.glCalls = function () { return bench.game.renderer.gl.__calls(); };

  try {
    bench.game = new Phaser.Game({
      type: Phaser.WEBGL,
      width: 1920, height: 1080,
      scale: { mode: Phaser.Scale.NONE },
      antialias: false,
      banner: false,
      audio: { noAudio: true },
      input: { keyboard: false, mouse: false, touch: false, gamepad: false },
      scene: [BattleBench],
    });
  } catch (e) {
    bench.error = String(e) + "\n" + (e && e.stack || "");
  }
})(globalThis);

// SPDX-License-Identifier: AGPL-3.0-only
// Faster rendering paths for Phaser 3.90 under QuickJS.
//
// 1. MultiPipeline.batchSprite: the matrix math, tint packing and 42 vertex writes go to
//    native code (__nativeBatchSprite, src/fastpath.c) in one call.
// 2. Container.renderWebGL: for plain Image/Sprite children, skip the temporary
//    setAlpha/setScrollFactor round trip (six JS calls per child) and pass the container's
//    alpha and scroll factor straight to the sprite batcher.
// 3. UpdateList.sceneUpdate: visit only sprites whose animation is playing (see below). Always on:
//    Phaser registers the function when each scene starts, so it can't be switched later.
// 4. Text.updateText: measure on every change, draw and upload only when rendered (see below);
//    a destroyed text's GPU texture serves a new text drawn with the same inputs.
// 5. NineSlice.renderWebGL: the vertex loop in native code.
// 6. CanvasPool.create/remove: constant-time instead of scanning the whole pool. 6b: a new Text's
//    canvas gets no GPU texture that its constructor would delete straight away.
// 7. Display list / container membership from the objects' own fields instead of list scans.
// 8. Utils.Array.Remove/RemoveAt and the WebGL renderer's wrapper lists: no JS shifting loop.
// 9. AnimationManager: the sprites' animation-removed listeners in a Set instead of an array.
// 10. Container.renderWebGL in C (src/fastpath.c).
// 11. TextStyle.setStyle without Phaser's generic GetValue lookups; font metrics remembered.
// 12. Utils.String.UUID (a texture name for every Text) made in C.
// 13. WebGLShader.setUniform1..4 in C (PokeRogue's sprite pipelines set ~10 uniforms per sprite).
// 14-17. Pipeline switching, Graphics, Rectangle/GeometryMask, EventEmitter.emit in C (see below).
// 18. BitmapText.renderWebGL with its character loop in C; in containers, all of it in C.
// 19. Tween.update and the common case of TweenData.update in C.
//
// Anything unusual (cropped sprites, custom pivots, masks, post pipelines, pipelines that
// override batchSprite/batchQuad such as PokeRogue's SpritePipeline) takes Phaser's own path.
(function (g) {
  if (!g.Phaser) return;
  "use strict";
  const MP = Phaser.Renderer.WebGL.Pipelines.MultiPipeline.prototype;
  const origSprite = MP.batchSprite;
  const origQuad = MP.batchQuad;
  const Container = Phaser.GameObjects.Container.prototype;
  const origContainerRender = Container.renderWebGL;
  const imageRender = Phaser.GameObjects.Image.prototype.renderWebGL;
  const spriteRender = Phaser.GameObjects.Sprite.prototype.renderWebGL;
  const nbs = g.__native.batchSprite;
  let enabled = false;

  // Returns false if this sprite needs Phaser's own path.
  function canFast(pipeline, go) {
    return !go.isCropped && !go.frame.customPivot && pipeline.batchQuad === MP.batchQuad &&
      pipeline.currentShader.vertexComponentCount === 7;
  }

  // Trimmed quads: PokeRogue draws many large, mostly transparent images (the arena's ground layers:
  // 1920x792 on screen with 4-10% visible; each egg gacha machine is three screen-wide layers, 11-33%
  // visible), and the GPU blends every transparent pixel. Textures from images get a map of where
  // their visible texels are (src/gl.c gl_alpha_box), and such a sprite is drawn as the part of
  // its quad that can have them (src/fastpath.c sprite_quad.trim): the same pixels, minus ones
  // that were left unchanged. (Where a pixel centre falls exactly on a texel edge, which needs a
  // scale like 1.25, float rounding may pick the other texel; PokeRogue draws 3 pixels a texel.) Only for pipelines whose shader draws a transparent texel as nothing
  // (MultiPipeline with tint effects 0 and 1; PokeRogue's FieldSprite with effect 0), in normal
  // blending, with one tint for the four corners. localStorage PKR_TRIM=0 turns it off.
  const TRIM_PIPELINES = { MultiPipeline: 2, FieldSprite: 1 };
  const alphaBox = g.__native.alphaBox;
  let trimOn = !!alphaBox;
  try { if (g.localStorage.getItem("PKR_TRIM") === "0") trimOn = false; } catch (e) { /* keep */ }
  // Sets pipeline._pkrTrim (0, 1, 2: see above) and frame._pkrTrim ([s0, t0, s1, t1] as fractions
  // of the frame, or false); a frame whose texture isn't uploaded yet is asked again later.
  function trimInfo(pipeline, frame) {
    if (pipeline._pkrTrim === undefined) pipeline._pkrTrim = (trimOn && TRIM_PIPELINES[pipeline.name]) || 0;
    if (frame._pkrTrim !== undefined) return;
    const tex = frame.source && frame.source.glTexture;
    if (!tex || !tex.webGLTexture) return;
    let t = false;
    const w = frame.cutWidth, h = frame.cutHeight;
    if (trimOn && !frame.rotated && w >= 32 && h >= 32) {
      const b = alphaBox(tex.webGLTexture, frame.cutX, frame.cutY, w, h);
      // under 80% of the frame: worth the four extra multiplications
      if (b && (b[2] - b[0]) * (b[3] - b[1]) < 0.8 * w * h) t = [b[0] / w, b[1] / h, b[2] / w, b[3] / h];
    }
    frame._pkrTrim = t;
  }
  g.__rt.trimInfo = trimInfo; // tests/phaser

  // alphaMul / sfx / sfy: multipliers inherited from the parent container.
  function batchFast(pipeline, go, camera, parent, alphaMul, sfx, sfy) {
    const frame = go.frame;
    pipeline.manager.set(pipeline, go);
    const dox = go.displayOriginX, doy = go.displayOriginY;
    let x = -dox + frame.x, y = -doy + frame.y, fx = 1, fy = 1;
    if (go.flipX) { x += -frame.realWidth + dox * 2; fx = -1; }
    if (go.flipY) { y += -frame.realHeight + doy * 2; fy = -1; }
    let gx = go.x, gy = go.y;
    if (camera.roundPixels) { gx = Math.floor(gx); gy = Math.floor(gy); }
    if (pipeline.shouldFlush(6)) pipeline.flush();
    let unit = pipeline.setGameObject(go, frame);
    pipeline.manager.preBatch(go);
    if (pipeline.shouldFlush(6)) pipeline.flush();
    if (!pipeline.currentBatch) unit = pipeline.setTexture2D(frame.glTexture);
    const a = camera.alpha * alphaMul;
    if (pipeline._pkrTrim === undefined || frame._pkrTrim === undefined) trimInfo(pipeline, frame);
    const trim = frame._pkrTrim;
    if (trim && pipeline._pkrTrim > (go.tintFill ? 1 : 0) && pipeline.renderer.currentBlendMode === 0 &&
        go.tintTopLeft === go.tintTopRight && go.tintTopLeft === go.tintBottomLeft && go.tintTopLeft === go.tintBottomRight &&
        go._alphaTL === go._alphaTR && go._alphaTL === go._alphaBL && go._alphaTL === go._alphaBR) {
      nbs(pipeline.vertexData, pipeline.vertexCount * 7, camera.matrix.matrix, parent ? parent.matrix : null,
        -camera.scrollX * go.scrollFactorX * sfx, -camera.scrollY * go.scrollFactorY * sfy,
        gx, gy, go.rotation, go.scaleX * fx, go.scaleY * fy, x, y, frame.cutWidth, frame.cutHeight,
        camera.renderRoundPixels, frame.u0, frame.v0, frame.u1, frame.v1,
        go.tintTopLeft, go.tintTopRight, go.tintBottomLeft, go.tintBottomRight,
        a * go._alphaTL, a * go._alphaTR, a * go._alphaBL, a * go._alphaBR, go.tintFill, unit, trim[0], trim[1], trim[2], trim[3]);
    } else {
      nbs(pipeline.vertexData, pipeline.vertexCount * 7, camera.matrix.matrix, parent ? parent.matrix : null,
        -camera.scrollX * go.scrollFactorX * sfx, -camera.scrollY * go.scrollFactorY * sfy,
        gx, gy, go.rotation, go.scaleX * fx, go.scaleY * fy, x, y, frame.cutWidth, frame.cutHeight,
        camera.renderRoundPixels, frame.u0, frame.v0, frame.u1, frame.v1,
        go.tintTopLeft, go.tintTopRight, go.tintBottomLeft, go.tintBottomRight,
        a * go._alphaTL, a * go._alphaTR, a * go._alphaBL, a * go._alphaBR, go.tintFill, unit);
    }
    pipeline.vertexCount += 6;
    pipeline.currentBatch.count = pipeline.vertexCount - pipeline.currentBatch.start;
    pipeline.onBatch(go);
    pipeline.manager.postBatch(go);
  }

  function fastBatchSprite(go, camera, parent) {
    if (!canFast(this, go)) return origSprite.call(this, go, camera, parent);
    batchFast(this, go, camera, parent, 1, 1, 1);
  }

  // Phaser's ContainerWebGLRenderer with a shortcut for plain sprite children.
  function fastContainerRender(renderer, container, camera, parentMatrix) {
    camera.addToRenderList(container);
    const children = container.list;
    const childCount = children.length;
    if (childCount === 0) return;
    const transformMatrix = container.localTransform;
    if (parentMatrix) {
      transformMatrix.loadIdentity();
      transformMatrix.multiply(parentMatrix);
      transformMatrix.translate(container.x, container.y);
      transformMatrix.rotate(container.rotation);
      transformMatrix.scale(container.scaleX, container.scaleY);
    } else {
      transformMatrix.applyITRS(container.x, container.y, container.rotation, container.scaleX, container.scaleY);
    }
    renderer.pipelines.preBatch(container);
    const containerHasBlendMode = container.blendMode !== -1;
    if (!containerHasBlendMode) renderer.setBlendMode(0);
    const alpha = container.alpha;
    const scrollFactorX = container.scrollFactorX;
    const scrollFactorY = container.scrollFactorY;
    for (let i = 0; i < childCount; i++) {
      const child = children[i];
      if (!child.willRender(camera)) continue;
      if (!containerHasBlendMode && child.blendMode !== renderer.currentBlendMode) renderer.setBlendMode(child.blendMode);
      const type = child.type;
      if (type !== renderer.currentType) { renderer.newType = true; renderer.currentType = type; }
      renderer.nextTypeMatch = i < childCount - 1 ? children[i + 1].type === renderer.currentType : false;
      const rw = child.renderWebGL;
      const pipeline = child.pipeline;
      if ((rw === imageRender || rw === spriteRender) && !child.mask && !child.hasPostPipeline &&
          child.alphaTopLeft !== undefined && enabled && pipeline.batchSprite === MP.batchSprite && canFast(pipeline, child)) {
        camera.addToRenderList(child);
        batchFast(pipeline, child, camera, transformMatrix, alpha, scrollFactorX, scrollFactorY);
        renderer.newType = false;
        continue;
      }
      // Phaser's generic path
      let tl, tr, bl, br;
      if (child.alphaTopLeft !== undefined) {
        tl = child.alphaTopLeft; tr = child.alphaTopRight; bl = child.alphaBottomLeft; br = child.alphaBottomRight;
      } else {
        tl = tr = bl = br = child.alpha;
      }
      const csx = child.scrollFactorX, csy = child.scrollFactorY;
      const mask = child.mask;
      if (mask) mask.preRenderWebGL(renderer, child, camera);
      child.setScrollFactor(csx * scrollFactorX, csy * scrollFactorY);
      child.setAlpha(tl * alpha, tr * alpha, bl * alpha, br * alpha);
      child.renderWebGL(renderer, child, camera, transformMatrix, container);
      child.setAlpha(tl, tr, bl, br);
      child.setScrollFactor(csx, csy);
      if (mask) mask.postRenderWebGL(renderer, camera);
      renderer.newType = false;
    }
    renderer.pipelines.postBatch(container);
  }

  // 3. UpdateList.sceneUpdate: Phaser calls preUpdate on every object in a scene's update list each
  //    frame. For a plain Sprite that is anims.update(), which returns at once unless an animation
  //    is playing. PokeRogue keeps over a thousand sprites in its lists (UI icons, hidden ones
  //    included) and few animate; even just visiting them costs ~17 ms per frame on the Vita.
  //    So the runtime tracks which animations are playing (writes to AnimationState.isPlaying) and
  //    each frame visits only those sprites plus the list's other (non-Sprite) objects, which are
  //    cached until the list changes. Animations update in no particular order between sprites.
  const playing = new Set();
  const ASP = Phaser.Animations.AnimationState.prototype;
  Object.defineProperty(ASP, "isPlaying", {
    configurable: true,
    get() { return this._pkrPlaying === true; },
    set(v) {
      v = !!v;
      if (v === (this._pkrPlaying === true)) return;
      this._pkrPlaying = v;
      if (v) playing.add(this); else playing.delete(this);
    },
  });
  const origAnimDestroy = ASP.destroy;
  ASP.destroy = function () { playing.delete(this); return origAnimDestroy.apply(this, arguments); };
  const PQ = Phaser.Structs.ProcessQueue.prototype;
  const PQ_ADD = Phaser.Structs.Events.PROCESS_QUEUE_ADD, PQ_REMOVE = Phaser.Structs.Events.PROCESS_QUEUE_REMOVE;
  const spritePreUpdate = Phaser.GameObjects.Sprite.prototype.preUpdate;
  // Membership in the update list's queues by Set instead of indexOf: every sprite added to a scene
  // checks both its pending and active arrays (thousands long during UI setup). The sets follow the
  // arrays incrementally; a size mismatch (someone changed an array directly) rebuilds them.
  function pendingSet(q) {
    if (!q._pkrPending || q._pkrPendingArr !== q._pending || q._pkrPending.size !== q._pending.length) {
      q._pkrPending = new Set(q._pending);
      q._pkrPendingArr = q._pending;
    }
    return q._pkrPending;
  }
  function activeSet(q) {
    if (!q._pkrActive || q._pkrActiveArr !== q._active || q._pkrActive.size !== q._active.length) {
      q._pkrActive = new Set(q._active);
      q._pkrActiveArr = q._active;
    }
    return q._pkrActive;
  }
  // fastSceneUpdate's split of an update list into animated sprites (members) and everything else
  // (others, in list order).
  function rebuildSplit(q) {
    const list = q._active, others = [], members = new Set();
    for (let i = 0; i < list.length; i++) {
      const go = list[i];
      if (go.preUpdate === spritePreUpdate) members.add(go); else others.push(go);
    }
    q._pkrList = list, q._pkrOthers = others, q._pkrMembers = members;
  }
  // ProcessQueue.update with the same effects (removals first, then additions, same events), using
  // the sets instead of indexOf, and keeping the split up to date instead of rebuilding it.
  PQ.update = function () {
    if (this._toProcess === 0) return this._active;
    const active = this._active, aset = activeSet(this);
    const split = this._pkrMembers && this._pkrList === active ? this : null;
    const d = this._destroy;
    for (let i = 0; i < d.length; i++) {
      const item = d[i];
      if (!aset.has(item)) continue;
      active.splice(active.indexOf(item), 1);
      aset.delete(item);
      if (split && !split._pkrMembers.delete(item)) {
        const k = split._pkrOthers.indexOf(item);
        if (k !== -1) split._pkrOthers.splice(k, 1);
      }
      this.emit(PQ_REMOVE, item);
    }
    d.length = 0;
    const p = this._pending;
    for (let i = 0; i < p.length; i++) {
      const item = p[i];
      if (this.checkQueue && aset.has(item)) continue;
      active.push(item);
      aset.add(item);
      if (split) { if (item.preUpdate === spritePreUpdate) split._pkrMembers.add(item); else split._pkrOthers.push(item); }
      this.emit(PQ_ADD, item);
    }
    p.length = 0;
    if (this._pkrPending) this._pkrPending.clear();
    this._toProcess = 0;
    return active;
  };
  PQ.isPending = function (item) { return this._toProcess > 0 && pendingSet(this).has(item); };
  PQ.isActive = function (item) { return activeSet(this).has(item); };
  PQ.add = function (item) {
    if ((this.checkQueue && this.isActive(item) && !this.isDestroying(item)) || this.isPending(item)) return item;
    this._pending.push(item);
    pendingSet(this).add(item);
    this._toProcess++;
    return item;
  };
  PQ.remove = function (item) {
    if (this.isPending(item)) {
      const t = this._pending, i = t.lastIndexOf(item);
      if (i !== -1) t.splice(i, 1);
      pendingSet(this).delete(item);
    } else if (this.isActive(item)) {
      this._destroy.push(item);
      this._toProcess++;
    }
    return item;
  };
  const UL = Phaser.GameObjects.UpdateList.prototype;
  const origSceneUpdate = UL.sceneUpdate;
  function fastSceneUpdate(time, delta) {
    const list = this._active;
    if (list !== this._pkrList || !this._pkrMembers || this._pkrMembers.size + this._pkrOthers.length !== list.length) rebuildSplit(this);
    const others = this._pkrOthers;
    for (let i = 0, n = others.length; i < n; i++) {
      const go = others[i];
      if (go.active) go.preUpdate.call(go, time, delta);
    }
    if (playing.size === 0) return;
    const members = this._pkrMembers, states = Array.from(playing);
    for (let i = 0; i < states.length; i++) {
      const a = states[i], go = a.parent;
      if (!a.isPlaying || !go || !go.active || !members.has(go)) continue;
      a.update(time, delta);
    }
  }
  UL.sceneUpdate = fastSceneUpdate; // registered as a listener when each scene starts: always on

  // 4. Text.updateText: Phaser redraws a text object's canvas and uploads it to the GPU on every
  //    change (setText, setStyle, setColor...), even while the text is hidden. PokeRogue builds
  //    over a thousand texts during setup, about five updates each. Here a change only measures
  //    (Phaser's own code, with drawing switched off, so width, height and wrapping stay exact for
  //    the game's layout); the drawing and upload happen once, when the text is next rendered.
  const TP = Phaser.GameObjects.Text.prototype;
  const origUpdateText = TP.updateText, origTextRender = TP.renderWebGL;
  const IDENTITY = [1, 0, 0, 1, 0, 0];
  const noop = () => undefined;
  // Stand-in for a canvas's native object while a text is only measured: font, measuring and
  // size calls go through; drawing and state calls do nothing. One shared prototype.
  let stubProto = null;
  function measureStub(real) {
    if (!stubProto) {
      stubProto = { measureOnly: true, getTransform: () => IDENTITY };
      for (let p = Object.getPrototypeOf(real); p && p !== Object.prototype; p = Object.getPrototypeOf(p))
        for (const k of Object.getOwnPropertyNames(p)) if (!(k in stubProto) && typeof real[k] === "function") stubProto[k] = noop;
      for (const k of ["setFont", "measureText", "width", "height", "setFill", "setStroke", "resize"])
        stubProto[k] = function () { return this.real[k].apply(this.real, arguments); };
    }
    const stub = Object.create(stubProto);
    stub.real = real;
    return stub;
  }
  const stubs = new WeakMap(); // native canvas object -> its measuring stand-in
  const textStats = { updates: 0, measures: 0, draws: 0, reused: 0, same: 0 };
  g.__rt.textStats = () => Object.assign({}, textStats);
  // useTex: the GPU texture the text gets (a reused one, see textCache), else it keeps its own.
  function measureNow(t, useTex) {
    textStats.measures++;
    t._pkrMeasureDirty = false;
    // measured in some other state than its texture shows: its sizes are that state's now, so the
    // next render draws (drawText's skip relies on the sizes of the last draw)
    if (!useTex) t._pkrSig = null;
    const ctx = t.context;
    const real = ctx._n, tex = useTex || (t.frame && t.frame.source && t.frame.source.glTexture) || null;
    const r = t.renderer, origC2T = r.canvasToTexture;
    let stub = stubs.get(real);
    if (!stub) stubs.set(real, (stub = measureStub(real)));
    ctx._n = stub;
    // The upload at the end of updateText: keep the current texture. A new text gets none until it
    // is drawn (Phaser would make one here and fill it with the blank canvas; the draw uploads
    // again, and texts that are never shown would hold one for nothing).
    r.canvasToTexture = () => tex;
    try { origUpdateText.call(t); } finally { ctx._n = real; delete r.canvasToTexture; }
    if (r.canvasToTexture !== origC2T) r.canvasToTexture = origC2T;
  }
  // Texts drawn again: PokeRogue builds its screens anew each time they open (the party screen:
  // ~40 texts, then destroyed when it closes), mostly with the same words. A destroyed text's GPU
  // texture is kept under the inputs of its last draw (text, style, padding, spacing); a new text
  // drawn with the same inputs takes it instead of drawing its canvas and uploading it. Phaser's
  // drawing is deterministic in those inputs, so the pixels are the same. A text whose colour is a
  // gradient or pattern, or whose wrapping is a callback, is never kept, nor one over
  // TEXT_CACHE_ONE (the message window's lines, ~1 MB each, rarely repeat). Least recently kept go
  // first past TEXT_CACHE_BYTES. Texts are drawn at 6x (a Pokémon's name: 221x69, 61 KB).
  const TEXT_CACHE_BYTES = 4 << 20, TEXT_CACHE_ONE = 256 << 10;
  const textCache = new Map(); // signature -> [texture wrappers]
  let textCacheBytes = 0;
  const texBytes = (tex) => (tex.width | 0) * (tex.height | 0) * 4;
  function textSignature(t) {
    const s = t.style, p = t.padding, m = s.metrics;
    if (typeof t._text !== "string" || s.wordWrapCallback || typeof s.color !== "string" || typeof s.stroke !== "string" ||
        typeof s.shadowColor !== "string" || (s.backgroundColor !== null && typeof s.backgroundColor !== "string") || !m || !p ||
        !(t.splitRegExp instanceof RegExp)) return null;
    return [t._text, s._font, s.color, s.stroke, s.strokeThickness, s.backgroundColor, s.shadowOffsetX, s.shadowOffsetY,
      s.shadowColor, s.shadowBlur, s.shadowStroke, s.shadowFill, s.align, s.maxLines, s.fixedWidth, s.fixedHeight,
      s.resolution, s.rtl, s.wordWrapWidth, s.wordWrapUseAdvanced, m.ascent, m.descent, m.fontSize, p.left, p.right, p.top,
      p.bottom, t.lineSpacing, t.letterSpacing, t.autoRound, t.splitRegExp.source, g.__rt.fontGeneration | 0].join("\u0001");
  }
  function textCacheTake(sig) {
    const list = textCache.get(sig);
    if (!list) return null;
    const tex = list.pop();
    if (!list.length) textCache.delete(sig);
    textCacheBytes -= texBytes(tex);
    return tex;
  }
  function textCachePut(sig, tex, renderer) {
    const bytes = texBytes(tex);
    if (!bytes || bytes > TEXT_CACHE_ONE) return false;
    const list = textCache.get(sig);
    if (list) { textCache.delete(sig); list.push(tex); textCache.set(sig, list); } else textCache.set(sig, [tex]);
    textCacheBytes += bytes;
    for (const [k, l] of textCache) {
      if (textCacheBytes <= TEXT_CACHE_BYTES) break;
      while (l.length && textCacheBytes > TEXT_CACHE_BYTES) {
        const old = l.shift();
        textCacheBytes -= texBytes(old);
        renderer.deleteTexture(old);
      }
      if (!l.length) textCache.delete(k);
    }
    return true;
  }
  g.__rt.textCache = {
    stats: () => ({ entries: textCache.size, bytes: textCacheBytes }),
    clear() {
      const r = g.__rt.renderer;
      for (const l of textCache.values()) for (const tex of l) if (r) r.deleteTexture(tex);
      textCache.clear();
      textCacheBytes = 0;
    },
  };
  // The draw a render does when the text changed: Phaser's updateText, or a kept texture.
  function drawText(t) {
    const src = t.frame && t.frame.source;
    const sig = src && g.__rt.textReuse !== false ? textSignature(t) : null;
    // Changed back to what its texture already shows (the game sets the same text, colour or shadow
    // again: the starter screen does it to its 81 cost labels on every filter change, log232): the
    // same inputs draw the same pixels at the same size, so nothing to draw or upload.
    if (sig && sig === t._pkrSig && src.glTexture && t.canvas && t.canvas.width && g.__rt.textSkipSame !== false) {
      textStats.same++;
      return;
    }
    textStats.draws++;
    if (sig && !src.glTexture) {
      const tex = textCacheTake(sig);
      if (tex) {
        measureNow(t, tex);
        textStats.reused++;
        // Same inputs give the same size; anything else draws into it after all.
        if (t.canvas.width === tex.width && t.canvas.height === tex.height) { t._pkrSig = sig; return; }
        textStats.reused--;
      }
    }
    origUpdateText.call(t);
    t._pkrSig = sig;
  }
  const origTextPreDestroy = TP.preDestroy;
  if (typeof origTextPreDestroy === "function") {
    TP.preDestroy = function () {
      const src = this.frame && this.frame.source, tex = src && src.glTexture;
      // _pkrSig: the inputs the texture was last drawn with (cleared when Phaser's own updateText ran).
      if (enabled && tex && this._pkrSig && this.renderer && textCachePut(this._pkrSig, tex, this.renderer)) src.glTexture = null;
      return origTextPreDestroy.call(this);
    };
  }
  // Measuring is deferred too: the game typically changes a new text four times in a row
  // (constructor, setPadding, setShadow, setLineSpacing) and reads its size at most once.
  // A change only marks the text; its size properties are accessors that measure on first read
  // (width, height and the display origin, which updateText derives from them).
  let lazySize = false;
  {
    const keys = ["width", "height", "_displayOriginX", "_displayOriginY"];
    const descs = keys.map((k) => Object.getOwnPropertyDescriptor(TP, k));
    if (descs.every((d) => d && "value" in d && d.writable)) {
      keys.forEach((k, i) => {
        const store = "_pkr_" + k;
        TP[store] = descs[i].value;
        Object.defineProperty(TP, k, {
          configurable: true, enumerable: true,
          get() { if (this._pkrMeasureDirty) measureNow(this); return this[store]; },
          set(v) { this[store] = v; },
        });
      });
      lazySize = true;
    }
  }
  function lazyUpdateText() {
    const ctx = this.context;
    if (!enabled || !this.renderer || !this.renderer.gl || !ctx || !ctx._n) {
      this._pkrMeasureDirty = false;
      this._pkrSig = null;
      return origUpdateText.call(this);
    }
    textStats.updates++;
    this._pkrTextDirty = true;
    if (lazySize && g.__rt.lazyMeasure !== false) this._pkrMeasureDirty = true;
    else measureNow(this);
    return this;
  }
  function textRender(renderer, src) {
    if (src._pkrTextDirty) {
      src._pkrTextDirty = false;
      src._pkrMeasureDirty = false;
      drawText(src);
    }
    return origTextRender.apply(this, arguments);
  }
  TP.updateText = lazyUpdateText;
  TP.renderWebGL = textRender;

  // 4b. The same for rex's BBCodeText (phaser3-rex-plugins, the game's rich text: menu options, the
  //     party screen's item lists). Its updateText parses the tags, measures, draws the canvas (96 px
  //     fonts, scaled down) and uploads it on every change, and the game changes a new one ~5 times
  //     in a row (constructor, setShadow, setLineSpacing, setText twice) before reading its width.
  //     The item transfer list makes ~10 of them each time it scrolls: ~385 ms a press on the Vita
  //     (log200). Here a change only marks the text: it is measured (rex's own code, drawing
  //     switched off) when its size or lines are first read, and drawn and uploaded when rendered,
  //     or given the texture a destroyed text with the same inputs left (textCache).
  //     The class is the plugin's (window.RexPlugins.GameObjects.BBCodeText, set when its module
  //     loads); it is patched when the game registers the plugin. Other rex texts are left alone.
  const rexStats = { updates: 0, measures: 0, draws: 0, reused: 0 };
  g.__rt.rexTextStats = () => Object.assign({}, rexStats);
  function patchRexText(BB) {
    const RTP = BB && BB.prototype && Object.getPrototypeOf(BB.prototype);
    if (!RTP || Object.prototype.hasOwnProperty.call(RTP, "_pkrLazy") || typeof RTP.updateText !== "function" ||
        typeof RTP.renderWebGL !== "function" || typeof RTP.preDestroy !== "function") return false;
    // Not the code this was written for: rex 1.80's updateText(runWrap) lays out the lines
    // (canvasText.updatePenManager), sizes the canvas, draws it and uploads it (canvasToTexture). The
    // game's code is precompiled without its source, so where the source can't be read the shape
    // is checked: these methods, and on first use the text's canvasText (see usable).
    const src = Function.prototype.toString.call(RTP.updateText);
    if (/\[native code\]/.test(src)) {
      if (RTP.updateText.length !== 1 || typeof RTP.setText !== "function" || typeof RTP.setPadding !== "function" ||
          typeof RTP.getWrappedText !== "function" || typeof BB.prototype.setDelimiters !== "function") return false;
    } else if (!/updatePenManager/.test(src) || !/canvasToTexture/.test(src)) return false;
    let shapeOk = null; // canvasText checked on the first text
    const usable = (t) => {
      if (shapeOk === null) {
        const ct = t.canvasText;
        if (!ct) return false;
        shapeOk = typeof ct.updatePenManager === "function" && typeof ct.draw === "function" && "linesWidth" in ct &&
          !!t.style && typeof t.style.toJSON === "function" && !!t.padding;
        if (!shapeOk) g.__native.log("[fastpath] rex BBCodeText: its text layout isn't the one expected; left as it is");
      }
      return shapeOk;
    };
    const origUpdate = RTP.updateText, origRender = RTP.renderWebGL, origPreDestroy = RTP.preDestroy;
    RTP._pkrLazy = true;
    // The size becomes accessors that measure first while a change is pending. Only if they are
    // plain data properties further up (Phaser's ComputedSize and Origin components).
    const keys = ["width", "height", "_displayOriginX", "_displayOriginY"];
    let lazy = true;
    for (const k of keys) {
      let d = null;
      for (let p = RTP; p && !d; p = Object.getPrototypeOf(p)) d = Object.getOwnPropertyDescriptor(p, k);
      if (d && !("value" in d)) lazy = false;
    }
    if (lazy) {
      for (const k of keys) {
        const store = "_pkrR_" + k;
        let d = null;
        for (let p = RTP; p && !d; p = Object.getPrototypeOf(p)) d = Object.getOwnPropertyDescriptor(p, k);
        RTP[store] = d ? d.value : undefined;
        Object.defineProperty(RTP, k, {
          configurable: true, enumerable: true,
          get() { if (this._pkrRexMeasure) rexMeasure(this); return this[store]; },
          set(v) { this[store] = v; },
        });
      }
    }
    // So do the methods that read the laid-out lines (not canvasText itself: the constructor sets
    // its link cursor style through it, after the first setText).
    if (lazy) {
      for (const k of ["getWrappedText", "getPlainText", "getText", "copyPenManager", "getPenManager", "setInteractive",
        "drawAreaBounds", "getHitArea"]) {
        const f = RTP[k];
        if (typeof f !== "function") continue;
        RTP[k] = function () { if (this._pkrRexMeasure) rexMeasure(this); return f.apply(this, arguments); };
      }
      for (let p = RTP; p; p = Object.getPrototypeOf(p)) {
        const d = Object.getOwnPropertyDescriptor(p, "contentHeight");
        if (!d) continue;
        if (typeof d.get === "function") {
          const get = d.get;
          Object.defineProperty(RTP, "contentHeight", { configurable: true, enumerable: d.enumerable, set: d.set,
            get() { if (this._pkrRexMeasure) rexMeasure(this); return get.call(this); } });
        }
        break;
      }
    }
    // rex's updateText with the canvas's drawing calls switched off and the upload skipped.
    function rexMeasure(t, useTex) {
      rexStats.measures++;
      t._pkrRexMeasure = false;
      const wrap = t._pkrRexWrap;
      t._pkrRexWrap = false;
      const ctx = t.context, real = ctx._n, fsrc = t.frame && t.frame.source;
      const tex = useTex || (fsrc && fsrc.glTexture) || null;
      const r = t.renderer;
      let stub = stubs.get(real);
      if (!stub) stubs.set(real, (stub = measureStub(real)));
      ctx._n = stub;
      // (rex tags the texture it gets for a WebGL debugger, so a text without one yet gets a
      // stand-in for the call, then none again.)
      r.canvasToTexture = () => tex || {};
      try { origUpdate.call(t, wrap); } finally {
        ctx._n = real;
        delete r.canvasToTexture;
        if (fsrc) fsrc.glTexture = tex;
      }
    }
    // The inputs of a draw: rex's drawing is deterministic in its text, style (all of it, as
    // toJSON lists it), padding and lines' scroll. Texts with images or a custom parser are not kept.
    function rexSignature(t) {
      if (typeof t._text !== "string" || t._imageManager || !t.style || typeof t.style.toJSON !== "function" || !t.padding) return null;
      let style;
      try { style = JSON.stringify(t.style.toJSON()); } catch (e) { return null; }
      const p = t.padding;
      return ["rex", t.type, t._text, style, p.left, p.right, p.top, p.bottom, t._scrollY, g.__rt.fontGeneration | 0].join("\u0001");
    }
    function rexDraw(t) {
      rexStats.draws++;
      const fsrc = t.frame && t.frame.source;
      const sig = fsrc && g.__rt.textReuse !== false ? rexSignature(t) : null;
      if (sig && !fsrc.glTexture) {
        const tex = textCacheTake(sig);
        if (tex) {
          rexMeasure(t, tex); // sizes the canvas and takes tex as the text's texture
          if (t.canvas.width === tex.width && t.canvas.height === tex.height) { rexStats.reused++; t._pkrSig = sig; return; }
          // a different size after all: draw into it
        }
      }
      const wrap = t._pkrRexWrap;
      t._pkrRexWrap = false;
      t._pkrRexMeasure = false;
      origUpdate.call(t, wrap);
      t._pkrSig = sig;
    }
    RTP.updateText = function (runWrap) {
      const ctx = this.context;
      if (!enabled || g.__rt.lazyRexText === false || !this.renderer || !this.renderer.gl || !ctx || !ctx._n || !lazy || !usable(this)) {
        const pending = this._pkrRexWrap; // lines not laid out since an earlier change
        this._pkrRexDraw = this._pkrRexMeasure = this._pkrRexWrap = false;
        this._pkrSig = null;
        return origUpdate.call(this, runWrap === undefined || !!runWrap || !!pending);
      }
      rexStats.updates++;
      if (runWrap === undefined || runWrap) this._pkrRexWrap = true;
      this._pkrRexDraw = true;
      this._pkrSig = null;
      // An interactive text's hit area is sized here: measure now.
      if (!this.input && g.__rt.lazyMeasure !== false) this._pkrRexMeasure = true;
      else { this._pkrRexMeasure = true; rexMeasure(this); }
      return this;
    };
    RTP.renderWebGL = function (renderer, s) {
      if (s._pkrRexDraw) {
        s._pkrRexDraw = false;
        rexDraw(s);
      }
      return origRender.apply(this, arguments);
    };
    RTP.preDestroy = function () {
      const fsrc = this.frame && this.frame.source, tex = fsrc && fsrc.glTexture;
      this._pkrRexMeasure = false; // nothing reads its size any more
      if (enabled && tex && this._pkrSig && !this._pkrRexDraw && this.renderer && textCachePut(this._pkrSig, tex, this.renderer)) fsrc.glTexture = null;
      return origPreDestroy.apply(this, arguments);
    };
    g.__rt.rexTextPatched = true;
    return true;
  }
  {
    const PM = Phaser.Plugins && Phaser.Plugins.PluginManager && Phaser.Plugins.PluginManager.prototype;
    const origReg = PM && PM.registerGameObject;
    if (typeof origReg === "function") {
      PM.registerGameObject = function (key) {
        if (key === "rexBBCodeText" && !g.__rt.rexTextPatched) {
          const ns = g.RexPlugins && g.RexPlugins.GameObjects;
          const ok = patchRexText(ns && ns.BBCodeText);
          g.__native.log("[fastpath] rex BBCodeText: " + (ok ? "drawn when rendered" : "left as it is (not the code this was written for)"));
        }
        return origReg.apply(this, arguments);
      };
    }
    g.__rt.patchRexText = patchRexText; // tests/phaser
  }

  // 5. NineSlice.renderWebGL (the game's window frames, 54 vertices each): Phaser's code with the
  //    vertex loop in native code (__native.batchVerts). If the batch can't take all vertices,
  //    it is flushed first instead of mid-way.
  const NSP = Phaser.GameObjects.NineSlice.prototype, origNineSlice = NSP.renderWebGL;
  const GetCalcMatrix = Phaser.GameObjects.GetCalcMatrix, WGLUtils = Phaser.Renderer.WebGL.Utils;
  const nbv = g.__native.batchVerts;
  function nineSliceRender(renderer, src, camera, parentMatrix) {
    const verts = src.vertices, n = verts.length;
    if (n === 0) return;
    const sp = src.pipeline;
    if (!enabled || !nbv || !GetCalcMatrix || !sp || !sp.currentShader || sp.currentShader.vertexComponentCount !== 7 ||
        sp.vertexCapacity < n) return origNineSlice.apply(this, arguments);
    camera.addToRenderList(src);
    const pipeline = renderer.pipelines.set(sp, src);
    const calc = GetCalcMatrix(src, camera, parentMatrix, false).calc;
    renderer.pipelines.preBatch(src);
    if (pipeline.vertexAvailable() < n) pipeline.flush();
    const unit = pipeline.setGameObject(src);
    const tint = WGLUtils.getTintAppendFloatAlpha(src.tint, camera.alpha * src.alpha);
    nbv(pipeline.vertexData, pipeline.vertexCount * 7, calc.matrix, verts, camera.roundPixels, unit, src.tintFill, tint);
    pipeline.vertexCount += n;
    pipeline.currentBatch.count = pipeline.vertexCount - pipeline.currentBatch.start;
    renderer.pipelines.postBatch(src);
  }
  // All of it in C when available (src/fastpath.c "NineSlice"); localStorage PKR_NATIVE_NINESLICE=0
  // keeps the version above.
  let nineSliceFast = nineSliceRender;
  try {
    if (g.__native.nineSliceSetup && GetCalcMatrix && g.localStorage.getItem("PKR_NATIVE_NINESLICE") !== "0") {
      nineSliceFast = g.__native.nineSliceSetup(origNineSlice);
      g.__rt.nineSliceSwap = [[NSP, "renderWebGL", nineSliceRender, nineSliceFast]];
    }
  } catch (e) { nineSliceFast = nineSliceRender; }
  NSP.renderWebGL = nineSliceRender;

  // 6. CanvasPool.create / remove: Phaser keeps one array of pooled canvases and scans all of it
  //    to find a free one or the one to release, which grows with the square of the number of
  //    text objects (over a thousand in PokeRogue). A free list and lookup maps make both
  //    constant-time. Free canvases are reused most-recently-freed first instead of pool order.
  const CP = Phaser.Display.Canvas.CanvasPool, pool = CP.pool, Smoothing = Phaser.Display.Canvas.Smoothing;
  const CANVAS = Phaser.CANVAS;
  const origCreate = CP.create, origRemove = CP.remove, origDisable = CP.disableSmoothing, origEnable = CP.enableSmoothing;
  const origFreeAll = CP.free;
  let smoothingOff = false;
  const freeList = [], byCanvas = new Map(), byParent = new Map();
  function track(c) {
    let set = byParent.get(c.parent);
    if (!set) byParent.set(c.parent, (set = new Set()));
    set.add(c);
  }
  function release(c) {
    if (c.parent) {
      const set = byParent.get(c.parent);
      if (set) { set.delete(c); if (!set.size) byParent.delete(c.parent); }
    }
    c.parent = null;
    c.canvas.width = 1;
    c.canvas.height = 1;
    if (!c._pkrFree) { c._pkrFree = true; freeList.push(c); }
  }
  CP.disableSmoothing = function () { smoothingOff = true; return origDisable.apply(this, arguments); };
  CP.enableSmoothing = function () { smoothingOff = false; return origEnable.apply(this, arguments); };
  CP.create = function (parent, width = 1, height = 1, canvasType = CANVAS, selfParent = false) {
    if (canvasType !== CANVAS) return origCreate.apply(this, arguments); // not pooled
    let c = null;
    while (freeList.length) {
      const f = freeList.pop();
      f._pkrFree = false;
      if (!f.parent && f.type === CANVAS) { c = f; break; }
    }
    if (!c) {
      c = { parent, canvas: g.document.createElement("canvas"), type: canvasType };
      pool.push(c);
      byCanvas.set(c.canvas, c);
    } else {
      c.parent = parent;
    }
    const canvas = c.canvas;
    if (selfParent) c.parent = canvas;
    track(c);
    canvas.width = width;
    canvas.height = height;
    if (smoothingOff) Smoothing.disable(canvas.getContext("2d", { willReadFrequently: false }));
    return canvas;
  };
  CP.remove = function (parent) {
    if (parent instanceof g.HTMLCanvasElement) {
      const c = byCanvas.get(parent);
      if (c) release(c);
      return;
    }
    const set = byParent.get(parent);
    if (set) for (const c of Array.from(set)) release(c);
  };
  CP.free = function () {
    freeList.length = 0;
    byCanvas.clear();
    byParent.clear();
    return origFreeAll.apply(this, arguments);
  };

  // 6b. A new Text's first GPU texture: Phaser 3.90's Text constructor registers its 1x1 canvas with the
  // texture manager (addCanvas, which makes and filters a GPU texture for it) and deletes that texture
  // right away ("we override it later"). PokeRogue makes ~1,500 texts when it builds its screens (at
  // start and after every game reset), and each texture made and freed went through vitaGL's
  // allocator: ~0.7 s of a 7.6 s reset on the Vita (log142). For the canvas of a Text being built
  // (CanvasPool.create with a Text as parent, then addCanvas of that canvas), no texture is made: the
  // constructor then deletes null and sets null, as it would have.
  const TMP = Phaser.Textures && Phaser.Textures.TextureManager && Phaser.Textures.TextureManager.prototype;
  if (TMP && typeof TMP.addCanvas === "function" &&
      /deleteTexture\(this\.frame\.source\.glTexture\)/.test(String(Phaser.GameObjects.Text))) {
    const poolCreate = CP.create, origAddCanvas = TMP.addCanvas, TextClass = Phaser.GameObjects.Text;
    let pendingCanvas = null;
    CP.create = function (parent) {
      const c = poolCreate.apply(this, arguments);
      if (parent instanceof TextClass && !parent.texture) pendingCanvas = c; // (its TextStyle measures with other canvases next)
      return c;
    };
    const noTexture = () => null;
    TMP.addCanvas = function (key, source) {
      const r = this.game && this.game.renderer;
      if (source !== pendingCanvas || !r || !r.gl) return origAddCanvas.apply(this, arguments);
      pendingCanvas = null;
      const filter = r.setTextureFilter;
      r.createCanvasTexture = noTexture;
      r.setTextureFilter = function (t) { return t ? filter.apply(this, arguments) : this; };
      try { return origAddCanvas.apply(this, arguments); } finally { delete r.createCanvasTexture; delete r.setTextureFilter; }
    };
    g.__rt.textTextureSwap = true; // tests/phaser
  }
  // 6c. A canvas texture's pixel copy: Phaser's CanvasTexture reads its whole canvas back when it is
  //     made (getImageData) and keeps the copy (imageData, data, buffer, pixels) for its pixel methods
  //     (getPixel, setPixel, putData...), which PokeRogue never calls. A stat change's TileSprite has a
  //     canvas the size of the Pokemon's sprite: 936x1896 for wave 200's boss, a 7 MB copy nothing
  //     reads, and that one ran the Vita out of memory late in a long session (log207). The copy is
  //     all transparent anyway here: the texture's GPU upload comes first in Phaser's constructor, and
  //     src/canvas2d.c frees a canvas's pixels once uploaded. Now the copy is made when one of those
  //     four is first read, at the size the texture was made with (giving what Phaser's gives on this
  //     runtime); Phaser's own update() sets them as before.
  {
    const CT = Phaser.Textures && Phaser.Textures.CanvasTexture;
    const TMP2 = Phaser.Textures && Phaser.Textures.TextureManager && Phaser.Textures.TextureManager.prototype;
    const ctSrc = CT ? String(CT) : "";
    if (TMP2 && typeof TMP2.addCanvas === "function" && /imageData=this\.context\.getImageData\(0,0,/.test(ctSrc)) {
      const addCanvas = TMP2.addCanvas, tiny = () => new g.ImageData(1, 1);
      const KEYS = ["imageData", "data", "buffer", "pixels"];
      const lazyStats = { textures: 0, copies: 0 };
      g.__rt.canvasCopyStats = () => Object.assign({}, lazyStats);
      TMP2.addCanvas = function (key, source) {
        const ctx = enabled && g.__rt.lazyCanvasCopy !== false && source && typeof source.getContext === "function" ? source.getContext("2d") : null;
        if (!ctx || typeof ctx.getImageData !== "function" || Object.prototype.hasOwnProperty.call(ctx, "getImageData"))
          return addCanvas.apply(this, arguments);
        ctx.getImageData = tiny; // the constructor's read-back
        let tex;
        try { tex = addCanvas.apply(this, arguments); } finally { delete ctx.getImageData; }
        if (!tex || !(tex instanceof CT)) return tex;
        lazyStats.textures++;
        const v = {};
        let loaded = false;
        const load = () => {
          if (loaded) return;
          loaded = true;
          lazyStats.copies++;
          const id = tex.context.getImageData(0, 0, tex.width, tex.height);
          v.imageData = id, v.data = id.data, v.buffer = id.data.buffer, v.pixels = new Uint32Array(id.data.buffer);
        };
        for (const k of KEYS)
          Object.defineProperty(tex, k, { configurable: true, enumerable: true,
            get() { load(); return v[k]; },
            set(x) { loaded = true; v[k] = x; } });
        return tex;
      };
      g.__rt.canvasCopySwap = true; // tests/phaser
    }
  }

  // 7. Display list and container membership: Phaser checks membership by scanning the scene's
  //    display list (thousands of objects in PokeRogue) when an object is added or moved into a
  //    container, and removes with indexOf plus a JS shifting loop: quadratic during UI setup
  //    (indexOf was 11% of the setup frame). An object's displayList / parentContainer field
  //    already says where it is, so the scene's DisplayList and exclusive Containers use that.
  const DL = Phaser.GameObjects.DisplayList.prototype;
  const origExists = DL.exists, origDLAdd = DL.add, origDLRemove = DL.remove;
  DL.exists = function (child) {
    if (!enabled || !child || typeof child !== "object" || Array.isArray(child)) return origExists.call(this, child);
    if (child.displayList === this) return true;
    this._pkrFresh = child; // not here: the add() that usually follows can skip its own scan
    return false;
  };
  DL.add = function (child, skipCallback) {
    if (this._pkrFresh === child && child && !Array.isArray(child)) {
      this._pkrFresh = null;
      this.list.push(child);
      if (!skipCallback && this.addCallback) this.addCallback.call(this, child);
      return child;
    }
    this._pkrFresh = null;
    return origDLAdd.apply(this, arguments);
  };
  DL.remove = function (child, skipCallback) {
    if (!enabled || !child || Array.isArray(child)) return origDLRemove.apply(this, arguments);
    const i = this.list.lastIndexOf(child); // recently added objects are near the end
    if (i === -1) return null;
    this.list.splice(i, 1);
    if (!skipCallback && this.removeCallback) this.removeCallback.call(this, child);
    return child;
  };
  const CP2 = Phaser.GameObjects.Container.prototype, origContainerAdd = CP2.add;
  CP2.add = function (child) {
    // Exclusive containers set child.parentContainer; a child that isn't ours isn't in the list.
    if (enabled && this.exclusive && child && !(this.maxSize > 0)) {
      if (!Array.isArray(child)) {
        if (child.parentContainer !== this) {
          this.list.push(child);
          this.addHandler(child);
          return this;
        }
      } else {
        // Phaser's Add drops (and splices out of the caller's array) children already in the
        // list; only take the shortcut when there are none.
        let fresh = true;
        for (let i = 0; i < child.length; i++) if (!child[i] || child[i].parentContainer === this) { fresh = false; break; }
        if (fresh) {
          for (let i = 0; i < child.length; i++) {
            this.list.push(child[i]);
            this.addHandler(child[i]);
          }
          return this;
        }
      }
    }
    return origContainerAdd.apply(this, arguments);
  };
  // 7b. Container.removeHandler (Phaser 3.90) unsubscribes the child's DESTROY event from
  //     this.remove, but addHandler subscribed this.onChildDestroyed: every child taken out without
  //     being destroyed keeps a listener, and one added back gets another. The starter select
  //     screen's instructions take their icons out and add them back on every cursor move (log225:
  //     ~1,200 listeners per icon after 16 minutes there); each removal then scans and copies the
  //     whole list (eventemitter3 rebuilds it), and the moves took 0.5-1 s on the Vita. The listener
  //     addHandler made is now taken off with the child, as later Phaser does. The stale ones only
  //     ever ran when the child was destroyed: onChildDestroyed once per stale listener, removing
  //     a child that was no longer in the list.
  {
    const GOE = Phaser.GameObjects.Events;
    const origRemoveHandler = CP2.removeHandler, addSrc = String(CP2.addHandler), removeSrc = String(origRemoveHandler);
    if (GOE && GOE.DESTROY && /\.once\(\w+\.DESTROY,this\.onChildDestroyed,this\)/.test(addSrc) &&
        /\.off\(\w+\.DESTROY,this\.remove,this\)/.test(removeSrc)) {
      CP2.removeHandler = function (child) {
        if (enabled && child && typeof child.off === "function") child.off(GOE.DESTROY, this.onChildDestroyed, this);
        return origRemoveHandler.call(this, child);
      };
      g.__rt.containerListenerFix = true; // tests/phaser
    }
  }

  // 8. Utils.Array.Remove / RemoveAt (Container.remove, Structs.List): Phaser removes the element with
  //    SpliceOne, a JS loop shifting the rest of the array down. Destroying a big container's
  //    children one by one is quadratic in that loop (15% of the 50 s return to the title on the
  //    Vita). Native splice does the same shift in C. Callers look these up on the shared Utils.Array
  //    object at call time, so replacing them there is enough.
  const AU = Phaser.Utils.Array, origArrRemove = AU.Remove, origRemoveAt = AU.RemoveAt;
  AU.Remove = function (array, item, callback, context) {
    if (Array.isArray(item)) return origArrRemove.apply(this, arguments);
    const i = array.indexOf(item);
    if (i === -1) return null;
    array.splice(i, 1);
    if (callback) callback.call(context === undefined ? array : context, item);
    return item;
  };
  AU.RemoveAt = function (array, index, callback, context) {
    if (!(index >= 0 && index < array.length)) return origRemoveAt.apply(this, arguments); // throws
    const item = array.splice(index, 1)[0];
    if (callback) callback.call(context === undefined ? array : context, item);
    return item;
  };

  //    The WebGL renderer holds its own reference to Remove, and its wrapper lists are long (one
  //    entry per texture: 2,000+), so deleting the textures of destroyed texts and canvases goes
  //    through SpliceOne too. Those lists are only walked as a whole (context restore), so the order
  //    doesn't matter: swap with the last entry. The framebuffer stack keeps its order.
  const RP = Phaser.Renderer.WebGL.WebGLRenderer.prototype;
  const unordered = (array, item) => {
    const i = array.indexOf(item);
    if (i === -1) return;
    const last = array.pop();
    if (i < array.length) array[i] = last;
  };
  const ordered = (array, item) => {
    const i = array.indexOf(item);
    if (i !== -1) array.splice(i, 1);
  };
  RP.deleteTexture = function (t) {
    if (!t) return;
    unordered(this.glTextureWrappers, t);
    t.destroy();
    return this;
  };
  RP.deleteFramebuffer = function (f) {
    if (!f) return this;
    ordered(this.fboStack, f);
    unordered(this.glFramebufferWrappers, f);
    f.destroy();
    return this;
  };
  for (const [name, list] of [["deleteProgram", "glProgramWrappers"], ["deleteAttribLocation", "glAttribLocationWrappers"],
                              ["deleteUniformLocation", "glUniformLocationWrappers"], ["deleteBuffer", "glBufferWrappers"]]) {
    RP[name] = function (w) {
      if (w) {
        unordered(this[list], w);
        w.destroy();
      }
      return this;
    };
  }

  // 9. AnimationManager "remove" listeners: every Sprite's AnimationState subscribes to the global
  //    AnimationManager's REMOVE_ANIMATION event and unsubscribes when destroyed. With 1,200+ sprites
  //    that is one listener array, and the event emitter's removeListener rebuilds the whole array
  //    on every removal: destroying the sprites is quadratic (25% of the return to the title).
  //    Those subscriptions go into an insertion-ordered Set behind one real listener instead.
  const AM = Phaser.Animations.AnimationManager.prototype;
  const REMOVE_ANIM = Phaser.Animations.Events.REMOVE_ANIMATION;
  const globalRemove = Phaser.Animations.AnimationState.prototype.globalRemove;
  const origAMOn = AM.on, origAMOff = AM.off;
  AM.on = function (event, fn, context) {
    if (event !== REMOVE_ANIM || fn !== globalRemove || !context) return origAMOn.apply(this, arguments);
    let states = this._pkrAnimStates;
    if (!states) {
      states = this._pkrAnimStates = new Set();
      origAMOn.call(this, REMOVE_ANIM, function (key, anim) {
        for (const st of states) st.globalRemove(key, anim);
      });
    }
    states.add(context);
    return this;
  };
  AM.off = AM.removeListener = function (event, fn, context) {
    // Subscriptions made before this patch are still in the emitter's own array.
    if (event === REMOVE_ANIM && fn === globalRemove && context && this._pkrAnimStates &&
        this._pkrAnimStates.delete(context)) return this;
    return origAMOff.apply(this, arguments);
  };

  // 10. Container.renderWebGL in native code (src/fastpath.c renderContainer): the whole container
  //     tree is walked in C, plain sprites batched there; everything else calls back into JS.
  //     localStorage PKR_NATIVE_RENDER=0 keeps the JS version (fastContainerRender above).
  const nrc = g.__native.renderContainer;
  let useNative = false;
  try { useNative = !!nrc && g.localStorage.getItem("PKR_NATIVE_RENDER") !== "0"; } catch (e) { useNative = !!nrc; }
  if (useNative) {
    const WP = Phaser.Renderer.WebGL.WebGLPipeline.prototype, N = g.__native;
    // WebGLPipeline.flush / pushBatch and MultiPipeline.batchQuad in C (src/fastpath.c). The C flush
    // skips emitting its events when eventemitter3 has no listener under the event's name, so only
    // if listeners are kept under plain names. localStorage PKR_NATIVE_BATCH=0 keeps Phaser's.
    const PEv = Phaser.Renderer.WebGL.Pipelines && Phaser.Renderer.WebGL.Pipelines.Events;
    let plainEvents = false;
    try { const ee = new Phaser.Events.EventEmitter(); ee.on("x", () => {}); plainEvents = !!(ee._events && ee._events.x); } catch (e) { /* no */ }
    let nativeBatching = !!(N.pipelineFlush && PEv && PEv.BEFORE_FLUSH && PEv.AFTER_FLUSH && plainEvents);
    try { if (g.localStorage.getItem("PKR_NATIVE_BATCH") === "0") nativeBatching = false; } catch (e) { /* keep */ }
    if (nativeBatching) {
      g.__rt.origPipeline = { flush: WP.flush, pushBatch: WP.pushBatch, batchQuad: origQuad };
      g.__rt.nativePipeline = { flush: N.pipelineFlush, pushBatch: N.pipelinePushBatch, batchQuad: N.makeBatchQuad(origQuad) };
      WP.flush = g.__rt.nativePipeline.flush;
      WP.pushBatch = g.__rt.nativePipeline.pushBatch;
      MP.batchQuad = g.__rt.nativePipeline.batchQuad;
    }
    const alphaSingle = Object.getOwnPropertyDescriptor(Phaser.GameObjects.Components.AlphaSingle, "alpha");
    N.renderSetup({
      imageRender, spriteRender, containerRender: nrc, mpBatchSprite: fastBatchSprite, mpBatchQuad: MP.batchQuad,
      goWillRender: Phaser.GameObjects.GameObject.prototype.willRender, containerWillRender: Container.willRender,
      wpOnBind: WP.onBind, wpOnBatch: WP.onBatch, wpSetGameObject: WP.setGameObject,
      camAddToRenderList: Phaser.Cameras.Scene2D.BaseCamera.prototype.addToRenderList, textRender,
      textLazySize: lazySize, trimInfo,
      // Recognised on objects and done in C: the setAlpha/setScrollFactor round trip of generic
      // children, and MultiPipeline.batchSprite for sprites on pipelines with their own batchQuad.
      alphaSetAlpha: Phaser.GameObjects.Components.Alpha.setAlpha,
      alphaSingleSetAlpha: Phaser.GameObjects.Components.AlphaSingle.setAlpha,
      setScrollFactor: Phaser.GameObjects.Components.ScrollFactor.setScrollFactor,
      mpOrigBatchSprite: origSprite,
      pmPreBatch: Phaser.Renderer.WebGL.PipelineManager.prototype.preBatch,
      pmPostBatch: Phaser.Renderer.WebGL.PipelineManager.prototype.postBatch,
      containerSetScrollFactor: Container.setScrollFactor,
      alphaSingleSetter: alphaSingle && alphaSingle.value && alphaSingle.value.set,
      wpPushBatch: WP.pushBatch, wpCreateBatch: WP.createBatch, wpSetVertexBuffer: WP.setVertexBuffer,
      wpOnBeforeFlush: WP.onBeforeFlush, wpOnAfterFlush: WP.onAfterFlush,
      evBeforeFlush: PEv ? PEv.BEFORE_FLUSH : "", evAfterFlush: PEv ? PEv.AFTER_FLUSH : "",
    });
  }
  const containerRender = useNative ? nrc : fastContainerRender;
  g.__rt.jsContainerRender = fastContainerRender; // for checking the native one (scripts/probe_verify_render.js)

  // 11. TextStyle.setStyle: every Text builds a TextStyle, which reads 26 settings through
  //     GetAdvancedValue -> GetValue (two generic calls each, dotted paths split every time) and
  //     measures the font's ascent/descent on a pooled canvas. Same logic with the paths
  //     pre-split, and the metrics remembered per font (they depend only on the font string and
  //     the test string; cleared when a font finishes loading).
  const TSP = Phaser.GameObjects.TextStyle && Phaser.GameObjects.TextStyle.prototype;
  // Setting -> path in the style config, as in Phaser 3.90's TextStyle propertyMap. The defaults
  // are read from a TextStyle built by Phaser's own code (so they match this build exactly).
  const STYLE_PATHS = {
    fontFamily: "fontFamily", fontSize: "fontSize", fontStyle: "fontStyle", backgroundColor: "backgroundColor",
    color: "color", stroke: "stroke", strokeThickness: "strokeThickness", shadowOffsetX: "shadow.offsetX",
    shadowOffsetY: "shadow.offsetY", shadowColor: "shadow.color", shadowBlur: "shadow.blur", shadowStroke: "shadow.stroke",
    shadowFill: "shadow.fill", align: "align", maxLines: "maxLines", fixedWidth: "fixedWidth", fixedHeight: "fixedHeight",
    resolution: "resolution", rtl: "rtl", testString: "testString", baselineX: "baselineX", baselineY: "baselineY",
    wordWrapWidth: "wordWrap.width", wordWrapCallback: "wordWrap.callback", wordWrapCallbackScope: "wordWrap.callbackScope",
    wordWrapUseAdvanced: "wordWrap.useAdvancedWrap",
  };
  let STYLE_MAP = null;
  if (TSP && typeof TSP.toJSON === "function") {
    try {
      const probe = new Phaser.GameObjects.TextStyle({}, {});
      const keys = Object.keys(probe.toJSON()).filter((k) => k !== "metrics");
      if (keys.length === Object.keys(STYLE_PATHS).length && keys.every((k, i) => Object.keys(STYLE_PATHS)[i] === k)) {
        STYLE_MAP = keys.map((k) => {
          const path = STYLE_PATHS[k];
          const parts = path.includes(".") ? path.split(".") : null;
          return { k, path, parts, top: parts ? parts[0] : path, def: probe[k],
                   raw: k === "wordWrapCallback" || k === "wordWrapCallbackScope" };
        });
      }
    } catch (e) { STYLE_MAP = null; }
  }
  // Phaser's GetValue(source, path, def) for a dotted or plain path.
  function getValue(e, path, parts, def) {
    if (!e || typeof e === "number") return def;
    if (e.hasOwnProperty(path)) return e[path];
    if (!parts) return def;
    let a = e;
    for (let i = 0; i < parts.length; i++) {
      if (a && a.hasOwnProperty(parts[i])) a = a[parts[i]];
      else return def;
    }
    return a;
  }
  const RND = Phaser.Math.RND;
  // Phaser's GetAdvancedValue.
  function getAdvanced(e, path, parts, def) {
    const a = getValue(e, path, parts, null);
    if (a === null) return def;
    if (Array.isArray(a)) return RND.pick(a);
    if (typeof a === "object") {
      if (a.hasOwnProperty("randInt")) return RND.integerInRange(a.randInt[0], a.randInt[1]);
      if (a.hasOwnProperty("randFloat")) return RND.realInRange(a.randFloat[0], a.randFloat[1]);
    } else if (typeof a === "function") return a(path);
    return a;
  }
  const metricsCache = new Map();
  let metricsGen = -1;
  // MeasureText(style) is internal to Phaser; TextStyle.update(true) runs it (and then the parent's
  // updateText, switched off here).
  function measureFont(style) {
    if (metricsGen !== (g.__rt.fontGeneration | 0)) { metricsCache.clear(); metricsGen = g.__rt.fontGeneration | 0; }
    const key = style._font + "\u0001" + style.testString;
    const hit = metricsCache.get(key);
    if (hit) return { ascent: hit.ascent, descent: hit.descent, fontSize: hit.fontSize };
    const p = style.parent, own = Object.prototype.hasOwnProperty.call(p, "updateText"), saved = p.updateText;
    p.updateText = noop;
    try { style.update(true); } finally { if (own) p.updateText = saved; else delete p.updateText; }
    const m = style.metrics;
    metricsCache.set(key, { ascent: m.ascent, descent: m.descent, fontSize: m.fontSize });
    return m;
  }
  if (STYLE_MAP && typeof TSP.update === "function" && typeof TSP.setFont === "function") {
    const origSetStyle = TSP.setStyle;
    TSP.setStyle = function (e, t, n) {
      if (!enabled) return origSetStyle.apply(this, arguments);
      if (t === undefined) t = true;
      if (n === undefined) n = false;
      // A key the config doesn't have gives the default (or the current value): Phaser's GetValue
      // only reads own properties. PokeRogue's configs have a few of the ~30 keys.
      const own = e && (typeof e === "object" || typeof e === "function") ? new Set(Object.getOwnPropertyNames(e)) : null;
      for (let i = 0; i < STYLE_MAP.length; i++) {
        const s = STYLE_MAP[i], c = n ? s.def : this[s.k];
        if (s.k !== "fontSize" && (!own || (!own.has(s.top) && !own.has(s.path)))) this[s.k] = c;
        else if (s.raw) this[s.k] = getValue(e, s.path, s.parts, c);
        else if (e && s.k === "fontSize" && typeof e.fontSize === "number") this[s.k] = e.fontSize.toString() + "px";
        else this[s.k] = getAdvanced(e, s.path, s.parts, c);
      }
      const font = getValue(e, "font", null, null);
      if (font !== null) this.setFont(font, false);
      this._font = [this.fontStyle, this.fontSize, this.fontFamily].join(" ").trim();
      const fill = getValue(e, "fill", null, null);
      if (fill !== null) this.color = fill;
      const d = getValue(e, "metrics", null, false);
      if (d) this.metrics = { ascent: getValue(d, "ascent", null, 0), descent: getValue(d, "descent", null, 0), fontSize: getValue(d, "fontSize", null, 0) };
      else if (t || !this.metrics) this.metrics = measureFont(this);
      return t ? this.parent.updateText() : this.parent;
    };
    g.__rt.origSetStyle = origSetStyle; // for scripts/probe_textstyle.js
  }

  // 12. UUID: see replaceWrap in src/fastpath.c. Only if Phaser's UUID still uses that template.
  if (g.__native.replaceWrap && /xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx/.test(String(Phaser.Utils.String.UUID))) {
    const wrapped = g.__native.replaceWrap(String.prototype.replace);
    Object.defineProperty(String.prototype, "replace", { value: wrapped, writable: true, configurable: true, enumerable: false });
    g.__rt.uuidWrap = true;
  }

  // 13. WebGLShader.setUniform1..4: Phaser's code in C (src/fastpath.c js_set_uniform).
  //     localStorage PKR_NATIVE_UNIFORMS=0 keeps Phaser's.
  const WS = Phaser.Renderer.WebGL.WebGLShader && Phaser.Renderer.WebGL.WebGLShader.prototype;
  let nativeUniforms = !!(WS && g.__native.setUniform1);
  try { if (g.localStorage.getItem("PKR_NATIVE_UNIFORMS") === "0") nativeUniforms = false; } catch (e) { /* keep */ }
  if (nativeUniforms && /this\.uniforms\[/.test(String(WS.setUniform4))) {
    g.__rt.origSetUniform = [WS.setUniform1, WS.setUniform2, WS.setUniform3, WS.setUniform4];
    WS.setUniform1 = g.__native.setUniform1;
    WS.setUniform2 = g.__native.setUniform2;
    WS.setUniform3 = g.__native.setUniform3;
    WS.setUniform4 = g.__native.setUniform4;
  }

  // 14. Pipeline switching and the uniform wrappers in C (src/fastpath.c, "pipeline machinery"):
  //     PipelineManager.set / isCurrent, WebGLPipeline.bind / updateProjectionMatrix / set1f...,
  //     WebGLShader.bind / setAttribPointers / set1f..., WebGLRenderer.setProgram / setBlendMode.
  //     Only for Phaser versions whose code still looks like 3.90's (checked below).
  //     localStorage PKR_NATIVE_PIPELINE=0 keeps Phaser's; __rt.pipelineSwaps lists [object, name,
  //     Phaser's, native] for scripts/probe_verify_pipeline.js.
  const WR = Phaser.Renderer.WebGL.WebGLRenderer.prototype, PMP = Phaser.Renderer.WebGL.PipelineManager.prototype;
  const WPP = Phaser.Renderer.WebGL.WebGLPipeline.prototype;
  const PEvents = Phaser.Renderer.WebGL.Pipelines && Phaser.Renderer.WebGL.Pipelines.Events;
  let nativePipeline = !!(g.__native.pipelineSetup && WS && PEvents && PEvents.BIND);
  try { if (g.localStorage.getItem("PKR_NATIVE_PIPELINE") === "0") nativePipeline = false; } catch (e) { /* keep */ }
  const looks = [
    [WR.setProgram, /currentProgram[\s\S]*useProgram/], [WR.setBlendMode, /SKIP_CHECK[\s\S]*currentBlendMode[\s\S]*blendFuncSeparate/],
    [WR.flush, /pipelines\.flush\(\)/], [PMP.flush, /current\.flush\(\)/],
    [PMP.set, /isPostFX[\s\S]*isCurrent[\s\S]*unbind[\s\S]*\.bind\([\s\S]*updateProjectionMatrix[\s\S]*onBind/],
    [PMP.isCurrent, /currentShader[\s\S]*currentProgram/],
    [WPP.bind, /glReset[\s\S]*ARRAY_BUFFER_BINDING[\s\S]*activeTextures[\s\S]*onActive/],
    [WPP.updateProjectionMatrix, /projectionWidth[\s\S]*setProjectionMatrix/], [WPP.unbind, /currentRenderTarget/],
    [WS.bind, /setProgram[\s\S]*setAttribPointers/],
    [WS.setAttribPointers, /createAttribLocation[\s\S]*vertexAttribPointer[\s\S]*disableVertexAttribArray/],
  ];
  if (nativePipeline && looks.every(([f, re]) => typeof f === "function" && re.test(String(f)))) {
    const n = g.__native.pipelineSetup({ rendererFlush: WR.flush, pmFlush: PMP.flush, wpUnbind: WPP.unbind,
      wpOnActive: WPP.onActive, wsSetAttribPointers: WS.setAttribPointers, evBind: PEvents.BIND });
    const swaps = [[WR, "setProgram", n.setProgram], [WR, "setBlendMode", n.setBlendMode], [PMP, "set", n.pmSet],
      [PMP, "isCurrent", n.pmIsCurrent], [WPP, "bind", n.wpBind], [WPP, "updateProjectionMatrix", n.wpUpdateProjectionMatrix],
      [WS, "bind", n.wsBind], [WS, "setAttribPointers", n.wsSetAttribPointers]];
    // set1f(name, x) { return this.setUniform1(this.gl.uniform1f, name, x) } and the pipeline's
    // set1f(name, x, shader) { shader ||= this.currentShader; shader.set1f(name, x); return this }
    for (const k of Object.keys(n.shader)) {
      const gs = String(WS[k]), ps = String(WPP[k]);
      if (/setUniform[1-4]\(/.test(gs) && /\.uniform\w+/.test(gs)) swaps.push([WS, k, n.shader[k]]);
      if (/currentShader/.test(ps) && ps.includes("." + k + "(")) swaps.push([WPP, k, n.pipeline[k]]);
    }
    g.__rt.pipelineSwaps = swaps.map(([o, k, f]) => [o, k, o[k], f]);
    for (const [o, k, , f] of g.__rt.pipelineSwaps) o[k] = f;
  }

  // 15. Graphics.renderWebGL in C for command buffers of fill/line styles and fillRect (PokeRogue's
  //     masks and bars; src/fastpath.c "Graphics"), Phaser's renderer for anything else. Only if
  //     Graphics still writes Phaser 3.90's command numbers and the renderer looks like 3.90's.
  //     localStorage PKR_NATIVE_GRAPHICS=0 keeps Phaser's; __rt.graphicsSwap for the probes.
  const GP = Phaser.GameObjects.Graphics && Phaser.GameObjects.Graphics.prototype;
  let nativeGraphics = !!(GP && g.__native.graphicsSetup && MP.batchFillRect);
  try { if (g.localStorage.getItem("PKR_NATIVE_GRAPHICS") === "0") nativeGraphics = false; } catch (e) { /* keep */ }
  if (nativeGraphics) {
    const fake = { commandBuffer: [] };
    try {
      GP.beginPath.call(fake); GP.fillStyle.call(fake, 0x123456, 0.5); GP.fillRect.call(fake, 1, 2, 3, 4);
      GP.lineStyle.call(fake, 2, 0x654321, 0.25); GP.fillGradientStyle.call(fake, 1, 2, 3, 4, 0.1, 0.2, 0.3, 0.4);
      GP.lineGradientStyle.call(fake, 3, 5, 6, 7, 8, 0.6); GP.closePath.call(fake);
    } catch (e) { fake.commandBuffer = null; }
    const want = [1, 7, 0x123456, 0.5, 3, 1, 2, 3, 4, 6, 2, 0x654321, 0.25, 21, 0.1, 0.2, 0.3, 0.4, 1, 2, 3, 4, 22, 3, 0.6, 5, 6, 7, 8, 2];
    const rs = String(GP.renderWebGL), fr = String(MP.batchFillRect);
    nativeGraphics = !!fake.commandBuffer && fake.commandBuffer.join() === want.join() &&
      /commandBuffer[\s\S]*getTintAppendFloatAlpha[\s\S]*FILL_RECT[\s\S]*batchFillRect/.test(rs) &&
      /pipelines\.set\(this\)[\s\S]*multiply[\s\S]*setQuad[\s\S]*batchQuad\(null/.test(fr);
  }
  if (nativeGraphics) {
    const nat = g.__native.graphicsSetup({ render: GP.renderWebGL, batchFillRect: MP.batchFillRect });
    g.__rt.graphicsSwap = [[GP, "renderWebGL", GP.renderWebGL, nat]];
    GP.renderWebGL = nat;
  }

  // 16. Rectangle.renderWebGL (fill only) and GeometryMask's stencil handling in C (src/fastpath.c
  //     "Rectangle, GeometryMask"), if their code still looks like Phaser 3.90's.
  //     localStorage PKR_NATIVE_SHAPES=0 keeps Phaser's; __rt.shapesSwap for the probes.
  const RP2 = Phaser.GameObjects.Rectangle && Phaser.GameObjects.Rectangle.prototype;
  const GMP = Phaser.Display.Masks.GeometryMask && Phaser.Display.Masks.GeometryMask.prototype;
  let nativeShapes = !!(RP2 && GMP && g.__native.shapesSetup && MP.batchFillRect);
  try { if (g.localStorage.getItem("PKR_NATIVE_SHAPES") === "0") nativeShapes = false; } catch (e) { /* keep */ }
  if (nativeShapes) {
    const ok = (f, re) => typeof f === "function" && re.test(String(f));
    nativeShapes = ok(RP2.renderWebGL, /isRounded[\s\S]*isFilled[\s\S]*fillTint[\s\S]*batchFillRect[\s\S]*isStroked/) &&
      ok(GMP.preRenderWebGL, /STENCIL_TEST[\s\S]*maskCount[\s\S]*currentCameraMask[\s\S]*maskStack\.push[\s\S]*applyStencil/) &&
      ok(GMP.applyStencil, /colorMask[\s\S]*INCR[\s\S]*DECR[\s\S]*renderWebGL[\s\S]*invertAlpha/) &&
      ok(GMP.postRenderWebGL, /maskStack\.pop[\s\S]*STENCIL_TEST[\s\S]*applyStencil/);
  }
  if (nativeShapes) {
    const n = g.__native.shapesSetup({ rectRender: RP2.renderWebGL });
    g.__rt.shapesSwap = [[RP2, "renderWebGL", n.rectRender], [GMP, "preRenderWebGL", n.gmPreRender],
      [GMP, "postRenderWebGL", n.gmPostRender], [GMP, "applyStencil", n.gmApplyStencil]].map(([o, k, f]) => [o, k, o[k], f]);
    for (const [o, k, , f] of g.__rt.shapesSwap) o[k] = f;
  }

  // 17. eventemitter3's emit in C (src/fastpath.c "EventEmitter.emit"), skipping listeners that are
  //     WebGLPipeline's empty onPreRender/onRender/onPostRender. Only if emit is still eventemitter3's
  //     and events are stored under plain names. localStorage PKR_NATIVE_EMIT=0 keeps the JS one;
  //     __rt.emitSwap for the probes.
  {
    let EEP = Phaser.Events.EventEmitter.prototype;
    while (EEP && !Object.prototype.hasOwnProperty.call(EEP, "emit")) EEP = Object.getPrototypeOf(EEP);
    let plain = false;
    try { const ee = new Phaser.Events.EventEmitter(); ee.on("x", () => {}); plain = !!(ee._events && ee._events.x); } catch (e) { /* no */ }
    const src = EEP ? String(EEP.emit) : "";
    let nativeEmit = !!(g.__native.emitSetup && plain &&
      /^function\s*(emit)?\s*\(\s*\w+(\s*,\s*\w+){5}\s*\)\s*\{\s*var \w+\s*=\s*\w+\s*\?\s*\w+\s*\+\s*\w+\s*:\s*\w+;\s*if\s*\(\s*!this\._events\[\w+\]\s*\)\s*return/.test(src) &&
      /removeListener\(\w+,\s*\w+(\[\w+\])?\.fn,\s*(void 0|undefined),\s*(!0|true)\)/.test(src));
    try { if (g.localStorage.getItem("PKR_NATIVE_EMIT") === "0") nativeEmit = false; } catch (e) { /* keep */ }
    if (nativeEmit) {
      const empty = (f) => typeof f === "function" && /^function\s*\w*\s*\(\s*\)\s*\{\s*\}$/.test(String(f));
      const noops = ["onPreRender", "onRender", "onPostRender", "onResize"].map((k) => WPP[k]).filter(empty);
      const nat = g.__native.emitSetup(noops);
      g.__rt.emitSwap = [[EEP, "emit", EEP.emit, nat]];
      EEP.emit = nat;
    }
  }

  // 18. BitmapText.renderWebGL (the battle UI's numbers, item counts...): Phaser 3.90's
  //     BitmapTextWebGLRenderer with the character loop (BatchChar and MultiPipeline.batchQuad, ~30
  //     JS operations and a 20-argument call per character) in C (__native.batchChars), flushing
  //     before the same characters as Phaser does. 15-22% of a battle frame's JS on the Vita (0.76-
  //     0.78, logs 93, 102, 108). Phaser's own renderer for per-character colours and for pipelines
  //     with their own batchQuad, onBatch or shouldFlush. localStorage PKR_NATIVE_BITMAPTEXT=0: off.
  {
    const BTP = Phaser.GameObjects.BitmapText && Phaser.GameObjects.BitmapText.prototype;
    const origBT = BTP && BTP.renderWebGL, nbc = g.__native.batchChars;
    const src = origBT ? String(origBT) : "";
    let on = !!(nbc && GetCalcMatrix && WGLUtils && /getTextBounds\(\s*(!1|false)\s*\)/.test(src) &&
      /dropShadowX/.test(src) && /charColors/.test(src) && /shouldFlush\(\s*6\s*\)/.test(src));
    try { if (g.localStorage.getItem("PKR_NATIVE_BITMAPTEXT") === "0") on = false; } catch (e) { /* keep */ }
    const st = new Int32Array(1);
    // The characters from chars[0..] at `unit`; after a flush, the next character gets `nextUnit()`
    // (Phaser's loop: setGameObject) and, with `once`, the ones after it `unit` again (BatchChar's
    // stale textureUnit in Phaser's drop shadow loop, where batchQuad flushes by itself).
    const writeChars = (pipeline, chars, calc, src, dx, dy, round, unit, effect, tl, tr, bl, br, nextUnit, once) => {
      st[0] = 0;
      let u = unit;
      for (;;) {
        const room = Math.max(0, Math.floor((pipeline.vertexCapacity - pipeline.vertexCount) / 6));
        const q = nbc(pipeline.vertexData, pipeline.vertexCount * 7, room, chars, st, calc.matrix, src.displayOriginX,
          src.displayOriginY, dx, dy, round, u, effect, tl, tr, bl, br);
        if (q) {
          pipeline.vertexCount += 6 * q;
          pipeline.currentBatch.count = pipeline.vertexCount - pipeline.currentBatch.start;
        }
        if (st[0] >= chars.length) return u;
        pipeline.flush();
        u = nextUnit();
        if (once) { // that one character with the new unit, then the old one again
          const q1 = nbc(pipeline.vertexData, pipeline.vertexCount * 7, 1, chars, st, calc.matrix, src.displayOriginX,
            src.displayOriginY, dx, dy, round, u, effect, tl, tr, bl, br);
          pipeline.vertexCount += 6 * q1;
          pipeline.currentBatch.count = pipeline.vertexCount - pipeline.currentBatch.start;
          u = unit;
          if (st[0] >= chars.length) return u;
        }
      }
    };
    function bitmapTextRender(renderer, src, camera, parentMatrix) {
      const text = src._text;
      if (text.length === 0) return;
      const sp = src.pipeline;
      if (!enabled || !sp || sp.batchQuad !== MP.batchQuad || sp.onBatch !== WPP.onBatch || sp.shouldFlush !== WPP.shouldFlush ||
          !sp.currentShader || sp.currentShader.vertexComponentCount !== 7 || src.charColors.length)
        return origBT.apply(this, arguments);
      camera.addToRenderList(src);
      const pipeline = renderer.pipelines.set(sp, src);
      const calc = GetCalcMatrix(src, camera, parentMatrix).calc;
      renderer.pipelines.preBatch(src);
      const round = camera.roundPixels, cameraAlpha = camera.alpha, getTint = WGLUtils.getTintAppendFloatAlpha;
      const tintTL = getTint(src.tintTopLeft, cameraAlpha * src._alphaTL), tintTR = getTint(src.tintTopRight, cameraAlpha * src._alphaTR);
      const tintBL = getTint(src.tintBottomLeft, cameraAlpha * src._alphaBL), tintBR = getTint(src.tintBottomRight, cameraAlpha * src._alphaBR);
      const texture = src.frame.glTexture;
      let textureUnit = pipeline.setGameObject(src);
      const chars = src.getTextBounds(false).characters;
      const dropShadowX = src.dropShadowX, dropShadowY = src.dropShadowY;
      if (dropShadowX !== 0 || dropShadowY !== 0) {
        const c = src.dropShadowColor, a = src.dropShadowAlpha;
        writeChars(pipeline, chars, calc, src, dropShadowX, dropShadowY, round, textureUnit, 1,
          getTint(c, cameraAlpha * a * src._alphaTL), getTint(c, cameraAlpha * a * src._alphaTR),
          getTint(c, cameraAlpha * a * src._alphaBL), getTint(c, cameraAlpha * a * src._alphaBR),
          () => pipeline.setTexture2D(texture), true);
      }
      writeChars(pipeline, chars, calc, src, 0, 0, round, textureUnit, src.tintFill, tintTL, tintTR, tintBL, tintBR,
        () => (textureUnit = pipeline.setGameObject(src)), false);
      renderer.pipelines.postBatch(src);
    }
    if (on) {
      g.__rt.bitmapTextSwap = [[BTP, "renderWebGL", origBT, bitmapTextRender]];
      BTP.renderWebGL = bitmapTextRender;
      // Containers (src/fastpath.c renderContainer) draw such texts themselves, the per-text JS above included.
      if (g.__native.bitmapTextSetup) g.__native.bitmapTextSetup(bitmapTextRender, WPP.shouldFlush, BTP.getTextBounds);
    }
  }

  // 19. Tween.update and the common case of TweenData.update in C (src/fastpath.c "tweens"): a running
  //     tween's data playing forward or backward, the ease (Linear, Sine, Cubic computed there,
  //     others called), the target's property, onUpdate only when someone listens. Starting,
  //     finishing, repeats, yoyos, holds and delays stay in Phaser's code. 13-27% of a battle frame's
  //     JS on the Vita was tweens, most of it Phaser's stepping (0.83, log118). Only if Phaser's
  //     tween code looks like 3.90's. localStorage PKR_NATIVE_TWEENS=0: off.
  {
    const TW = Phaser.Tweens && Phaser.Tweens.Tween, TD = Phaser.Tweens && Phaser.Tweens.TweenData;
    const E = Phaser.Math && Phaser.Math.Easing, Ev = Phaser.Tweens && Phaser.Tweens.Events;
    const tu = TW && String(TW.prototype.update), du = TD && String(TD.prototype.update), de = TD && String(TD.prototype.dispatchEvent);
    let on = !!(g.__native.tweenSetup && E && E.Sine && E.Cubic && Ev && Ev.TWEEN_UPDATE &&
      /isLoopDelayed\(\)/.test(tu) && /isCompleteDelayed\(\)/.test(tu) && /hasStarted/.test(tu) && /totalProgress/.test(tu) &&
      /\.update\(\w+\)/.test(tu) && /nextState\(\)/.test(tu) && /isPendingRender\(\)/.test(du) && /isNumberTween/.test(du) &&
      /interpolation/.test(du) && /setStateFromEnd/.test(du) && /onUpdate/.test(du) && /isSeeking/.test(de) && /callbacks\[/.test(de));
    try { if (g.localStorage.getItem("PKR_NATIVE_TWEENS") === "0") on = false; } catch (e) { /* keep */ }
    if (on) {
      const orig = TW.prototype.update;
      const nat = g.__native.tweenSetup(orig, [E.Linear, E.Sine.In, E.Sine.Out, E.Sine.InOut, E.Cubic.In, E.Cubic.Out, E.Cubic.InOut],
        Ev.TWEEN_UPDATE, TD.prototype.update);
      g.__rt.tweenSwap = [[TW.prototype, "update", orig, nat]];
      TW.prototype.update = nat;
    }
  }

  // 20. Covered objects: PokeRogue's menus (party, summary, starter select, egg gacha and list...)
  //     are screen-filling opaque images over the battle, which goes on being drawn underneath (the
  //     arena and the Pokémon with their shaders): GPU work for pixels the menu then paints over.
  //     When a top-level child of the scene holds an image that covers the whole camera view and
  //     hides what is under it (alpha 1 down its containers, normal blending, MultiPipeline, no
  //     mask, post pipeline, rotation or crop, every texel of its frame at alpha 255 by src/gl.c's
  //     map), the top-level children before that one aren't drawn. They still go in the camera's
  //     render list (input orders the objects under the pointer by it). The image is looked for
  //     every COVER_SEARCH frames (a menu's first frames are drawn in full) and checked again every
  //     frame; what can't change between searches (type, pipeline, frame) is judged once per object
  //     and frame (log171: at every 8 frames the search was ~5% of a battle's JS samples).
  //     localStorage PKR_COVER=0: off.
  const WRP = Phaser.Renderer.WebGL.WebGLRenderer.prototype, origRender = WRP.render;
  let coverOn = !!alphaBox && typeof origRender === "function" && origRender.length === 3;
  try { if (g.localStorage.getItem("PKR_COVER") === "0") coverOn = false; } catch (e) { /* keep */ }
  if (coverOn) {
    const COVER_SEARCH = 30;
    const coverStats = { frames: 0, skipped: 0 };
    g.__rt.coverStats = () => Object.assign({}, coverStats);
    const tmpA = new Phaser.GameObjects.Components.TransformMatrix(), tmpB = new Phaser.GameObjects.Components.TransformMatrix();
    const tmpRect = new Phaser.Geom.Rectangle();
    const frameOpaque = (frame) => {
      if (frame._pkrOpaque !== undefined) return frame._pkrOpaque;
      const tex = frame.source && frame.source.glTexture;
      if (!tex || !tex.webGLTexture) return false;
      const b = alphaBox(tex.webGLTexture, frame.cutX, frame.cutY, frame.cutWidth, frame.cutHeight);
      return (frame._pkrOpaque = !!(b && b[4]) && !frame.rotated);
    };
    // What doesn't change while an object keeps its frame and pipeline: kept on it (_pkrCov*).
    const coverKind = (o) => {
      const f = o.frame, p = o.pipeline;
      if (o._pkrCovF === f && o._pkrCovP === p) return o._pkrCovOk;
      const t = o.type;
      const ok = (t === "Image" || t === "Sprite" || t === "NineSlice") && !!p && p.name === "MultiPipeline" && !!f &&
        f.cutWidth >= 64 && f.cutHeight >= 64 && frameOpaque(f);
      // a frame whose texture isn't uploaded yet gets judged again
      if (ok || (f && f._pkrOpaque !== undefined) || t !== "Image" && t !== "Sprite" && t !== "NineSlice") {
        o._pkrCovF = f, o._pkrCovP = p, o._pkrCovOk = ok;
      }
      return ok;
    };
    // o hides everything drawn before it by this camera (see above).
    const covers = (o, camera) => {
      if (!coverKind(o) || !o.scene || !o.willRender(camera) || o.mask || o.hasPostPipeline || o.isCropped ||
          o.blendMode !== 0 || o.tintFill) return false;
      if (o.alphaTopLeft !== undefined ? !(o._alphaTL === 1 && o._alphaTR === 1 && o._alphaBL === 1 && o._alphaBR === 1) : o.alpha !== 1) return false;
      for (let p = o.parentContainer; p; p = p.parentContainer)
        if (!p.willRender(camera) || p.alpha !== 1 || (p.blendMode !== -1 && p.blendMode !== 0) || p.mask || p.hasPostPipeline) return false;
      const m = o.getWorldTransformMatrix(tmpA, tmpB);
      if (Math.abs(m.b) > 1e-9 || Math.abs(m.c) > 1e-9) return false;
      const b = o.getBounds(tmpRect), v = camera.worldView;
      // off whole pixels, pixel rounding may move it by up to one
      const tol = Number.isInteger(b.x) && Number.isInteger(b.y) ? 0 : 1;
      return b.x <= v.x - tol && b.y <= v.y - tol && b.x + b.width >= v.x + v.width + tol && b.y + b.height >= v.y + v.height + tol;
    };
    // The last cover in drawing order: the top of a reverse walk.
    const findCover = (list, camera) => {
      for (let i = list.length - 1; i >= 0; i--) {
        const o = list[i];
        if (!o.willRender(camera)) continue;
        if (o.type === "Container" && o.list) {
          const c = findCover(o.list, camera);
          if (c) return c;
        } else if (covers(o, camera)) return o;
      }
      return null;
    };
    // What Phaser's renderers add to the render list, for the children not drawn.
    const listSkipped = (list, camera) => {
      for (let i = 0; i < list.length; i++) {
        const o = list[i];
        if (!o.willRender(camera)) continue;
        camera.addToRenderList(o);
        if (o.type === "Container" && o.list) listSkipped(o.list, camera);
      }
    };
    // Only the input plugin reads the render list (sortGameObjects: which of the objects under a
    // pointer is on top, by their order in it), so of the children not drawn only the interactive
    // ones need to be there, in drawing order. Walking every skipped object each frame took ~5% of
    // a frame on the party screen (log189); the interactive ones are a handful.
    const listInteractive = (scene, children, k, camera) => {
      const inp = scene.sys.input, il = inp && inp._list;
      if (!Array.isArray(il)) {
        for (let i = 0; i < k; i++) {
          camera.addToRenderList(children[i]);
          if (children[i].type === "Container" && children[i].list) listSkipped(children[i].list, camera);
        }
        return;
      }
      let found = null;
      for (let j = 0; j < il.length; j++) {
        const o = il[j];
        if (!o.willRender(camera)) continue;
        const path = [];
        let top = o, ok = true;
        while (top.parentContainer) {
          const p = top.parentContainer;
          if (!p.willRender(camera)) { ok = false; break; }
          path.push(p.list.indexOf(top));
          top = p;
        }
        if (!ok) continue;
        const t = children.indexOf(top);
        if (t < 0 || t >= k) continue;
        path.push(t);
        (found || (found = [])).push([path.reverse(), o]);
      }
      if (!found) return;
      if (found.length > 1) found.sort((a, b) => {
        const p = a[0], q = b[0];
        for (let i = 0; i < p.length && i < q.length; i++) if (p[i] !== q[i]) return p[i] - q[i];
        return p.length - q.length; // a container before what it holds
      });
      for (let i = 0; i < found.length; i++) camera.addToRenderList(found[i][1]);
    };
    WRP.render = function (scene, children, camera) {
      if (!enabled || children.length < 2 || camera.scrollX !== 0 || camera.scrollY !== 0 || camera.alpha !== 1 ||
          camera.mask || camera.rotation !== 0) return origRender.apply(this, arguments);
      let c = camera._pkrCover;
      if (c && (c.scene !== scene || !covers(c, camera))) c = null;
      if (!(camera._pkrCoverIn > 0)) {
        camera._pkrCoverIn = COVER_SEARCH;
        c = findCover(children, camera);
      }
      camera._pkrCoverIn--;
      camera._pkrCover = c;
      if (!c) return origRender.apply(this, arguments);
      let top = c;
      while (top.parentContainer) top = top.parentContainer;
      const k = children.indexOf(top);
      if (k <= 0) return origRender.apply(this, arguments);
      coverStats.frames++;
      coverStats.skipped += k;
      listInteractive(scene, children, k, camera);
      return origRender.call(this, scene, children.slice(k), camera);
    };
    g.__rt.coverSwap = [WRP, "render", origRender];
    g.__rt.coverFind = (list, camera) => findCover(list, camera); // scripts/probe_*
  }

  // 21. The input plugin's object list: an object made interactive is queued only if it isn't in the
  //     pending list or the list already, each checked with indexOf, and each object removed is found
  //     with indexOf and spliced out. PokeRogue's item bar destroys and makes all its ~46 interactive
  //     icons again on every item change, over a list of hundreds: 27% of that rebuild on the desktop
  //     (~250 ms on the Vita, several times a turn late in a run, log209). Here membership is kept in
  //     sets (rebuilt from the arrays whenever something else changed them) and the removals are done
  //     in one pass; the lists, their order and the clear() calls come out as Phaser's code makes them.
  {
    const IP = Phaser.Input && Phaser.Input.InputPlugin && Phaser.Input.InputPlugin.prototype;
    const EV = Phaser.Input && Phaser.Input.Events;
    const origPre = IP && IP.preUpdate, origQueue = IP && IP.queueForInsertion;
    if (IP && EV && EV.PRE_UPDATE && typeof origPre === "function" && typeof origQueue === "function" &&
        /this\._list=\w+\.concat\(\w+\.splice\(0\)\)/.test(String(origPre)) &&
        /this\._pendingInsertion\.indexOf\(\w+\)/.test(String(origQueue)) && /this\._list\.indexOf\(\w+\)/.test(String(origQueue)) &&
        /this\._pendingInsertion\.push\(\w+\)/.test(String(origQueue))) {
      const sets = (p) => {
        let s = p.__pkrSets;
        if (!s) Object.defineProperty(p, "__pkrSets", { value: (s = {}), writable: true });
        if (s.list !== p._list || s.listLen !== p._list.length) s.list = p._list, s.listLen = p._list.length, s.inList = new Set(p._list);
        if (s.pendArr !== p._pendingInsertion || s.pendLen !== p._pendingInsertion.length)
          s.pendArr = p._pendingInsertion, s.pendLen = p._pendingInsertion.length, s.pend = new Set(p._pendingInsertion);
        return s;
      };
      IP.queueForInsertion = function (child) {
        if (!enabled) return origQueue.apply(this, arguments);
        const s = sets(this);
        if (!s.pend.has(child) && !s.inList.has(child)) {
          this._pendingInsertion.push(child);
          s.pend.add(child), s.pendLen++;
        }
        return this;
      };
      IP.preUpdate = function () {
        if (!enabled) return origPre.apply(this, arguments);
        this.pluginEvents.emit(EV.PRE_UPDATE);
        const removeList = this._pendingRemoval, insertList = this._pendingInsertion;
        if (removeList.length === 0 && insertList.length === 0) return;
        const s = sets(this);
        let current = this._list;
        if (removeList.length) {
          const gone = new Set(), cleared = [];
          for (let i = 0; i < removeList.length; i++) {
            const o = removeList[i];
            if (s.inList.has(o) && !gone.has(o)) gone.add(o), cleared.push(o);
          }
          if (gone.size) {
            current = current.filter((o) => !gone.has(o));
            for (const o of gone) s.inList.delete(o);
            for (let i = 0; i < cleared.length; i++) this.clear(cleared[i], true);
          }
          this._pendingRemoval.length = 0;
        }
        const inserted = insertList.splice(0);
        this._list = current.concat(inserted);
        for (let i = 0; i < inserted.length; i++) s.inList.add(inserted[i]);
        s.list = this._list, s.listLen = this._list.length;
        s.pend = new Set(), s.pendArr = this._pendingInsertion, s.pendLen = 0;
      };
      g.__rt.inputListSwap = [IP, origPre, origQueue]; // tests/phaser
    }
  }

  // Which of the checked fast paths went in. Each one is skipped silently when Phaser's code no
  // longer looks like 3.90's, or when its source isn't there to check (0.49-0.54: compiled code
  // without source, so none were installed on the Vita); this line makes that visible.
  {
    const rt = g.__rt, parts = [["uniforms", rt.origSetUniform], ["pipeline", rt.pipelineSwaps && rt.pipelineSwaps.length],
      ["graphics", rt.graphicsSwap], ["shapes", rt.shapesSwap], ["emit", rt.emitSwap], ["uuid", rt.uuidWrap],
      ["bitmap text", rt.bitmapTextSwap], ["tweens", rt.tweenSwap]];
    const on = parts.filter((p) => p[1]).map((p) => p[0] + (typeof p[1] === "number" ? " (" + p[1] + " functions)" : ""));
    const off = parts.filter((p) => !p[1]).map((p) => p[0]);
    g.__native.log("[fastpath] native: " + (on.join(", ") || "none") + (off.length ? " | left to Phaser's JS: " + off.join(", ") : ""));
  }

  g.__rt.fastpath = g.__fastpath = {
    available: typeof nbs === "function",
    set(on) {
      on = !!(on && nbs);
      enabled = on;
      MP.batchSprite = on ? fastBatchSprite : origSprite;
      NSP.renderWebGL = on ? nineSliceFast : origNineSlice;
      Container.renderWebGL = on ? containerRender : origContainerRender;
    },
  };
})(globalThis);

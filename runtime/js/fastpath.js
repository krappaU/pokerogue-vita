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
// 4. Text.updateText: measure on every change, draw and upload only when rendered (see below).
// 5. NineSlice.renderWebGL: the vertex loop in native code.
// 6. CanvasPool.create/remove: constant-time instead of scanning the whole pool.
// 7. Display list / container membership from the objects' own fields instead of list scans.
// 8. Utils.Array.Remove/RemoveAt and the WebGL renderer's wrapper lists: no JS shifting loop.
// 9. AnimationManager: the sprites' animation-removed listeners in a Set instead of an array.
// 10. Container.renderWebGL in C (src/fastpath.c).
// 11. TextStyle.setStyle without Phaser's generic GetValue lookups; font metrics remembered.
// 12. Utils.String.UUID (a texture name for every Text) made in C.
// 13. WebGLShader.setUniform1..4 in C (PokeRogue's sprite pipelines set ~10 uniforms per sprite).
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
    nbs(pipeline.vertexData, pipeline.vertexCount * 7, camera.matrix.matrix, parent ? parent.matrix : null,
      -camera.scrollX * go.scrollFactorX * sfx, -camera.scrollY * go.scrollFactorY * sfy,
      gx, gy, go.rotation, go.scaleX * fx, go.scaleY * fy, x, y, frame.cutWidth, frame.cutHeight,
      camera.renderRoundPixels, frame.u0, frame.v0, frame.u1, frame.v1,
      go.tintTopLeft, go.tintTopRight, go.tintBottomLeft, go.tintBottomRight,
      a * go._alphaTL, a * go._alphaTR, a * go._alphaBL, a * go._alphaBR, go.tintFill, unit);
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
  const textStats = { updates: 0, measures: 0, draws: 0 };
  g.__rt.textStats = () => Object.assign({}, textStats);
  function measureNow(t) {
    textStats.measures++;
    t._pkrMeasureDirty = false;
    const ctx = t.context;
    const real = ctx._n, tex = t.frame && t.frame.source && t.frame.source.glTexture;
    const r = t.renderer, origC2T = r.canvasToTexture;
    let stub = stubs.get(real);
    if (!stub) stubs.set(real, (stub = measureStub(real)));
    ctx._n = stub;
    // The upload at the end of updateText: keep the current texture (a new text gets one now).
    if (tex) r.canvasToTexture = () => tex;
    try { origUpdateText.call(t); } finally { ctx._n = real; if (tex) delete r.canvasToTexture; }
    if (r.canvasToTexture !== origC2T) r.canvasToTexture = origC2T;
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
      textStats.draws++;
      origUpdateText.call(src);
    }
    return origTextRender.apply(this, arguments);
  }
  TP.updateText = lazyUpdateText;
  TP.renderWebGL = textRender;

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
      textLazySize: lazySize,
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
          return { k, path, parts: path.includes(".") ? path.split(".") : null, def: probe[k],
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
      for (let i = 0; i < STYLE_MAP.length; i++) {
        const s = STYLE_MAP[i], c = n ? s.def : this[s.k];
        if (s.raw) this[s.k] = getValue(e, s.path, s.parts, c);
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

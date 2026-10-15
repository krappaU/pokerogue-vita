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
    return !go.isCropped && !go.frame.customPivot && pipeline.batchQuad === origQuad &&
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
  const PQ = Phaser.Structs.ProcessQueue.prototype, origPQUpdate = PQ.update;
  PQ.update = function () {
    if (this._toProcess !== 0) this._pkrVersion = (this._pkrVersion | 0) + 1;
    const r = origPQUpdate.call(this);
    if (this._pkrPending && this._pending.length === 0) this._pkrPending.clear(); // update() empties it in place
    return r;
  };
  // Membership in the update list's queues by Set instead of indexOf: every sprite added to a scene
  // checks both its pending and active arrays (thousands long during UI setup).
  function pendingSet(q) {
    if (!q._pkrPending || q._pkrPendingArr !== q._pending) {
      q._pkrPending = new Set(q._pending);
      q._pkrPendingArr = q._pending;
    }
    return q._pkrPending;
  }
  function activeSet(q) {
    if (!q._pkrActive || q._pkrActiveArr !== q._active || q._pkrActiveVer !== (q._pkrVersion | 0)) {
      q._pkrActive = new Set(q._active);
      q._pkrActiveArr = q._active;
      q._pkrActiveVer = q._pkrVersion | 0;
    }
    return q._pkrActive;
  }
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
  const spritePreUpdate = Phaser.GameObjects.Sprite.prototype.preUpdate;
  function fastSceneUpdate(time, delta) {
    const list = this._active;
    if (list !== this._pkrList || (this._pkrVersion | 0) !== this._pkrSeen) {
      const others = [], members = new Set();
      for (let i = 0; i < list.length; i++) {
        const go = list[i];
        if (go.preUpdate === spritePreUpdate) members.add(go); else others.push(go);
      }
      this._pkrList = list, this._pkrSeen = this._pkrVersion | 0, this._pkrOthers = others, this._pkrMembers = members;
    }
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
  // size calls go through; drawing and state calls do nothing.
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
  function lazyUpdateText() {
    const ctx = this.context;
    if (!enabled || !this.renderer || !this.renderer.gl || !ctx || !ctx._n) return origUpdateText.call(this);
    const real = ctx._n, tex = this.frame && this.frame.source && this.frame.source.glTexture;
    const r = this.renderer, origC2T = r.canvasToTexture;
    let stub = stubs.get(real);
    if (!stub) stubs.set(real, (stub = measureStub(real)));
    ctx._n = stub;
    // The upload at the end of updateText: keep the current texture (a new text gets one now).
    if (tex) r.canvasToTexture = () => tex;
    try { origUpdateText.call(this); } finally { ctx._n = real; if (tex) delete r.canvasToTexture; }
    if (r.canvasToTexture !== origC2T) r.canvasToTexture = origC2T;
    this._pkrTextDirty = true;
    return this;
  }
  function textRender(renderer, src) {
    if (src._pkrTextDirty) {
      src._pkrTextDirty = false;
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
    if (enabled && this.exclusive && child && !Array.isArray(child) && child.parentContainer !== this &&
        !(this.maxSize > 0 && this.list.length >= this.maxSize)) {
      this.list.push(child);
      this.addHandler(child);
      return this;
    }
    return origContainerAdd.apply(this, arguments);
  };

  g.__rt.fastpath = g.__fastpath = {
    available: typeof nbs === "function",
    set(on) {
      on = !!(on && nbs);
      enabled = on;
      MP.batchSprite = on ? fastBatchSprite : origSprite;
      Container.renderWebGL = on ? fastContainerRender : origContainerRender;
    },
  };
})(globalThis);

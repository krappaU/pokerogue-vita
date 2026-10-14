// SPDX-License-Identifier: AGPL-3.0-only
// Faster rendering paths for Phaser 3.90 under QuickJS.
//
// 1. MultiPipeline.batchSprite: the matrix math, tint packing and 42 vertex writes go to
//    native code (__nativeBatchSprite, src/fastpath.c) in one call.
// 2. Container.renderWebGL: for plain Image/Sprite children, skip the temporary
//    setAlpha/setScrollFactor round trip (six JS calls per child) and pass the container's
//    alpha and scroll factor straight to the sprite batcher.
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

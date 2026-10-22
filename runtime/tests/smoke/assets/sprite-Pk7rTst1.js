// SPDX-License-Identifier: AGPL-3.0-only
// The palette parts of PokeRogue's sprite pipeline (src/pipelines/sprite.ts, Pagefault Games) as its
// bundler emits them, for the smoke test of src/gamefix.c: loaded as a chunk, they get its rewrites;
// fetched as text and evaluated, they don't. The bundle's own names (gr: Pokemon, kr: the variant
// colour cache, $r: rgbHexToRgba; B: the battle scene, wn: getTerrainColor, it: getCurrentTime) are
// parameters here. fieldBind is FieldSpritePipeline.onBind after its super.onBind().
export function bindColors(m,w){
let e=w&&m.spriteColorsBase||m.spriteColors||[],t=w&&m.fusionSpriteColorsBase||m.fusionSpriteColors||[],v=[0,0,0,0],y=[],x=[];for(let m=0;m<32;m++)y.splice(y.length,0,...m<e.length?e[m].map(e=>e/255):v),x.splice(x.length,0,...m<t.length?t[m]:v);this.set4fv(`spriteColors`,y.flat()).set4iv(`fusionSpriteColors`,x.flat())}
export function batchColors(e,gr,kr,$r){
if(e){let t=e,m=t.pipelineData,v=Object.hasOwn(m,`variant`)?m.variant:t.parentContainer instanceof gr?t.parentContainer.variant:0,y,x=[0,0,0,0],S=[],C=[];if((t.parentContainer instanceof gr?t.parentContainer.shiny:m.shiny)&&(y=kr[t.parentContainer instanceof gr?t.parentContainer.getSprite().texture.key:m.spriteKey])&&Object.hasOwn(y,v)){let e=Object.keys(y[v]);for(let t=0;t<32;t++)if(t<e.length){let m=Array.from(Object.values($r(e[t]))),x=Array.from(Object.values($r(y[v][e[t]])));S.splice(S.length,0,...m.map(e=>e/255)),C.splice(C.length,0,...x.map(e=>e/255))}else S.splice(S.length,0,...x),C.splice(C.length,0,...x)}else for(let e=0;e<32;e++)S.splice(S.length,0,...x),C.splice(C.length,0,...x);this.set4fv(`baseVariantColors`,S.flat()).set4fv(`variantColors`,C.flat())}}
export function fieldBind(e,B,overrideTint,wn,it){
let t=e.pipelineData,m=!!t.ignoreTimeTint,v=t.terrainColorRatio??0,y=B.currentBattle?.waveIndex?(B.currentBattle.waveIndex+B.waveCycleOffset)%40/40:it();this.set1f(`time`,y).setBoolean(`ignoreTimeTint`,m).setBoolean(`isOutside`,B.arena.isOutside()).set3fv(`overrideTint`,overrideTint().map(e=>e/255)).set3fv(`dayTint`,B.arena.getDayTint().map(e=>e/255)).set3fv(`duskTint`,B.arena.getDuskTint().map(e=>e/255)).set3fv(`nightTint`,B.arena.getNightTint().map(e=>e/255)).set3fv(`terrainColor`,wn(B.arena.terrainType).map(e=>e/255)).set1f(`terrainColorRatio`,v)}

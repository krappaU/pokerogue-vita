// SPDX-License-Identifier: AGPL-3.0-only
// The game's move animation loader (src/data/battle-anims.ts, Pagefault Games) as its bundler emits
// it, shortened (no fetch: a move's animation is an object made on the spot), for the smoke test of
// src/gamefix.c's "move animations reachable for unloading" and js/sprite_gc.js's dropping.
var ur=new Map,loads=0;export function initMoveAnim(e){return new Promise(t=>{if(ur.has(e)){if(ur.get(e)===null){let n=setInterval(()=>{ur.get(e)!==null&&(clearInterval(n),t())},50)}else t()}else{ur.set(e,null),loads++,Promise.resolve().then(()=>{ur.set(e,{move:e,frames:[1,2,3]}),t()})}})}export function getAnim(e){return ur.get(e)?.frames.length??0}export function loaded(){return loads}export function moveAnims(){return ur}

// SPDX-License-Identifier: AGPL-3.0-only
// The end of BattleScene.reset (src/battle-scene.ts, Pagefault Games) as its bundler emits it, for the
// smoke test of src/gamefix.c.
export class Scene{constructor(c,g){this.children=c,this.game=g,this.launched=0}finish(){this.children.removeAll(!0),this.game.domContainer.innerHTML=``,this.launchBattle()}launchBattle(){this.launched++}}

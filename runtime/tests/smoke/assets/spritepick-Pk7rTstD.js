// SPDX-License-Identifier: AGPL-3.0-only
// The starter select screen's updateSprite (src/ui/handlers/starter-select-ui-handler.ts, Pagefault
// Games) as its bundler emits it, for the smoke test of src/gamefix.c's "starter sprite from the
// latest request only". The sprite and the species are parameters.
export function makeHandler(){return class StarterSelectUiHandler{constructor(e){this.pokemonSprite=e,this.statsMode=!1}updateSprite(e,t,m,v,y){e.loadAssets(t,m,v,y,!0).then(()=>{this.pokemonSprite.play(e.getSpriteKey(t,m,v,y)).setPipelineData(`shiny`,v).setPipelineData(`variant`,y).setPipelineData(`spriteKey`,e.getSpriteKey(t,m,v,y)).setVisible(!this.statsMode)})}}}

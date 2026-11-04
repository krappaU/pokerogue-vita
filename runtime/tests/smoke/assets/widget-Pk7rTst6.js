// SPDX-License-Identifier: AGPL-3.0-only
// The end of TimeOfDayWidget's constructor (src/ui/time-of-day-widget.ts, Pagefault Games) as its
// bundler emits it, for the smoke test of src/gamefix.c. The base class and the scene are parameters.
export function makeWidget(Base,B){return class TimeOfDayWidget extends Base{onEncounterPhaseEvent=e=>this.onEncounterPhase(e);constructor(){super(),(this.timeOfDayIcons=[],this.add(this.timeOfDayIcons),B.eventTarget.addEventListener(`onEncounterPhase`,this.onEncounterPhaseEvent))}getBackTweens(){return[]}onEncounterPhase(e){this.seen=(this.seen||0)+1}}}

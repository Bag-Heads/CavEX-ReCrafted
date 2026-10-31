/*
	CaveWii_v2 CAVEWII_AGENT bridge -- live state-export collector.

	Kept OUT of state_export.h (the scorer-owned PURE contract) on purpose: this is
	the engine-side half that reads gstate. PC-only, env-gated. Fills a
	`struct game_export_state` from the running game and writes one JSON line
	(state_export_write) to stdout per call.
*/

#ifndef STATE_EXPORT_EMIT_H
#define STATE_EXPORT_EMIT_H

#include <stdbool.h>

// Emit one per-tick state line to stdout IFF the bridge is armed (env CAVEWII_AGENT=1,
// checked once). A no-op otherwise, so a normal run prints nothing. PC build only --
// the caller guards the call with #ifdef PLATFORM_PC.
void state_export_emit(void);

// True once CAVEWII_AGENT=1 has armed the bridge (cached). Lets the main loop gate
// the (future) action read on the same flag without re-reading the env.
bool state_export_agent_armed(void);

#endif

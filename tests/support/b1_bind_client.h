/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * Split-dims B1 local stamping (lane B handoff, 2026-09-26): server_local's
 * world-scoped producers stamp their traffic with the producing world's A1a
 * identity, and a real client binds its world context to it from the stamped
 * load CRPC_WORLD_RESET. Fixtures that install the client world by hand (an
 * unstamped CRPC_WORLD_RESET or a registry install) BEFORE creating the
 * server world call this once the world exists: it re-installs the client's
 * current slot context bound to that world's identity, exactly the state the
 * real load reset produces, so the fixture's stamped traffic applies instead
 * of being dropped as STALE. No-op when the client has no current context
 * (a server-only fixture) or the world is not live (generation 0). */
#ifndef B1_BIND_CLIENT_H
#define B1_BIND_CLIENT_H

#include "game/client_world_context.h"
#include "network/client_interface.h"
#include "network/server_world.h"

static inline void b1_bind_client_world(const struct server_world* w) {
	if(!w || w->generation == 0)
		return;
	struct client_ctx_registry* reg = clin_world_contexts();
	struct client_world_context* cur
		= client_ctx_registry_current(reg, w->world_index);
	if(!cur || cur->server_generation == w->generation)
		return;
	int dimension = cur->dimension;
	uint8_t views = cur->bound_views;
	struct client_world_context* bound = client_ctx_registry_install(
		reg, dimension, w->world_index, w->generation);
	if(bound)
		for(unsigned v = 0; v < 2; v++)
			if(views & (1u << v))
				(void)client_ctx_bind_view(bound, v);
}

#endif

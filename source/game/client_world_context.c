#include "client_world_context.h"
#include "../chunk_mesher.h"

#include <assert.h>
#include <limits.h>

#define CLIENT_CTX_SLOTS CLIENT_CTX_SLOT_COUNT

/* The registry pool bound (client_world_context.h, POOL BOUND): every retiring
 * entry holds >= 1 lease; mesher leases are capped by the mesher token pool
 * and at most one GPU hand-off lease exists at a time, so the current
 * contexts plus that many retiring ones must fit. A CHUNK_MESHER_QLENGTH bump
 * past this fails the build instead of silently turning "fail closed" into
 * "world load aborted" (Fable B1-phase2 F4). */
_Static_assert(CHUNK_MESHER_QLENGTH + 1 + CLIENT_CTX_SLOT_COUNT
				   <= CLIENT_CTX_POOL,
			   "client context pool too small for the mesher token pool + "
			   "one GPU lease + the current contexts");

/* Per-slot lifetime high-water mark (ASTRA-B1-RULINGS-0925 #1). Lives
 * outside any context so it survives the destruction of the context that
 * issued the generation (reconnect, respawn, world switch). Written only by
 * client_ctx_init / client_ctx_reset / client_ctx_slot_high_water_raise, all
 * serialized by the client owner. Never wraps: UINT32_MAX is the last value a
 * slot may issue and issue_generation() fails past it. */
static uint32_t client_ctx_slot_mark[CLIENT_CTX_SLOTS];

/* Issue the slot's next generation, or return 0 (never a valid generation)
 * when the slot is exhausted. Callers check before mutating any context so a
 * failed init/reset leaves it byte-for-byte unchanged. */
static uint32_t issue_generation(unsigned world_index) {
	if(world_index >= CLIENT_CTX_SLOTS
	   || client_ctx_slot_mark[world_index] == UINT32_MAX)
		return 0;
	return ++client_ctx_slot_mark[world_index];
}

static bool any_lease(const struct client_world_context* ctx) {
	for(int k = 0; k < CLIENT_CTX_LEASE_COUNT; k++)
		if(ctx->leases[k] != 0) return true;
	return false;
}

bool client_ctx_init(struct client_world_context* ctx, int dimension,
					  unsigned world_index) {
	if(!ctx || world_index >= CLIENT_CTX_SLOTS) return false;
	uint32_t generation = issue_generation(world_index);
	if(generation == 0) return false;
	*ctx = (struct client_world_context){0};
	ctx->dimension = dimension;
	ctx->world_index = (uint8_t)world_index;
	ctx->generation = generation;
	return true;
}

bool client_ctx_bind_server(struct client_world_context* ctx,
							uint32_t server_generation) {
	if(!ctx || ctx->retiring || server_generation == 0
	   || ctx->server_generation != 0 || ctx->unstamped)
		return false;
	ctx->server_generation = server_generation;
	return true;
}

bool client_ctx_bind_unstamped(struct client_world_context* ctx) {
	if(!ctx || ctx->retiring || ctx->server_generation != 0 || ctx->unstamped)
		return false;
	ctx->unstamped = true;
	return true;
}

bool client_ctx_bind_view(struct client_world_context* ctx, unsigned view) {
	if(!ctx || ctx->retiring || view > 1 || (ctx->bound_views & (1u << view)))
		return false;
	ctx->bound_views |= (uint8_t)(1u << view);
	return true;
}

bool client_ctx_unbind_view(struct client_world_context* ctx, unsigned view) {
	if(!ctx || view > 1 || !(ctx->bound_views & (1u << view)))
		return false;
	ctx->bound_views &= (uint8_t)~(1u << view);
	return true;
}

bool client_ctx_accept_rpc(const struct client_world_context* ctx,
						   unsigned world_index, uint32_t server_generation) {
	/* Authoritative server identity only. An unbound context (0) matches
	 * nothing, and the client epoch is deliberately not consulted here. */
	return ctx && !ctx->retiring && ctx->world_index == world_index
		&& ctx->server_generation != 0
		&& ctx->server_generation == server_generation;
}

bool client_ctx_accept(const struct client_world_context* ctx,
				   unsigned world_index, uint32_t generation) {
	/* Client epoch only: asynchronous completions this client started. */
	return ctx && !ctx->retiring && ctx->world_index == world_index
		&& ctx->generation == generation;
}

bool client_ctx_payload_ready(const struct client_world_context* ctx) {
	return ctx && !ctx->retiring && !ctx->quiescing
		&& (ctx->server_generation != 0 || ctx->unstamped);
}

bool client_ctx_reset(struct client_world_context* ctx) {
	if(!ctx || ctx->retiring) return false;
	uint32_t generation = issue_generation(ctx->world_index);
	if(generation == 0) return false;
	ctx->generation = generation;
	/* Leases are counted per kind, not per generation: anything outstanding
	 * now references the OLD generation's payloads. Park every producer until
	 * all of it drains and the owner has replaced those payloads (ruling #2).
	 * An already-parked context stays parked through a further reset. With
	 * nothing outstanding the owner cleaned up synchronously in this reset,
	 * so there is nothing to drain and producers are not stalled. */
	if(any_lease(ctx)) ctx->quiescing = true;
	return true;
}

bool client_ctx_lease_acquire(struct client_world_context* ctx,
					   enum client_ctx_lease_kind kind) {
	if(!ctx || ctx->retiring || ctx->quiescing || kind < 0
	   || kind >= CLIENT_CTX_LEASE_COUNT || ctx->leases[kind] == UINT32_MAX)
		return false;
	ctx->leases[kind]++;
	return true;
}

bool client_ctx_lease_release(struct client_world_context* ctx,
					   enum client_ctx_lease_kind kind) {
	if(!ctx || kind < 0 || kind >= CLIENT_CTX_LEASE_COUNT
	   || ctx->leases[kind] == 0)
		return false;
	/* Owner-thread only (header THREADING note): a release arriving while
	 * another is in progress means a producer released directly instead of
	 * marshalling to the owner. Best-effort tripwire, asserts-on builds. */
	assert(!ctx->in_release && "client_ctx_lease_release: concurrent release");
	ctx->in_release = true;
	ctx->leases[kind]--;
	ctx->in_release = false;
	return true;
}

void client_ctx_retire(struct client_world_context* ctx) {
	if(ctx) ctx->retiring = true;
}

bool client_ctx_try_free(const struct client_world_context* ctx) {
	return ctx && ctx->retiring && ctx->bound_views == 0 && !any_lease(ctx);
}

uint32_t client_ctx_slot_high_water(unsigned world_index) {
	return world_index < CLIENT_CTX_SLOTS ? client_ctx_slot_mark[world_index]
										  : 0;
}

bool client_ctx_slot_high_water_raise(unsigned world_index, uint32_t floor) {
	if(world_index >= CLIENT_CTX_SLOTS
	   || floor <= client_ctx_slot_mark[world_index])
		return false;
	client_ctx_slot_mark[world_index] = floor;
	return true;
}

bool client_ctx_quiesce_drained(const struct client_world_context* ctx) {
	return ctx && ctx->quiescing && !ctx->retiring && !any_lease(ctx);
}

bool client_ctx_quiesce_complete(struct client_world_context* ctx) {
	if(!client_ctx_quiesce_drained(ctx)) return false;
	ctx->quiescing = false;
	return true;
}

/* ---- Registry ------------------------------------------------------------ */

static struct client_world_context* reg_entry(struct client_ctx_registry* reg,
											  uint8_t one_based) {
	return one_based ? &reg->pool[one_based - 1] : NULL;
}

static void reg_reclaim(struct client_ctx_registry* reg, unsigned i) {
	if(reg->used[i] && client_ctx_try_free(&reg->pool[i])) {
		reg->used[i] = false;
		reg->pool[i] = (struct client_world_context){0};
	}
}

void client_ctx_registry_retire(struct client_ctx_registry* reg,
								unsigned world_index) {
	if(!reg || world_index >= CLIENT_CTX_SLOTS) return;
	uint8_t cur = reg->current[world_index];
	if(!cur) return;
	reg->current[world_index] = 0;
	struct client_world_context* ctx = &reg->pool[cur - 1];
	/* A retiring context keeps no views: the views follow the successor. */
	ctx->bound_views = 0;
	client_ctx_retire(ctx);
	reg_reclaim(reg, cur - 1u);
}

struct client_world_context* client_ctx_registry_install(
	struct client_ctx_registry* reg, int dimension, unsigned world_index,
	uint32_t server_generation) {
	if(!reg || world_index >= CLIENT_CTX_SLOTS) return NULL;
	client_ctx_registry_retire(reg, world_index);
	for(unsigned i = 0; i < CLIENT_CTX_POOL; i++) {
		if(reg->used[i]) continue;
		struct client_world_context fresh;
		if(!client_ctx_init(&fresh, dimension, world_index)) break;
		bool bound = server_generation
			? client_ctx_bind_server(&fresh, server_generation)
			: client_ctx_bind_unstamped(&fresh);
		assert(bound);
		(void)bound;
		reg->pool[i] = fresh;
		reg->used[i] = true;
		reg->current[world_index] = (uint8_t)(i + 1u);
		return &reg->pool[i];
	}
	reg->install_failures++;
	return NULL;
}

struct client_world_context* client_ctx_registry_current(
	struct client_ctx_registry* reg, unsigned world_index) {
	if(!reg || world_index >= CLIENT_CTX_SLOTS) return NULL;
	return reg_entry(reg, reg->current[world_index]);
}

bool client_ctx_registry_owns(const struct client_ctx_registry* reg,
							  const struct client_world_context* ctx) {
	if(!reg || !ctx || ctx < reg->pool || ctx >= reg->pool + CLIENT_CTX_POOL)
		return false;
	return reg->used[ctx - reg->pool];
}

bool client_ctx_registry_release(struct client_ctx_registry* reg,
								 struct client_world_context* ctx,
								 enum client_ctx_lease_kind kind) {
	if(!client_ctx_registry_owns(reg, ctx)) return false;
	if(!client_ctx_lease_release(ctx, kind)) return false;
	reg_reclaim(reg, (unsigned)(ctx - reg->pool));
	return true;
}

bool client_ctx_registry_poll(struct client_ctx_registry* reg) {
	if(!reg) return false;
	bool quiescing = false;
	for(unsigned slot = 0; slot < CLIENT_CTX_SLOTS; slot++) {
		struct client_world_context* ctx = reg_entry(reg, reg->current[slot]);
		if(!ctx || !ctx->quiescing) continue;
		if(!client_ctx_quiesce_complete(ctx)) quiescing = true;
	}
	for(unsigned i = 0; i < CLIENT_CTX_POOL; i++)
		reg_reclaim(reg, i);
	return quiescing;
}

unsigned client_ctx_registry_in_use(const struct client_ctx_registry* reg) {
	unsigned n = 0;
	if(reg)
		for(unsigned i = 0; i < CLIENT_CTX_POOL; i++)
			n += reg->used[i] ? 1u : 0u;
	return n;
}

static enum client_ctx_route route_uncounted(
	struct client_ctx_registry* reg, unsigned world_index,
	uint32_t world_generation, struct client_world_context** out) {
	struct client_world_context* ctx = NULL;
	enum client_ctx_route route;
	if(world_generation == 0) {
		unsigned live = 0, only = 0;
		for(unsigned slot = 0; slot < CLIENT_CTX_SLOTS; slot++)
			if(reg->current[slot]) {
				live++;
				only = slot;
			}
		if(live == 0)
			route = CLIENT_CTX_ROUTE_NO_CONTEXT;
		else if(live > 1)
			route = CLIENT_CTX_ROUTE_AMBIGUOUS;
		else {
			ctx = reg_entry(reg, reg->current[only]);
			route = CLIENT_CTX_ROUTE_APPLY;
		}
	} else if(world_index >= CLIENT_CTX_SLOTS || !reg->current[world_index]) {
		route = CLIENT_CTX_ROUTE_NO_CONTEXT;
	} else {
		ctx = reg_entry(reg, reg->current[world_index]);
		if(client_ctx_accept_rpc(ctx, world_index, world_generation)) {
			route = CLIENT_CTX_ROUTE_APPLY;
		} else {
			route = CLIENT_CTX_ROUTE_STALE;
			ctx = NULL;
		}
	}
	if(route == CLIENT_CTX_ROUTE_APPLY && !client_ctx_payload_ready(ctx))
		route = CLIENT_CTX_ROUTE_HOLD;
	*out = ctx;
	return route;
}

enum client_ctx_route client_ctx_registry_route(
	struct client_ctx_registry* reg, unsigned world_index,
	uint32_t world_generation, struct client_world_context** out) {
	struct client_world_context* ctx = NULL;
	if(!reg) {
		if(out) *out = NULL;
		return CLIENT_CTX_ROUTE_NO_CONTEXT;
	}
	enum client_ctx_route route
		= route_uncounted(reg, world_index, world_generation, &ctx);
	reg->routed[route]++;
	if(out) *out = ctx;
	return route;
}

enum client_ctx_route client_ctx_registry_route_payload(
	struct client_ctx_registry* reg, unsigned payload_slot,
	unsigned world_index, uint32_t world_generation,
	struct client_world_context** out) {
	struct client_world_context* ctx = NULL;
	if(!reg) {
		if(out) *out = NULL;
		return CLIENT_CTX_ROUTE_NO_CONTEXT;
	}
	enum client_ctx_route route
		= route_uncounted(reg, world_index, world_generation, &ctx);
	if((route == CLIENT_CTX_ROUTE_APPLY || route == CLIENT_CTX_ROUTE_HOLD)
	   && ctx != client_ctx_registry_current(reg, payload_slot)) {
		route = CLIENT_CTX_ROUTE_FOREIGN;
		ctx = NULL;
	}
	reg->routed[route]++;
	if(out) *out = ctx;
	return route;
}

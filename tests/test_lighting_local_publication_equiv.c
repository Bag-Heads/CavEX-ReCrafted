/* Copyright (c) 2026 CaveWii_v2 contributors */
#include "support/split_residency_fixture.h"
#include "game/game_state.h"
#include "column_view.h"
#include "config.h"
#include "lighting.h"
#include "settle_observer.h"
#include "item/items.h"

struct bytes { uint8_t* data; size_t size, capacity; };
struct result {
	struct bytes presentation, dirty;
	uint32_t hits, avoided, probes, copy_us, validation_us;
	size_t charged;
};
static struct result* observing;
static bool fail_private;
void* __real_mem2_alloc(size_t bytes);
void* __wrap_mem2_alloc(size_t bytes) {
	if(fail_private && bytes == COLUMN_VIEW_PRIVATE_BYTES) {
		fail_private = false;
		return NULL;
	}
	return __real_mem2_alloc(bytes);
}
static void append(struct bytes* b, const void* data, size_t size) {
	if(b->size + size > b->capacity) {
		size_t capacity = (b->size + size) * 2 + 256;
		void* p = realloc(b->data, capacity);
		assert(p);
		b->data = p; b->capacity = capacity;
	}
	memcpy(b->data + b->size, data, size);
	b->size += size;
}
static void word(struct bytes* b, uint32_t value) { append(b, &value, sizeof(value)); }
static void block_bytes(struct bytes* out, struct block_data b) {
	uint8_t fields[] = {b.type, b.metadata, b.sky_light, b.torch_light};
	append(out, fields, sizeof(fields));
}
static void dirty(const struct settle_input* input) {
	if(!observing || !input->chunk || input->stage > SET_DIRTY_EDIT) return;
	const struct chunk* c = input->chunk;
	word(&observing->dirty, input->stage);
	word(&observing->dirty, c->x); word(&observing->dirty, c->y); word(&observing->dirty, c->z);
	word(&observing->dirty, c->dirty_generation); word(&observing->dirty, c->rebuild_displist);
}
static void configure(bool enabled) {
	struct config c = {.root = json_value_init_object()};
	assert(c.root);
	assert(!config_g9_settled_light_reuse(&c));
	assert(config_set_int(&c, "experimental.g9_priority_lease_int", 0));
	assert(config_set_int(&c, "experimental.g9_main_handoff_int", 0));
	assert(config_set_int(&c, "experimental.g9_settled_light_reuse_int", enabled));
	struct config_g9_lease lease = config_read_g9_lease(&c);
	assert(!lease.enabled && !lease.main_handoff);
	thread_g9_lease_configure(false, lease.max_us, lease.service_us);
	thread_g9_handoff_configure(false);
	column_light_configure(config_g9_settled_light_reuse(&c));
	assert(column_light_enabled() == enabled && !thread_g9_lease_enabled());
	config_destroy(&c);
}
static void strict_config(void) {
	const char* values[] = {"true", "false", "\"1\"", "1.5", "-1", "2", "null"};
	for(unsigned n = 0; n < sizeof(values) / sizeof(values[0]); n++) {
		char json[128];
		snprintf(json, sizeof(json), "{\"experimental\":{\"g9_settled_light_reuse_int\":%s}}", values[n]);
		struct config c = {.root = json_parse_string(json)};
		assert(c.root && !config_g9_settled_light_reuse(&c));
		config_destroy(&c);
	}
	puts("PASS strict 0|1 config, absent OFF, B and lease OFF");
}
static struct server_local* setup(bool enabled, int dim, bool seam, bool open) {
	configure(enabled);
	gstate.remote_session = false;
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = dim}});
	struct server_local* s = split_fixture();
	if(dim == WORLD_DIM_NETHER) {
		server_world_discard(&s->worlds[0]);
		string_t path;
		string_init_set_str(path, split_path);
		assert(server_world_create_with_populate_scratch(&s->worlds[0], path,
			WORLD_DIM_NETHER, 12345, true, s->world_populate_scratch));
		string_clear(path);
		s->worlds[0].session = s; s->worlds[0].local = s;
		b1_bind_client_world(&s->worlds[0]);
	}
	struct server_chunk* sc;
	int count = seam ? 3 : 1;
	for(int n = 0; n < count; n++)
		assert(server_world_load_chunk(&s->worlds[0], n == 1, n == 2, &sc));
	int x = seam ? 15 : 8, z = seam ? 15 : 8;
	static const int offsets[7][3] = {
		{0,0,0},{0,1,0},{0,-1,0},{-1,0,0},{1,0,0},{0,0,1},{0,0,-1}};
	for(unsigned n = 0; n < 7; n++) {
		if(open && n == 4) continue; /* propagation path for partial-OOM proof */
		(void)server_world_set_block(&s->worlds[0], x + offsets[n][0],
			8 + offsets[n][1], z + offsets[n][2], (struct block_data){.type = BLOCK_STONE});
	}
	assert(server_world_light_drain(&s->worlds[0])); /* full install starts settled */
	clin_drain_discard();
	for(int n = 0; n < count; n++) {
		sc = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(n == 1, n == 2));
		assert(sc);
		struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
		assert(slot);
		clin_chunk_slot_copy_local(slot, sc);
		memset(slot->grass_i, 0, sizeof(slot->grass_i));
		memset(slot->grass_j, 0, sizeof(slot->grass_j));
		slot->piston_moving.count = 0;
		struct client_rpc call = {.type = CRPC_CHUNK,
			.payload.chunk = {.x = (n == 1) * 16, .z = (n == 2) * 16,
				.sx = 16, .sy = WORLD_HEIGHT, .sz = 16, .slot = slot}};
		clin_rpc_stamp(&call, s->worlds[0].world_index, s->worlds[0].generation);
		clin_rpc_send(&call);
	}
	clin_update_drain();
	assert(world_find_chunk(&gstate.world, x, 8, z)->ownership == LOCAL_SHARED_VIEW);
	assert(chunk_mesher_init());
	return s;
}
static void stop(struct server_local* s) {
	observing = NULL; chunk_settle_observe = NULL;
	clin_drain_discard();
	chunk_mesher_shutdown();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	world_destroy(&gstate.world);
	client_session_clear_windows(&gstate.players[0]);
	dict_entity_clear(gstate.entities);
	free(gstate.players[0].local_player); gstate.players[0].local_player = NULL;
	split_destroy(s);
	struct column_light_stats stats;
	column_light_get_stats(&stats);
	assert(stats.live == 0 && stats.bytes == 0);
	uint32_t made, freed, refs;
	column_backing_test_counts(&made, &freed, &refs);
	assert(made == freed && refs == 0);
}
static void publication_point(struct result* result, bool seam) {
	struct block_data* padded = calloc(18u * 18u * 18u, sizeof(*padded));
	assert(padded);
	for(int n = 0; n < (seam ? 3 : 1); n++) {
		struct world_section* section = dict_wsection_get(gstate.world.sections,
			SECTION_TO_ID(n == 1, n == 2));
		word(&result->presentation, section != NULL);
		if(!section) continue;
		append(&result->presentation, section->heightmap, sizeof(section->heightmap));
		for(unsigned cy = 0; cy < COLUMN_HEIGHT; cy++) {
			struct chunk* c = section->column[cy];
			word(&result->presentation, c != NULL);
			if(!c) continue;
			word(&result->presentation, c->dirty_generation);
			word(&result->presentation, c->rebuild_displist);
			chunk_mesher_gather(c, padded); /* production padded-border gather */
			for(unsigned i = 0; i < 18u * 18u * 18u; i++)
				block_bytes(&result->presentation, padded[i]);
			assert(chunk_mesher_send(c)); /* production worker and emitted lists */
			c->mesh_in_flight = true; c->rebuild_displist = false;
			ptime_t start = time_get();
			while(chunk_mesher_pending()) {
				chunk_mesher_receive();
				assert(time_diff_s(start, time_get()) < 30);
				if(chunk_mesher_pending()) thread_msleep(1);
			}
			for(unsigned k = 0; k < CHUNK_MESH_LISTS; k++) {
				word(&result->presentation, c->has_displist[k]);
				if(c->has_displist[k]) {
					word(&result->presentation, (uint32_t)c->mesh[k].index);
					append(&result->presentation, c->mesh[k].data, c->mesh[k].index);
				}
			}
		}
	}
	free(padded);
}

enum control {
	MATCH, OWNER, DIMENSION, WORLD_EPOCH, INCARNATION, CONTENT, LIGHT, EVENTS,
	METADATA, HEIGHT, BORDER, SPECULATIVE, QUIET_ABA, LATER_WRITE, EVICTION,
	RESET, ACTIVE, MULTIPLE, TORCH, ROOF, CHECKPOINT, DRAIN_OOM, RECOVERY, WRAP,
	CAP, LIGHT_BYTES, PARTIAL_OOM, RELOAD, STALE_RPC, CONTROL_COUNT
};
static const char* names[] = {"match", "owner", "dimension", "world epoch", "incarnation",
	"content", "light", "ordered events", "metadata", "height", "border", "speculative",
	"quiet ABA", "later server write", "eviction", "reset", "active flood", "multiple edits",
	"torch", "roof", "checkpoint trial/rollback", "reservation OOM", "recovery", "wrap", "cap", "captured light bytes", "partial drain OOM", "reload before ingress", "stale queued reset"};
static void recover_under_pressure(struct server_local* s) {
	struct server_chunk* sc = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(0, 0));
	assert(sc && sc->backing);
	struct column_view_stats stats;
	column_view_get_stats(&stats);
	assert(stats.pages == 0 && stats.fallback_live == 0);
	/* SPLIT-SPILL (pool 256 -> 1024): this scenario is DEFINED at a 256-page
	 * capacity (2 shared victims + 16 divergent 16-page views exactly fill
	 * it, so the victims' next private write must fail into recovery). Pin
	 * admissions at 256 so it keeps that meaning at any configured capacity
	 * >= 256, as test_column_dedup_pressure does. */
	_Static_assert(COLUMN_VIEW_POOL_PAGES >= 256, "scenario needs a 256-page admission limit");
	column_view_test_page_limit(256);
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	clin_chunk_slot_copy_local(slot, sc);
	struct column_view* victims[2], *divergent[16];
	for(unsigned n = 0; n < 2; n++) {
		victims[n] = column_view_create(slot->backing, slot->ids, slot->metadata,
			slot->lighting_sky, slot->lighting_torch);
		assert(victims[n]);
	}
	for(unsigned i = 0; i < 16 * COLUMN_VIEW_PAGE_CELLS; i++) slot->ids[i] ^= 1;
	for(unsigned n = 0; n < 16; n++) {
		divergent[n] = column_view_create(slot->backing, slot->ids, slot->metadata,
			slot->lighting_sky, slot->lighting_torch);
		assert(divergent[n]);
	}
	column_view_get_stats(&stats);
	assert(stats.pages == 256);
	for(unsigned n = 0; n < 2; n++) {
		fail_private = true;
		column_view_set(victims[n], 0, (struct block_data){.sky_light = 1}, true);
		assert(!fail_private);
		assert(column_view_recovery_pending() == (n == 1));
	}
	assert(column_view_recovery_begin()); /* real presentation invalidation */
	world_unload_all(&gstate.world); /* exactly-once held publication release */
	for(unsigned n = 0; n < 2; n++) column_view_unref(victims[n]);
	for(unsigned n = 0; n < 16; n++) column_view_unref(divergent[n]);
	clin_chunk_slot_release(slot);
	column_view_recovery_reset();
	column_view_test_page_limit(COLUMN_VIEW_POOL_PAGES);
	column_view_get_stats(&stats);
	assert(stats.pages == 0 && stats.fallback_live == 0);
}
static struct result run(bool enabled, enum control control, int dim, bool seam) {
	struct result result = {0};
	struct server_local* s = setup(enabled, dim, seam, control == PARTIAL_OOM);
	int x = seam ? 15 : 8, z = seam ? 15 : 8;
	publication_point(&result, seam);
	struct column_light_stats before, after;
	column_light_get_stats(&before);
	struct column_light_publication* held[COLUMN_LIGHT_PUBLICATIONS] = {0};
	if(control == CAP && enabled)
		for(unsigned n = 0; n < COLUMN_LIGHT_PUBLICATIONS; n++) {
			held[n] = column_light_publication_alloc(); assert(held[n]);
		}
	if(control == QUIET_ABA) {
		assert(server_world_set_block_metadata_quiet(&s->worlds[0], x + 1, 8, z, 7));
		assert(server_world_set_block_metadata_quiet(&s->worlds[0], x + 1, 8, z, 0));
	}
	if(control == DRAIN_OOM) server_world_test_light_fail_after(0);
	observing = &result; chunk_settle_observe = dirty;
	bool edited = server_world_set_block(&s->worlds[0], x, 8, z,
		(struct block_data){.type = control == TORCH || control == PARTIAL_OOM
			? BLOCK_TORCH : BLOCK_STONE, .metadata = 1});
	if(control == DRAIN_OOM) {
		assert(!edited && server_world_light_halted(&s->worlds[0]));
		server_world_test_light_fail_after(-1);
	} else {
		assert(edited);
		if(control == ROOF)
			assert(server_world_set_block(&s->worlds[0], x, 80, z, (struct block_data){.type = BLOCK_STONE}));
		if(control == MULTIPLE)
			assert(server_world_set_block(&s->worlds[0], x, 8, z, (struct block_data){.type = BLOCK_STONE, .metadata = 2}));
		if(control == CHECKPOINT) {
			assert(server_world_checkpoint_chunks(&s->worlds[0]));
			assert(server_world_light_pending(&s->worlds[0]));
			column_light_get_stats(&after);
			assert(after.published == before.published); /* trial never publishes */
		}
		if(control == PARTIAL_OOM) {
			lighting_test_drain_fail_reserve_after(2);
			assert(!server_world_light_drain(&s->worlds[0]));
			lighting_test_drain_fail_reserve_after(-1);
			assert(server_world_light_halted(&s->worlds[0])
				&& server_world_light_pending(&s->worlds[0])
				&& !stack_empty(&s->worlds[0].light_frontier));
			column_light_get_stats(&after);
			assert(after.published == before.published);
		} else assert(server_world_light_drain(&s->worlds[0]));
	}
	if(control == LATER_WRITE)
		assert(server_world_set_block_metadata_quiet(&s->worlds[0], x + 1, 8, z, 3));
	if(control == RELOAD) {
		struct server_chunk* sc = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(0, 0));
		struct column_backing* old = sc->backing;
		assert(server_world_save_chunk(&s->worlds[0], true, 0, 0));
		assert(server_world_load_chunk(&s->worlds[0], 0, 0, &sc) && sc->backing != old);
	}
	if(control == STALE_RPC)
		clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
			.payload.world_reset = {.dimension = WORLD_DIM_NETHER}});
	clin_update_drain();
	struct column_light_publication* p = gstate.world.settled_light;
	if(enabled && control <= EVENTS) assert(p);
	if(p) {
		if(control == OWNER) p->owner++;
		if(control == DIMENSION) p->dimension = dim == WORLD_DIM_NETHER ? 0 : -1;
		if(control == WORLD_EPOCH) p->world_generation++;
		if(control == INCARNATION) p->dependency[0].revision.incarnation++;
		if(control == CONTENT) p->dependency[0].revision.content++;
		if(control == LIGHT) p->dependency[0].revision.light++;
		if(control == EVENTS) p->dependency[0].revision.events++;
	}
	if(control == METADATA || control == SPECULATIVE)
		chunk_set_block_raw(world_find_chunk(&gstate.world, x, 8, z), W2C_COORD(x), 8,
			W2C_COORD(z), (struct block_data){.type = BLOCK_STONE, .metadata = control == METADATA ? 7 : 1});
	if(control == LIGHT_BYTES)
		chunk_set_light(world_find_chunk(&gstate.world, x, 8, z), W2C_COORD(x), 8,
			W2C_COORD(z), 1);
	if(control == HEIGHT) {
		struct world_section* section = dict_wsection_get(gstate.world.sections, SECTION_TO_ID(0, 0));
		section->heightmap[W2C_COORD(x) + W2C_COORD(z) * 16]++;
	}
	if(control == BORDER) {
		uint8_t height[256], top[256], tint[256] = {0};
		memset(height, 65, sizeof(height)); memset(top, BLOCK_STONE, sizeof(top));
		assert(world_build_far_section(&gstate.world, 16, 0, height, top, tint, tint));
	}
	if(control == EVICTION) world_unload_section(&gstate.world, 0, 0);
	if(control == RESET) world_unload_all(&gstate.world);
	if(control == ACTIVE) lighting_flood_seed(&gstate.world.lighting_flood_queue, x, 8, z, true);
	if(control == RECOVERY) {
		recover_under_pressure(s);
	}
	if(control == WRAP) {
		struct server_chunk* sc = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(0, 0));
		thread_stripe_lock(sc->backing->mutex);
		sc->backing->revision.content = UINT32_MAX;
		column_light_mutated(sc->backing, false);
		assert(!enabled || sc->backing->revision.content == 0);
		thread_stripe_unlock(sc->backing->mutex);
	}
	for(unsigned n = 0; n < COLUMN_LIGHT_PUBLICATIONS; n++) column_light_publication_release(held[n]);
	publication_point(&result, seam);
	unsigned frames = 0;
	do {
		world_update_lighting(&gstate.world);
		result.charged += world_lighting_driver_last_work();
		assert(world_lighting_driver_last_work() <= LIGHTING_CLIENT_POPS_PER_FRAME);
		assert(++frames < 1000);
	} while(!stack_empty(&gstate.world.lighting_updates)
		|| !lighting_flood_queue_empty(&gstate.world.lighting_flood_queue));
	publication_point(&result, seam);
	column_light_get_stats(&after);
	result.hits = after.hits - before.hits;
	result.avoided = after.avoided_pops - before.avoided_pops;
	result.probes = after.probe_pops - before.probe_pops;
	result.copy_us = after.copy_us - before.copy_us;
	result.validation_us = after.validate_us - before.validate_us;
	if(enabled) {
		printf("case %s: published=%u hits=%u fallbacks=", names[control],
			after.published - before.published, result.hits);
		for(unsigned n = 0; n < COLUMN_LIGHT_FALLBACKS; n++)
			printf("%s%u", n ? "," : "", after.fallback[n] - before.fallback[n]);
		puts("");
	}
	if((control == MATCH || control == CHECKPOINT) && enabled && dim == WORLD_DIM_OVERWORLD)
		assert(result.hits == 1 && result.avoided > 0);
	if(dim == WORLD_DIM_NETHER) assert(result.hits == 0); /* differing height identity */
	if(control != MATCH && control != CHECKPOINT) assert(result.hits == 0);
	stop(s);
	return result;
}
static void equal(const struct bytes* a, const struct bytes* b) {
	assert(a->size == b->size && !memcmp(a->data, b->data, a->size));
}
static void dispose(struct result* r) { free(r->presentation.data); free(r->dirty.data); }
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init(); items_init();
	assert(svin_init() && clin_init());
	strict_config();
	for(enum control control = MATCH; control < CONTROL_COUNT; control++) {
		/* A boundary mismatch needs an actual resident neighbouring column. */
		bool seam = control == BORDER;
		struct result off = run(false, control, WORLD_DIM_OVERWORLD, seam);
		struct result on = run(true, control, WORLD_DIM_OVERWORLD, seam);
		equal(&off.presentation, &on.presentation); equal(&off.dirty, &on.dirty);
		assert(off.charged == on.charged);
		printf("PASS %s: publications/padded inputs/mesh bytes/dirty order identical; hits=%u avoided=%u probe=%u copy_elapsed_us=%u validation_elapsed_us=%u charged=%zu\n",
			names[control], on.hits, on.avoided, on.probes, on.copy_us, on.validation_us, on.charged);
		dispose(&off); dispose(&on);
	}
	for(int dim = -1; dim <= 0; dim++) {
		struct result off = run(false, MATCH, dim, true), on = run(true, MATCH, dim, true);
		equal(&off.presentation, &on.presentation); equal(&off.dirty, &on.dirty);
		printf("PASS seam dimension=%d hits=%u avoided=%u\n", dim, on.hits, on.avoided);
		dispose(&off); dispose(&on);
	}
	struct column_light_stats stats;
	column_light_get_stats(&stats);
	assert(stats.peak_live <= 4 && stats.peak_bytes <= COLUMN_LIGHT_PUBLICATION_BYTES);
	printf("R23 aggregate hits=%u avoided=%u probe=%u peak_live=%u peak_bytes=%u live=%u; compute saving UNKNOWN\n",
		stats.hits, stats.avoided_pops, stats.probe_pops, stats.peak_live, stats.peak_bytes, stats.live);
	configure(false);
	return 0;
}

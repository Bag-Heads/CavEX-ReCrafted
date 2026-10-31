/* Copyright (c) 2026 CaveWii_v2 contributors
 * A/B the real loading stream; compare every published plane, then the near
 * world's planes and simulation clocks/RNG for 600 ticks. Physical residency
 * is intentionally excluded: the optimization transfers support to disk. */
#include "support/split_residency_fixture.h"
#include "carve_observer.h"
#include "chunk_mesher.h"
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "lighting.h"
#include "platform/audio.h"
#include "stack.h"
#include "util.h"
#include "world.h"
#include "worldgen/worldgen.h"
#include <inttypes.h>
#include <limits.h>

static uint64_t stream;
static unsigned sends;
/* G9X-6: the REAL client world (gstate.world), fed through the production
 * FIFO consumer (clin_update_drain -> clin_process) and relit by the normal
 * client lighting driver, exactly like test_light_queue_equiv's
 * packet_delivery case. It replaces the old discarding receiver, which
 * rebuilt every CHUNK snapshot into a private world and dropped every other
 * RPC (SET_BLOCK, UNLOAD, settled light). */
static struct world* rendered;
static uint64_t complete;
static uint64_t pin_complete;
static uint64_t moving[4];
static unsigned populations[4];
static uint64_t readmitted_planes[2], readmitted_mesh[2];
static uint32_t readmitted_cursor[2];
static FILE* attribution_file;
/* The replacement world's starting simulation state, identical in every run. */
struct start_state {
	uint64_t rand, hell_rand;
	uint32_t tick;
	size_t scheduled, chunks;
};
static struct start_state start;
static bool have_start;
#define CONTINUATION_TICKS 600u
struct continuation_snapshot {
	uint32_t current_tick;
	uint64_t rand_seed;
	uint64_t planes;
	size_t scheduled_count;
	struct scheduled_tick* scheduled;
};
static struct continuation_snapshot continuation[CONTINUATION_TICKS];
static unsigned continuation_comparisons;
enum continuation_mode {
	CONTINUATION_NONE,
	CONTINUATION_PIN_RECORD,
	CONTINUATION_RECORD,
	CONTINUATION_COMPARE_OFF,
	CONTINUATION_PIN_ON,
};
static bool attribution_line(const char* line) {
	return fprintf(attribution_file, "%s\n", line) > 0;
}
static uint64_t hash_bytes(uint64_t hash, const void* data, size_t n) {
	const uint8_t* p = data;
	while(n--) { hash ^= *p++; hash *= UINT64_C(1099511628211); }
	return hash;
}
/* ---- G9X-6 RED instrumentation: delivery log + client/server comparison ---- */
#define S_IDX(lx, y, lz) ((size_t)(y) + ((size_t)(lz) + (size_t)(lx) * CHUNK_SIZE) * WORLD_HEIGHT)
enum plane { PLANE_ID, PLANE_META, PLANE_SKY, PLANE_BLOCK, PLANE_HEIGHT, PLANES };
static const char* const plane_name[PLANES] = {"id", "meta", "Sky", "Block", "height"};
#define WIN_X0 (-MAX_VIEW_DISTANCE - 4)
#define WIN_W (16 + 2 * MAX_VIEW_DISTANCE + 9)
#define WIN_Z0 (-MAX_VIEW_DISTANCE - 4)
#define WIN_H (2 * MAX_VIEW_DISTANCE + 9)
struct delivery {
	bool any;
	uint8_t type;		   /* last RPC sent for this column */
	bool consumed;		   /* the client FIFO consumed that RPC */
	unsigned step, update; /* when it was sent (step UINT_MAX = loading) */
	unsigned chunks, sets, unloads;
	bool have_chunk; /* plane digests of the LAST delivered CHUNK snapshot */
	unsigned chunk_step, chunk_update;
	struct column_revision chunk_rev;
	uint64_t sent[4];
};
static struct delivery deliveries[WIN_W][WIN_H];
static unsigned cur_step = UINT_MAX, cur_update;
static const char* cur_run = "?";
static bool drain_in_observer = true;
static struct delivery* delivery_at(int cx, int cz) {
	int i = cx - WIN_X0, j = cz - WIN_Z0;
	if(i < 0 || i >= WIN_W || j < 0 || j >= WIN_H) return NULL;
	return &deliveries[i][j];
}
static uint64_t digest_cells(const uint8_t* bytes, bool nibble) {
	uint64_t h = UINT64_C(14695981039346656037);
	for(size_t i = 0; i < 32768; i++) {
		uint8_t v = nibble ? (uint8_t)((bytes[i / 2] >> ((i & 1) ? 4 : 0)) & 15) : bytes[i];
		h ^= v; h *= UINT64_C(1099511628211);
	}
	return h;
}
static uint64_t digest_client(int cx, int cz, enum plane p) {
	uint64_t h = UINT64_C(14695981039346656037);
	for(int lx = 0; lx < 16; lx++) for(int lz = 0; lz < 16; lz++)
		for(int y = 0; y < WORLD_HEIGHT; y++) {
			struct block_data b = world_get_block(&gstate.world, cx * 16 + lx, y, cz * 16 + lz);
			uint8_t v = p == PLANE_ID ? b.type : p == PLANE_META ? b.metadata
				: p == PLANE_SKY ? b.sky_light : b.torch_light;
			h ^= v; h *= UINT64_C(1099511628211);
		}
	return h;
}
static uint64_t digest_server(const struct server_chunk* c, enum plane p) {
	return p == PLANE_ID ? digest_cells(c->ids, false)
		: p == PLANE_META ? digest_cells(c->metadata, true)
		: p == PLANE_SKY ? digest_cells(c->lighting_sky, true)
		: digest_cells(c->lighting_torch, true);
}
static void note_send(int cx, int cz, uint8_t type) {
	struct delivery* d = delivery_at(cx, cz);
	if(!d) return;
	d->any = true; d->type = type; d->consumed = false;
	d->step = cur_step; d->update = cur_update;
	if(type == CRPC_UNLOAD_CHUNK) { d->unloads++; d->have_chunk = false; }
	if(type == CRPC_SET_BLOCK || type == CRPC_SET_BLOCK_PACK) d->sets++;
}
static void note_consume(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	int cx, cz;
	switch(rpc->type) {
		case CRPC_CHUNK:
			cx = WCOORD_CHUNK_OFFSET(rpc->payload.chunk.x);
			cz = WCOORD_CHUNK_OFFSET(rpc->payload.chunk.z); break;
		case CRPC_UNLOAD_CHUNK:
			cx = rpc->payload.unload_chunk.x; cz = rpc->payload.unload_chunk.z; break;
		case CRPC_SET_BLOCK:
			cx = WCOORD_CHUNK_OFFSET(rpc->payload.set_block.x);
			cz = WCOORD_CHUNK_OFFSET(rpc->payload.set_block.z); break;
		default: return;
	}
	struct delivery* d = delivery_at(cx, cz);
	if(d && d->type == rpc->type) d->consumed = true;
}
static const char* rpc_type_name(uint8_t t) {
	return t == CRPC_CHUNK ? "CHUNK" : t == CRPC_UNLOAD_CHUNK ? "UNLOAD_CHUNK"
		: t == CRPC_SET_BLOCK ? "SET_BLOCK" : t == CRPC_SET_BLOCK_PACK ? "SET_BLOCK_PACK"
		: "other";
}
static void receive(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_CHUNK) {
		const struct chunk_rpc_slot* slot = rpc->payload.chunk.slot;
		stream = hash_bytes(stream, &rpc->payload.chunk.x, sizeof(rpc->payload.chunk.x));
		stream = hash_bytes(stream, &rpc->payload.chunk.z, sizeof(rpc->payload.chunk.z));
		stream = hash_bytes(stream, slot->ids, 32768);
		stream = hash_bytes(stream, slot->metadata, 16384);
		stream = hash_bytes(stream, slot->lighting_sky, 16384);
		stream = hash_bytes(stream, slot->lighting_torch, 16384);
		stream = hash_bytes(stream, slot->heightmap, 256);
		sends++;
		int cx = WCOORD_CHUNK_OFFSET(rpc->payload.chunk.x);
		int cz = WCOORD_CHUNK_OFFSET(rpc->payload.chunk.z);
		note_send(cx, cz, CRPC_CHUNK);
		struct delivery* d = delivery_at(cx, cz);
		if(d) {
			d->chunks++; d->have_chunk = true;
			d->chunk_step = cur_step; d->chunk_update = cur_update;
			d->chunk_rev = slot->local_revision;
			d->sent[PLANE_ID] = digest_cells(slot->ids, false);
			d->sent[PLANE_META] = digest_cells(slot->metadata, true);
			d->sent[PLANE_SKY] = digest_cells(slot->lighting_sky, true);
			d->sent[PLANE_BLOCK] = digest_cells(slot->lighting_torch, true);
		}
	} else if(rpc->type == CRPC_UNLOAD_CHUNK) {
		note_send(rpc->payload.unload_chunk.x, rpc->payload.unload_chunk.z, CRPC_UNLOAD_CHUNK);
	} else if(rpc->type == CRPC_SET_BLOCK) {
		note_send(WCOORD_CHUNK_OFFSET(rpc->payload.set_block.x),
			WCOORD_CHUNK_OFFSET(rpc->payload.set_block.z), CRPC_SET_BLOCK);
	} else if(rpc->type == CRPC_SET_BLOCK_PACK) {
		for(uint32_t i = 0; i < rpc->payload.set_block_pack.n; i++)
			if(!((rpc->payload.set_block_pack.spawn_mask | rpc->payload.set_block_pack.other_mask) & (1u << i)))
				note_send(WCOORD_CHUNK_OFFSET(rpc->payload.set_block_pack.rec[i].set_block.x),
					WCOORD_CHUNK_OFFSET(rpc->payload.set_block_pack.rec[i].set_block.z),
					CRPC_SET_BLOCK_PACK);
	}
	/* Real FIFO consumer: everything queued BEFORE this send is processed by
	 * the production client (this envelope follows it in order). Synchronous
	 * on this thread so delivery timing is identical in every run. */
	if(drain_in_observer) clin_update_drain();
}
/* The normal per-frame client lighting driver, run until idle. */
static void client_light_idle(void) {
	for(int n = 0; n < 1000000; n++) {
		if(stack_empty(&gstate.world.lighting_updates)
		   && lighting_flood_queue_empty(&gstate.world.lighting_flood_queue))
			return;
		world_update_lighting(&gstate.world);
	}
	assert(!"client lighting never went idle");
}
/* After each server update: consume the FIFO (deferred output included),
 * then let the client relight. */
static void client_pump(void) {
	clin_update_drain();
	for(unsigned n = 0; !clin_rpc_flush_pending(); n++) {
		assert(n < 10000);
		clin_update_drain();
	}
	clin_update_drain();
	client_light_idle();
}
static void server_update(struct server_local* s) {
	cur_update++;
	server_local_test_update_once(s);
	client_pump();
}
struct cell { uint8_t present, id, meta, sky, blk; };
#define HALO (CHUNK_SIZE + 2)
#define HALO_CELLS (HALO * HALO * WORLD_HEIGHT)
static struct cell halo_ref[2][2][HALO_CELLS]; /* [target][client/server] */
static uint8_t halo_ref_height[2][2][HALO * HALO];
static uint8_t halo_ref_tint[2][HALO * HALO][2];
static bool halo_ref_valid[2];
static unsigned long red_stale_total;
static struct cell client_cell(int x, int y, int z) {
	if(!world_find_chunk(&gstate.world, x, y, z)) return (struct cell){0};
	struct block_data b = world_get_block(&gstate.world, x, y, z);
	return (struct cell){1, b.type, b.metadata, b.sky_light, b.torch_light};
}
static struct server_chunk* server_col(struct server_local* s, int cx, int cz) {
	return dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(cx, cz));
}
static struct cell server_cell(struct server_local* s, int x, int y, int z) {
	struct server_chunk* c = server_col(s, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));
	if(!c) return (struct cell){0};
	size_t i = S_IDX(W2C_COORD(x), y, W2C_COORD(z));
	return (struct cell){1, c->ids[i], nibble_read(c->metadata, i),
		nibble_read(c->lighting_sky, i), nibble_read(c->lighting_torch, i)};
}
static uint8_t cell_plane(struct cell c, enum plane p) {
	return p == PLANE_ID ? c.id : p == PLANE_META ? c.meta : p == PLANE_SKY ? c.sky : c.blk;
}
static void describe_column(struct server_local* s, int cx, int cz, enum plane p,
	char* out, size_t n) {
	struct server_chunk* c = server_col(s, cx, cz);
	struct delivery* d = delivery_at(cx, cz);
	char server[256] = "server column absent (retired)";
	if(c) {
		const struct column_revision* r = c->backing ? &c->backing->revision : NULL;
		snprintf(server, sizeof server, "server column (%d,%d) rev owner/inc/content/light/events=%u/%u/%u/%u/%u settled_light_rev=%u sent=%d populated=%d",
			cx, cz, r ? r->owner : 0, r ? r->incarnation : 0, r ? r->content : 0, r ? r->light : 0,
			r ? r->events : 0, c->backing ? c->backing->settled_light_revision : 0, c->sent, c->populated);
	}
	char last[400] = "last delivery: none";
	const char* verdict = "never published to the client";
	if(d && d->any) {
		char when[32], cwhen[32];
		if(d->step == UINT_MAX) snprintf(when, sizeof when, "load");
		else snprintf(when, sizeof when, "step %u", d->step);
		if(d->chunk_step == UINT_MAX) snprintf(cwhen, sizeof cwhen, "load");
		else snprintf(cwhen, sizeof cwhen, "step %u", d->chunk_step);
		snprintf(last, sizeof last, "last delivery event: %s at %s update %u consumed=%d (chunks %u sets %u unloads %u); last CHUNK snapshot %s at %s update %u rev content/light/events=%u/%u/%u",
			rpc_type_name(d->type), when, d->update, d->consumed, d->chunks, d->sets, d->unloads,
			d->have_chunk ? "present" : "none", cwhen, d->chunk_update,
			d->chunk_rev.content, d->chunk_rev.light, d->chunk_rev.events);
		if(p == PLANE_HEIGHT) verdict = "height plane (no snapshot digest)";
		else if(!d->have_chunk) verdict = "no live CHUNK snapshot for this column";
		else {
			uint64_t client = digest_client(cx, cz, p);
			uint64_t srv = c ? digest_server(c, p) : 0;
			if(client == d->sent[p] && c && srv != d->sent[p])
				verdict = "PUBLICATION LOSS: server bytes newer than the client's delivered copy";
			else if(client != d->sent[p] && c && srv == d->sent[p])
				verdict = "client copy diverged from an up-to-date delivered snapshot (client-side write/relight)";
			else if(client != d->sent[p])
				verdict = "client copy differs from its last delivered snapshot (discarded/overwritten delivery or client relight)";
			else verdict = "client equals delivered snapshot; server column absent";
		}
	}
	snprintf(out, n, "%s; %s; verdict: %s", server, last, verdict);
}
static void world_generations(struct server_local* s, char* out, size_t n) {
	struct client_world_context* ctx = client_ctx_registry_current(clin_world_contexts(), 0);
	snprintf(out, n, "world generation server=%u client ctx=%u/server_gen %u input_epoch=%u",
		s->worlds[0].generation, ctx ? ctx->generation : 0, ctx ? ctx->server_generation : 0,
		(unsigned)s->input_epoch_counter);
}
/* Before hashing the target's meshes: every mesher input of its column --
 * the 18x18x128 halo -- client vs server, then (ON) against the OFF run. */
static void compare_halo(struct server_local* s, bool retire, unsigned step,
	unsigned target, int cx, int cz) {
	unsigned diffs[PLANES] = {0}, client_absent = 0, server_absent = 0;
	bool first = true;
	char gen[160]; world_generations(s, gen, sizeof gen);
	static struct cell now[2][HALO_CELLS];
	static uint8_t now_height[2][HALO * HALO], now_tint[HALO * HALO][2];
	for(int hx = 0; hx < HALO; hx++) for(int hz = 0; hz < HALO; hz++) {
		int x = cx * 16 - 1 + hx, z = cz * 16 - 1 + hz;
		size_t col = (size_t)hx * HALO + (size_t)hz;
		struct server_chunk* sc = server_col(s, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z));
		bool client_has = world_find_chunk(&gstate.world, x, 0, z) != NULL;
		uint8_t hc = client_has ? (uint8_t)world_get_height(&gstate.world, x, z) : 0;
		uint8_t hs = sc ? sc->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE] : 0;
		now_height[0][col] = hc; now_height[1][col] = hs;
		now_tint[col][0] = now_tint[col][1] = 0;
		if(client_has) (void)world_get_section_tint_at(&gstate.world, x, z, &now_tint[col][0], &now_tint[col][1]);
		if(client_has && sc && hc != hs) {
			if(first) {
				char why[1200]; describe_column(s, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z), PLANE_HEIGHT, why, sizeof why);
				printf("G9X-6 RED: run=%s step=%u target=(%d,%d) first divergence (%d,*,%d) plane=height expected(server)=%u actual(client)=%u; %s; %s\n",
					cur_run, step, cx, cz, x, z, hs, hc, gen, why);
				first = false;
			}
			diffs[PLANE_HEIGHT]++;
		}
		for(int y = 0; y < WORLD_HEIGHT; y++) {
			size_t i = col * WORLD_HEIGHT + (size_t)y;
			struct cell cl = client_cell(x, y, z), sv = server_cell(s, x, y, z);
			now[0][i] = cl; now[1][i] = sv;
			if(!cl.present) { client_absent++; continue; }
			if(!sv.present) { server_absent++; continue; }
			for(enum plane p = PLANE_ID; p <= PLANE_BLOCK; p++) {
				if(cell_plane(cl, p) == cell_plane(sv, p)) continue;
				if(first) {
					char why[1200]; describe_column(s, WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z), p, why, sizeof why);
					printf("G9X-6 RED: run=%s step=%u target=(%d,%d) first divergence (%d,%d,%d) plane=%s expected(server)=%u actual(client)=%u; %s; %s\n",
						cur_run, step, cx, cz, x, y, z, plane_name[p], cell_plane(sv, p), cell_plane(cl, p), gen, why);
					first = false;
				}
				diffs[p]++;
			}
		}
	}
	unsigned total = diffs[0] + diffs[1] + diffs[2] + diffs[3] + diffs[4];
	red_stale_total += total;
	printf("%s run=%s step=%u target=(%d,%d) mesher halo client-vs-server: id %u meta %u height %u Sky %u Block %u (client-absent cells %u, server-absent cells %u)\n",
		total ? "G9X-6 RED:" : "G9X-6 halo ok:", cur_run, step, cx, cz, diffs[PLANE_ID], diffs[PLANE_META],
		diffs[PLANE_HEIGHT], diffs[PLANE_SKY], diffs[PLANE_BLOCK], client_absent, server_absent);
	if(!retire) {
		memcpy(halo_ref[target], now, sizeof now);
		memcpy(halo_ref_height[target], now_height, sizeof now_height);
		memcpy(halo_ref_tint[target], now_tint, sizeof now_tint);
		halo_ref_valid[target] = true;
	} else if(halo_ref_valid[target]) {
		/* Cross-run: which mesher input differs between OFF and ON? */
		bool voxels_equal = true;
		for(unsigned side = 0; side < 2; side++) {
			/* Residency is excluded (the optimization moves support to disk):
			 * a column present in only one run is reported, not compared. */
			unsigned n = 0, residency = 0; size_t firsti = SIZE_MAX;
			for(size_t i = 0; i < HALO_CELLS; i++) {
				const struct cell* a = &halo_ref[target][side][i];
				const struct cell* b = &now[side][i];
				if(a->present != b->present) { residency++; continue; }
				if(memcmp(a, b, sizeof(struct cell))) {
					if(firsti == SIZE_MAX) firsti = i;
					n++;
				}
			}
			unsigned hn = 0;
			for(size_t c = 0; c < HALO * HALO; c++)
				hn += halo_ref[target][side][c * WORLD_HEIGHT].present
					&& now[side][c * WORLD_HEIGHT].present
					&& halo_ref_height[target][side][c] != now_height[side][c];
			if(residency)
				printf("G9X-6 note: run=%s step=%u target=(%d,%d) %s halo OFF-vs-ON: %u cells resident in one run only (retired support, not compared)\n",
					cur_run, step, cx, cz, side ? "SERVER" : "CLIENT", residency);
			if(n || hn) {
				voxels_equal = false;
				if(firsti != SIZE_MAX) {
					size_t col = firsti / WORLD_HEIGHT;
					int x = cx * 16 - 1 + (int)(col / HALO), z = cz * 16 - 1 + (int)(col % HALO);
					int y = (int)(firsti % WORLD_HEIGHT);
					struct cell a = halo_ref[target][side][firsti], b = now[side][firsti];
					printf("G9X-6 RED: run=%s step=%u target=(%d,%d) %s halo OFF-vs-ON: %u cells, %u heights differ; first (%d,%d,%d) OFF present/id/meta/Sky/Block=%u/%u/%u/%u/%u ON=%u/%u/%u/%u/%u\n",
						cur_run, step, cx, cz, side ? "SERVER" : "CLIENT", n, hn, x, y, z,
						a.present, a.id, a.meta, a.sky, a.blk, b.present, b.id, b.meta, b.sky, b.blk);
				} else
					printf("G9X-6 RED: run=%s step=%u target=(%d,%d) %s halo OFF-vs-ON: heights differ in %u columns\n",
						cur_run, step, cx, cz, side ? "SERVER" : "CLIENT", hn);
			}
		}
		if(voxels_equal) {
			unsigned tn = 0;
			for(size_t c = 0; c < HALO * HALO; c++)
				tn += memcmp(halo_ref_tint[target][c], now_tint[c], 2) != 0;
			printf("G9X-6 halo ok: run=%s step=%u target=(%d,%d) every voxel mesher input agrees OFF-vs-ON (client and server halo); client tint columns differing: %u\n",
				cur_run, step, cx, cz, tn);
		}
	}
	fflush(stdout);
}
/* Every published column in the window: client copy vs server stored planes. */
static void compare_view(struct server_local* s, unsigned step) {
	unsigned long cells[PLANES] = {0};
	unsigned columns = 0, stale_columns = 0, loss = 0, copy = 0, other = 0;
	/* first divergence per plane (id/meta stale bytes are a distinct gap) */
	bool first_plane[4] = {true, true, true, true};
	char gen[160]; world_generations(s, gen, sizeof gen);
	for(int cx = WIN_X0; cx < WIN_X0 + WIN_W; cx++) for(int cz = WIN_Z0; cz < WIN_Z0 + WIN_H; cz++) {
		struct server_chunk* sc = server_col(s, cx, cz);
		if(!sc || !world_find_chunk(&gstate.world, cx * 16, 0, cz * 16)) continue;
		columns++;
		bool stale = false, plane_stale[4] = {0};
		for(int lx = 0; lx < 16; lx++) for(int lz = 0; lz < 16; lz++) {
			int x = cx * 16 + lx, z = cz * 16 + lz;
			if((uint8_t)world_get_height(&gstate.world, x, z) != sc->heightmap[lx + lz * CHUNK_SIZE]) {
				cells[PLANE_HEIGHT]++; stale = true;
			}
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				struct cell cl = client_cell(x, y, z), sv = server_cell(s, x, y, z);
				for(enum plane p = PLANE_ID; p <= PLANE_BLOCK; p++) {
					if(cell_plane(cl, p) == cell_plane(sv, p)) continue;
					cells[p]++; stale = plane_stale[p] = true;
					if(first_plane[p]) {
						char why[1200]; describe_column(s, cx, cz, p, why, sizeof why);
						printf("G9X-6 RED: run=%s %s%u view first divergence (%d,%d,%d) plane=%s expected(server)=%u actual(client)=%u; %s; %s\n",
							cur_run, step == UINT_MAX ? "after-load" : "step=", step == UINT_MAX ? 0 : step,
							x, y, z, plane_name[p], cell_plane(sv, p), cell_plane(cl, p), gen, why);
						first_plane[p] = false;
					}
				}
			}
		}
		if(!stale) continue;
		stale_columns++;
		struct delivery* d = delivery_at(cx, cz);
		enum plane p = plane_stale[PLANE_SKY] ? PLANE_SKY : plane_stale[PLANE_BLOCK] ? PLANE_BLOCK
			: plane_stale[PLANE_ID] ? PLANE_ID : PLANE_META;
		if(!d || !d->have_chunk) { other++; continue; }
		uint64_t client = digest_client(cx, cz, p), srv = digest_server(sc, p);
		if(client == d->sent[p] && srv != d->sent[p]) loss++;
		else if(client != d->sent[p]) copy++;
		else other++;
	}
	unsigned long total = cells[0] + cells[1] + cells[2] + cells[3] + cells[4];
	printf("%s run=%s %s%u view: %u published columns, %u stale; stale cells id %lu meta %lu height %lu Sky %lu Block %lu; publication-loss columns %u, delivered-copy-mismatch columns %u, other %u\n",
		total ? "G9X-6 RED:" : "G9X-6 view ok:", cur_run, step == UINT_MAX ? "after-load" : "step=",
		step == UINT_MAX ? 0 : step, columns, stale_columns, cells[PLANE_ID], cells[PLANE_META],
		cells[PLANE_HEIGHT], cells[PLANE_SKY], cells[PLANE_BLOCK], loss, copy, other);
	red_stale_total += total;
	fflush(stdout);
}
static uint64_t rendered_column(int x, int z) {
	uint64_t h = UINT64_C(14695981039346656037);
	for(int y = 0; y < WORLD_HEIGHT; y += 16) {
		struct chunk* c = world_find_chunk(rendered, x * 16, y, z * 16);
		assert(c);
		c->dirty_generation++;
		assert(chunk_mesher_send(c));
		c->mesh_in_flight = true;
		c->rebuild_displist = false;
		c->first_meshed = true;
		/* Guards a genuinely hung/deadlocked mesher worker (mesh_in_flight
		 * never clears). Was a 30-WALL-SECOND bound, which false-triggers
		 * under `ctest -jN`: this thread can be preempted off-CPU by sibling
		 * test processes for well over 30s of real time while doing nothing
		 * wrong, and pure wall-clock can't tell that apart from a real hang.
		 * Rule now: bound the number of times THIS thread actually executes
		 * the loop body (an iteration budget), not how much wall-clock ticks
		 * by while it isn't scheduled at all. Preemption gaps between
		 * iterations cost nothing against the budget; only iterations this
		 * thread truly runs do. At 1ms/iteration that is still ~30s of this
		 * thread's own CPU time before tripping -- ample for one chunk mesh,
		 * which normally completes in low milliseconds. */
		enum { MESH_WAIT_MAX_ITERS = 30000u };
		unsigned wait_iters = 0;
		while(c->mesh_in_flight) {
			chunk_mesher_receive();
			assert(++wait_iters < MESH_WAIT_MAX_ITERS);
			if(c->mesh_in_flight) thread_msleep(1);
		}
		for(unsigned k = 0; k < CHUNK_MESH_LISTS; k++) {
			h = hash_bytes(h, &c->has_displist[k], sizeof(c->has_displist[k]));
			if(!c->has_displist[k]) continue;
			struct displaylist* l = &c->mesh[k];
			h = hash_bytes(h, &l->index, sizeof(l->index));
			h = hash_bytes(h, l->data, l->index * (l->tinted ? 25u : 22u));
		}
		h = hash_bytes(h, c->reachable, sizeof(c->reachable));
	}
	return h;
}
static uint64_t server_column_planes(const struct server_chunk* c) {
	uint64_t h = UINT64_C(14695981039346656037);
	h = hash_bytes(h, c->ids, 32768);
	h = hash_bytes(h, c->metadata, 16384);
	h = hash_bytes(h, c->lighting_sky, 16384);
	h = hash_bytes(h, c->lighting_torch, 16384);
	h = hash_bytes(h, c->heightmap, 256);
	h = hash_bytes(h, &c->populated, sizeof(c->populated));
	return h;
}
static uint64_t scheduled_hash(struct server_local* s) {
	uint64_t hash = UINT64_C(14695981039346656037);
	hash = hash_bytes(hash, &s->world.scheduled_tick_count, sizeof(s->world.scheduled_tick_count));
	for(size_t i = 0; i < s->world.scheduled_tick_count; i++) {
		const struct scheduled_tick* t = &s->world.scheduled_ticks[
			(s->world.scheduled_tick_head + i) % s->world.scheduled_tick_capacity];
		hash = hash_bytes(hash, &t->x, sizeof(t->x));
		hash = hash_bytes(hash, &t->y, sizeof(t->y));
		hash = hash_bytes(hash, &t->z, sizeof(t->z));
		hash = hash_bytes(hash, &t->fire_at_tick, sizeof(t->fire_at_tick));
		hash = hash_bytes(hash, &t->block_type, sizeof(t->block_type));
		hash = hash_bytes(hash, &t->queue_flags, sizeof(t->queue_flags));
	}
	return hash;
}
static uint64_t planes_hash(struct server_local* s) {
	uint64_t hash = UINT64_C(14695981039346656037);
	for(int z = -MAX_VIEW_DISTANCE; z <= MAX_VIEW_DISTANCE; z++)
		for(int x = s->residency_anchors[0].x - MAX_VIEW_DISTANCE;
			x <= s->residency_anchors[1].x + MAX_VIEW_DISTANCE; x++) {
			if(!residency_contains(s->residency_anchors, 2, MAX_VIEW_DISTANCE, x, z)) continue;
			struct server_chunk* c = dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(x, z));
			assert(c);
			hash = hash_bytes(hash, c->ids, 32768);
			hash = hash_bytes(hash, c->metadata, 16384);
			hash = hash_bytes(hash, c->lighting_sky, 16384);
			hash = hash_bytes(hash, c->lighting_torch, 16384);
			hash = hash_bytes(hash, c->heightmap, 256);
		}
	return hash;
}
static uint64_t near_hash(struct server_local* s) {
	uint64_t hash = UINT64_C(14695981039346656037);
	hash = hash_bytes(hash, &s->world.current_tick, sizeof(s->world.current_tick));
	hash = hash_bytes(hash, &s->rand_src.seed, sizeof(s->rand_src.seed));
	hash = hash_bytes(hash, &(uint64_t){scheduled_hash(s)}, sizeof(uint64_t));
	return hash_bytes(hash, &(uint64_t){planes_hash(s)}, sizeof(uint64_t));
}
static bool scheduled_tick_equal(const struct scheduled_tick* a,
	const struct scheduled_tick* b) {
	return a->x == b->x && a->y == b->y && a->z == b->z
		&& a->fire_at_tick == b->fire_at_tick
		&& a->block_type == b->block_type && a->queue_flags == b->queue_flags;
}
static const struct scheduled_tick* scheduled_at(const struct server_world* w,
	size_t i) {
	return &w->scheduled_ticks[(w->scheduled_tick_head + i)
		% w->scheduled_tick_capacity];
}
static void continuation_record(struct server_local* s, unsigned index) {
	struct continuation_snapshot* out = &continuation[index];
	out->current_tick = s->world.current_tick;
	out->rand_seed = s->rand_src.seed;
	out->planes = planes_hash(s);
	out->scheduled_count = s->world.scheduled_tick_count;
	out->scheduled = malloc(out->scheduled_count * sizeof(*out->scheduled));
	assert(out->scheduled || !out->scheduled_count);
	for(size_t i = 0; i < out->scheduled_count; i++)
		out->scheduled[i] = *scheduled_at(&s->world, i);
}
static size_t continuation_first_scheduled_difference(
	const struct continuation_snapshot* expected, const struct server_world* actual) {
	size_t shared = expected->scheduled_count < actual->scheduled_tick_count
		? expected->scheduled_count : actual->scheduled_tick_count;
	for(size_t i = 0; i < shared; i++)
		if(!scheduled_tick_equal(&expected->scheduled[i], scheduled_at(actual, i)))
			return i;
	return shared;
}
/* Columns complete_hash had to re-admit from disk (absent at hash time). */
static unsigned hash_reloaded;
static uint64_t complete_hash(struct server_local* s) {
	/* Re-admit through production load, including TerrainPopulated and rearming. */
	assert(server_world_sync_regions(&s->world));
	uint64_t hash = UINT64_C(14695981039346656037);
	int r = MAX_VIEW_DISTANCE + 1;
	for(int z = -r; z <= r; z++) for(int x = -r; x <= 16 + r; x++) {
		if(!residency_contains(s->residency_anchors, 2, r, x, z)) continue;
		struct server_chunk* c = dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(x, z));
		if(!c) {
			server_world_begin_update(&s->world);
			assert(server_world_load_chunk(&s->world, x, z, &c));
			assert(c);
			hash_reloaded++;
		}
		hash = hash_bytes(hash, c->ids, 32768);
		hash = hash_bytes(hash, c->metadata, 16384);
		hash = hash_bytes(hash, c->lighting_sky, 16384);
		hash = hash_bytes(hash, c->lighting_torch, 16384);
		hash = hash_bytes(hash, c->heightmap, 256);
		hash = hash_bytes(hash, &c->populated, sizeof(c->populated));
	}
	return hash;
}
static uint64_t run(bool retire, bool armed, enum continuation_mode continuation_mode,
	uint64_t* high) {
	server_local_test_startup_retirement(retire);
	server_world_test_population_calls = 0;
	if(armed) assert(carve_observer_start());
	struct server_local* s = split_fixture();
	/* Fresh seed 42 terrain, unchanged production near radius on this platform. */
	server_world_discard(&s->world);
	/* RETIRE-LAVA-PLAN step 3: World.rand (server_world.c runtime_jrand_seed)
	 * is otherwise seeded from a process-wide uniquifier, so every run() would
	 * start its population-fluid draws (Beta lava's nextInt(4) flowing->still
	 * branch) from a DIFFERENT state. Pin it for the replacement world so OFF
	 * and ON compare matched inputs. */
	server_world_test_seed_runtime_jrand(&s->worlds[0].rand, INT64_C(0x5EED2E71));
	string_t path; string_init_set_str(path, split_path);
	assert(server_world_create_with_populate_scratch(&s->world, path,
		WORLD_DIM_OVERWORLD, 42, false, s->world_populate_scratch));
	string_clear(path);
	{
		/* Every run must start from the identical simulation state. */
		const struct server_world* w0 = &s->worlds[0];
		struct start_state now = {
			.rand = w0->rand.seed,
			.hell_rand = w0->hell_populate_rng.seed,
			.tick = w0->current_tick,
			.scheduled = w0->scheduled_tick_count,
			.chunks = dict_server_chunks_size(w0->chunks),
		};
		if(!have_start) { start = now; have_start = true; }
		assert(now.rand == start.rand && now.hell_rand == start.hell_rand
			   && now.tick == start.tick && now.scheduled == start.scheduled
			   && now.chunks == start.chunks);
	}
	s->world.local = s;
	s->worlds[0].session = s; /* A1a (D1): owning session for services/callbacks */
	s->render_demo_mob = -1;
	s->input_ack_pending = 1; /* hold simulation until the full replay loads */
	s->player.finished_loading = true; /* model the issued PLAYER_POS handoff */
	s->rand_src = (struct random_gen){.seed = 12345};
	*server_local_sound_rng() = (struct random_gen){.seed = 6789};
	*audio_cosmetic_rng() = (struct random_gen){.seed = 9876};
	server_local_test_seed_math_random(7654);
	s->far_view_distance = MAX_VIEW_DISTANCE + 1;
	s->far_lod_terrain_only = false;
	assert(server_local_set_anchor_sep(s, 256));
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	/* G9X-6: the real client world, reset like a session start (the
	 * server's own stamped session WORLD_RESET, server_local.c:19129). */
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset.dimension = WORLD_DIM_OVERWORLD,
		.payload.world_reset.world_index = s->worlds[0].world_index,
		.payload.world_reset.world_generation = s->worlds[0].generation});
	memset(deliveries, 0, sizeof deliveries);
	cur_step = UINT_MAX; cur_update = 0;
	cur_run = retire ? (continuation_mode == CONTINUATION_PIN_ON ? "ON/pin" : "ON")
		: continuation_mode == CONTINUATION_PIN_RECORD ? "OFF/pin-record"
		: continuation_mode == CONTINUATION_RECORD ? "OFF/record"
		: continuation_mode == CONTINUATION_COMPARE_OFF ? "OFF/compare" : "OFF";
	clin_test_set_consume_observer(note_consume, NULL);
	clin_test_set_rpc_observer(receive, NULL);
	rendered = &gstate.world;
	assert(chunk_mesher_init());
	stream = UINT64_C(14695981039346656037); sends = 0;
	unsigned load = 0;
	do {
		server_update(s);
		carve_observer_frame(rendered, load, load, 0, false);
		assert(++load < 1000);
	} while(s->anchor_near_owed[0] || s->anchor_near_owed[1]);
	/* One final retirement opportunity before the re-admission boundary. */
	server_update(s);
	if(retire && continuation_mode == CONTINUATION_PIN_ON) {
		/* The timing budget intentionally changes residency, so drain every
		 * presently eligible support column before taking a deterministic golden. */
		for(unsigned pass = 0; pass < 1000; pass++) {
			uint32_t before = s->startup_retired;
			server_local_test_retire_startup_support(s);
			if(s->startup_retired == before) break;
			assert(pass + 1 < 1000);
		}
	}
	carve_observer_frame(rendered, load, load, 1, true);
	if(armed) {
		struct carve_attrib a; assert(carve_observer_attrib(0, &a));
		*high = 0;
		for(unsigned c = 0; c < CARVE_CLASSES; c++)
			for(unsigned p = 0; p < 3; p++) *high += a.bytes[c][p];
		assert(!a.errors);
		struct carve_totals census; carve_observer_snapshot(&census);
		uint32_t live, shared, counted = 0;
		column_backing_get_stats(&live, &shared);
		for(unsigned c = 0; c < 5; c++) counted += census.attribution.columns[c];
		assert(counted == live && census.count[CARVE_COLUMN] == (uint64_t)live * 5);
		const char* dir = getenv("CW_GATE_ATTRIB_DIR");
		if(dir) {
			char path[1024];
			int n = snprintf(path, sizeof(path), "%s/retire_%u.ndjson", dir, retire);
			assert(n > 0 && (size_t)n < sizeof(path));
			attribution_file = fopen(path, "wb"); assert(attribution_file);
			assert(carve_observer_emit(rendered, load, true, attribution_line));
			assert(fclose(attribution_file) == 0); attribution_file = NULL;
		}
	}
	printf("loading retire=%u armed=%u updates=%u sends=%u payload_KiB=%.3f stream=%016" PRIx64 "\n",
		retire, armed, load, sends, (double)*high / 1024, stream); fflush(stdout);
	uint64_t loading_stream = stream;
	if(continuation_mode == CONTINUATION_PIN_RECORD) {
		pin_complete = complete_hash(s);
		s->input_ack_pending = 0;
		s->player.finished_loading = true;
		for(unsigned tick = 0; tick < CONTINUATION_TICKS; tick++) {
			server_update(s);
			continuation_record(s, tick);
		}
		goto verified;
	}
	if(continuation_mode == CONTINUATION_PIN_ON) {
		/* RETIRE-LIGHT: equality must not pass because retirement never ran.
		 * The ON run really retires support columns, and complete_hash really
		 * re-admits retired columns from disk before comparing them. */
		uint32_t retired_on = s->startup_retired;
		hash_reloaded = 0;
		uint64_t all = complete_hash(s);
		printf("retirement executed: startup_retired=%u reloaded_for_compare=%u\n",
			retired_on, hash_reloaded); fflush(stdout);
		assert(retired_on > 0 && hash_reloaded > 0);
		assert(pin_complete == all);
		s->input_ack_pending = 0;
		s->player.finished_loading = true;
		unsigned first_difference = UINT_MAX;
		size_t first_scheduled = SIZE_MAX;
		for(unsigned tick = 0; tick < CONTINUATION_TICKS; tick++) {
			server_update(s);
			const struct continuation_snapshot* expected = &continuation[tick];
			uint64_t actual_planes = planes_hash(s);
			bool queue_equal = expected->scheduled_count == s->world.scheduled_tick_count;
			if(queue_equal)
				for(size_t i = 0; i < expected->scheduled_count; i++)
					if(!scheduled_tick_equal(&expected->scheduled[i], scheduled_at(&s->world, i))) {
						queue_equal = false;
						break;
					}
			{
				bool equal = s->world.current_tick == expected->current_tick
					&& s->rand_src.seed == expected->rand_seed && queue_equal
					&& actual_planes == expected->planes;
				if(!equal && first_difference == UINT_MAX) {
					first_difference = s->world.current_tick;
					first_scheduled = continuation_first_scheduled_difference(expected, &s->world);
					const struct scheduled_tick* off = first_scheduled < expected->scheduled_count
						? &expected->scheduled[first_scheduled] : NULL;
					const struct scheduled_tick* on = first_scheduled < s->world.scheduled_tick_count
						? scheduled_at(&s->world, first_scheduled) : NULL;
					fprintf(stderr, "startup divergence tick=%u continuation=%u entry=%zu off=%u@(%d,%d,%d)/%u on=%u@(%d,%d,%d)/%u seed=%" PRIu64 "/%" PRIu64 "\n",
						first_difference, tick, first_scheduled,
						off ? off->block_type : 0, off ? off->x : 0, off ? off->y : 0,
						off ? off->z : 0, off ? off->fire_at_tick : 0,
						on ? on->block_type : 0, on ? on->x : 0, on ? on->y : 0,
						on ? on->z : 0, on ? on->fire_at_tick : 0,
						expected->rand_seed, (uint64_t)s->rand_src.seed);
					/* Retirement ON is diagnostic and remains disabled by default.
					 * Timed admission can change whether divergence is observed here. */
				}
			}
		}
		if(first_difference == UINT_MAX)
			puts("startup retirement ON diagnostic: no divergence observed in 600 ticks");
		goto verified;
	}
	const int readmit_x[2] = {-MAX_VIEW_DISTANCE - 1,
		16 + MAX_VIEW_DISTANCE + 1};
	for(unsigned i = 0; i < 2; i++) {
		struct server_chunk* c = dict_server_chunks_get(
			s->world.chunks, S_CHUNK_ID(readmit_x[i], 0));
		if(retire) assert(!c); /* saved and backing released before movement */
		else assert(c);
	}
	/* G9X-6 RED: already-published columns after loading + retirement. */
	{
		const struct client_ctx_registry* reg = clin_world_contexts();
		printf("G9X-6 client routing:");
		for(unsigned k = 0; k < CLIENT_CTX_ROUTE_COUNT; k++) printf(" %u", reg->routed[k]);
		printf("\n");
	}
	compare_view(s, UINT_MAX);
	/* Move both anchors one column west, then east past their original
	 * support edge, then back. The real scheduler admits retired support;
	 * no archive-reader or bulk reload runs before these admissions. */
	const int shifts[4] = {-1, 0, 1, 0};
	for(unsigned step = 0; step < 4; step++) {
		s->player.x = shifts[step] * 16 + 0.5;
		cur_step = step;
		unsigned updates = 0;
		do {
			server_update(s);
			assert(++updates < 1000);
		} while(s->anchor_near_owed[0] || s->anchor_near_owed[1]);
		int target = step == 0 ? 0 : step == 2 ? 1 : -1;
		if(target >= 0) {
			struct server_chunk* c = dict_server_chunks_get(
				s->world.chunks, S_CHUNK_ID(readmit_x[target], 0));
			assert(c && c->populated);
			uint64_t planes = server_column_planes(c);
			/* G9X-6 RED: every mesher input before the mesh is hashed. */
			compare_halo(s, retire, step, (unsigned)target, readmit_x[target], 0);
			uint64_t mesh = rendered_column(readmit_x[target], 0);
			if(retire)
				printf("G9X-6 target=(%d,0) step=%u: server planes %s, mesh %s (OFF %016" PRIx64 " ON %016" PRIx64 ")\n",
					readmit_x[target], step, planes == readmitted_planes[target] ? "equal" : "DIFFER",
					mesh == readmitted_mesh[target] ? "equal" : "DIFFER", readmitted_mesh[target], mesh);
			fflush(stdout);
			if(!retire) {
				readmitted_planes[target] = planes;
				readmitted_mesh[target] = mesh;
				readmitted_cursor[target] = c->load_tick_rearm_cursor;
			} else {
				assert(planes == readmitted_planes[target]);
				assert(mesh == readmitted_mesh[target]);
				assert(c->load_tick_rearm_cursor == readmitted_cursor[target]);
			}
		}
		compare_view(s, step);
		uint64_t h = near_hash(s);
		if(!retire) { moving[step] = h; populations[step] = server_world_test_population_calls; }
		else {
			if(h != moving[step] || populations[step] != server_world_test_population_calls)
				fprintf(stderr, "reentry step=%u planes/queue/rearm=%016" PRIx64 "/%016" PRIx64 " populate=%u/%u\n", step, moving[step], h, populations[step], server_world_test_population_calls);
			assert(h == moving[step]);
			assert(populations[step] == server_world_test_population_calls);
		}
	}
	uint64_t all = complete_hash(s);
	if(!retire) complete = all;
	else assert(complete == all);
	if(continuation_mode != CONTINUATION_NONE) {
		/* Compare two independent retirement-OFF continuations tick for tick. */
		server_local_test_startup_retirement(false);
		/* The production far stream is wall-clock paced. Merged persistence I/O
		 * shifted where its 8 ms boundary falls, exposing the variable admission
		 * count in back-to-back test updates. Fix that test-only input at one while
		 * comparing simulation snapshots. */
		server_local_test_far_gen_one_per_update(true);
		s->input_ack_pending = 0;
		s->player.finished_loading = true;
		for(unsigned tick = 0; tick < CONTINUATION_TICKS; tick++) {
			/* Isolate world tick/reload equivalence from the separately tested
			 * natural-spawn cadence, whose entity occupancy is outside this gate. */
			s->world_time = 1;
			server_update(s);
			if(continuation_mode == CONTINUATION_RECORD) {
				continuation_record(s, tick);
				continue;
			}
			const struct continuation_snapshot* expected = &continuation[tick];
			assert(s->world.current_tick == expected->current_tick);
			assert(s->rand_src.seed == expected->rand_seed);
			assert(s->world.scheduled_tick_count == expected->scheduled_count);
			for(size_t i = 0; i < expected->scheduled_count; i++)
				assert(scheduled_tick_equal(&expected->scheduled[i],
					scheduled_at(&s->world, i)));
			assert(planes_hash(s) == expected->planes);
			continuation_comparisons++;
		}
	}
verified:
	server_local_test_far_gen_one_per_update(false);
	assert(!mem2_stat_oom() && !mem2_canary_hits()
		&& !__atomic_load_n(&g_mesh_truncations, __ATOMIC_RELAXED));
	clin_test_set_rpc_observer(NULL, NULL);
	client_pump();
	clin_test_set_consume_observer(NULL, NULL);
	chunk_mesher_shutdown();
	rendered = NULL;
	world_unload_all(&gstate.world);
	split_destroy(s);
	clin_drain_discard();
	if(armed) assert(carve_observer_stop());
	return loading_stream;
}
static void ownership_gate(void) {
	struct server_local* s = split_fixture();
	assert(server_local_set_anchor_sep(s, 256));
	s->residency_anchors[0] = (struct residency_anchor){0, 0};
	s->residency_anchors[1] = (struct residency_anchor){16, 0};
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	int x = -MAX_VIEW_DISTANCE - 1;
	struct server_chunk* c = split_populate(s, x, 0);
	assert(server_local_startup_class(s, x, 0) == CARVE_A_DEPENDENCY);
	for(int z = -3; z <= 3; z++) for(int dx = 1; dx <= 3; dx++) {
		struct server_chunk* near = split_populate(s, x + dx, z);
		near->sent = true;
	}
	/* Re-find after dictionary growth. No borrowed struct pointer survives it. */
	c = dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(x, 0));
	assert(server_local_startup_class(s, x, 0) == CARVE_A_SUPPORT_READY);
	/* Before any setter runs, eligible support must stay resident by default. */
	uint32_t retired_before = s->startup_retired;
	server_local_test_retire_startup_support(s);
	assert(s->startup_retired == retired_before);
	assert(dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(x, 0)) == c);
	server_local_test_startup_retirement(true);
	column_backing_ref(c->backing); /* transport/view/worker lease */
	assert(server_local_startup_class(s, x, 0) == CARVE_A_LEASED);
	column_backing_unref(c->backing);
	s->world.gen_pending[1] = true;
	s->world.gen_pending_x[1] = x;
	s->world.gen_pending_z[1] = 0;
	assert(server_local_startup_class(s, x, 0) == CARVE_A_DEPENDENCY);
	s->world.gen_pending[1] = false;
	assert(server_local_startup_class(s, -MAX_VIEW_DISTANCE, 0) == CARVE_A_PLAYER);
	assert(split_populate(s, 16, 0));
	assert(server_local_startup_class(s, 16, 0) == CARVE_A_PLAYER);
	c = dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(x, 0));
	struct column_backing* backing = c->backing;
	c->modified = true;
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
	server_local_test_retire_startup_support(s);
	c = dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(x, 0));
	assert(c && c->backing == backing && c->modified);
	assert(s->startup_save_failed == 1 && s->startup_retired == 0);
	s->chunk_save_retry_delay = 0;
	server_local_test_retire_startup_support(s);
	assert(s->startup_retired > 0);
	assert(!dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(x, 0)));
	struct server_chunk* loaded;
	assert(server_world_load_chunk(&s->world, x, 0, &loaded));
	assert(loaded && loaded->populated);
	split_destroy(s);
}
static void io_batch(void) {
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	for(int x = 0; x < 8; x++) assert(split_populate(s, x, 0));
	ptime_t start = time_get();
	float maximum = 0;
	for(int x = 0; x < 8; x++) {
		ptime_t one = time_get();
		assert(server_world_save_chunk(&s->world, false, x, 0));
		float ms = time_diff_s(one, time_get()) * 1000;
		if(ms > maximum) maximum = ms;
	}
	printf("IO eight-column compression+write batch %.3f ms; max column %.3f ms\n",
		time_diff_s(start, time_get()) * 1000, maximum);
	puts("IO bound: platform clock, no PC-to-Broadway scaling assumption; after one save, start no next column once batch elapsed reaches 2.000 ms (eight hard cap)");
	fflush(stdout);
	split_destroy(s);
}
/* MAINTAINER RULING 2026-09-11: runtime split server load radius. The sweep
 * keeps the (radius+1) support ring, so the dial moves the ring it protects: a
 * column 5 out from an anchor was a PLAYER column under MAX_VIEW_DISTANCE and
 * is retirable support under dial 4 -- unless the OTHER anchor's dial still
 * claims it, which is the union protection this pins. */
static void dial_retirement_ring(void) {
	server_local_test_startup_retirement(true);
	struct server_local* s = split_fixture();
	s->split_view_radius = 4;
	assert(server_local_set_anchor_sep(s, 144)); /* 9 chunks apart */
	assert(server_local_view_radius(s) == 4);
	s->residency_anchors[0] = (struct residency_anchor){0, 0};
	s->residency_anchors[1] = (struct residency_anchor){9, 0};
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	const int support_x = -5; /* |dx| = 4 + 1 from P0, 14 from P1 */
	assert(split_populate(s, support_x, 0));
	/* ensure_populated's neighbourhood inside P0's dial must be sent, or the
	 * column classes as a dependency rather than retirable support. */
	for(int z = -3; z <= 3; z++) for(int dx = 1; dx <= 3; dx++) {
		struct server_chunk* near = split_populate(s, support_x + dx, z);
		near->sent = true;
	}
	const int kept_x = 5; /* same |dx| = 5 from P0, but 4 from P1 */
	assert(split_populate(s, kept_x, 0));
	assert(server_local_startup_class(s, support_x, 0) == CARVE_A_SUPPORT_READY);
	assert(server_local_startup_class(s, kept_x, 0) == CARVE_A_PLAYER);
	/* Drain: the per-call budget is wall-clock timed, so one call proves the
	 * retirement but not the retention. */
	for(unsigned pass = 0; pass < 1000; pass++) {
		uint32_t before = s->startup_retired;
		server_local_test_retire_startup_support(s);
		if(s->startup_retired == before) break;
		assert(pass + 1 < 1000);
	}
	assert(s->startup_retired > 0 && !s->startup_save_failed);
	assert(!dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(support_x, 0)));
	assert(dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(kept_x, 0)));
	struct server_chunk* loaded;
	assert(server_world_load_chunk(&s->world, support_x, 0, &loaded));
	assert(loaded && loaded->populated);
	server_local_test_startup_retirement(false);
	split_destroy(s);
	puts("PASS dial 4: the startup sweep retires the 4+1 support ring; the "
		 "other anchor's dial keeps the same distance resident");
}
int main(void) {
	blocks_init(); assert(clin_init());
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	if(getenv("G9X6_DRAIN_AFTER_UPDATE")) drain_in_observer = false;
	ownership_gate();
	io_batch();
	/* Wall-clock admission changes which diagnostic candidate columns retire.
	 * Pin one production save per boundary so the divergence golden is stable. */
	server_local_test_startup_retirement_limit(8);
	uint64_t before = 0, after = 0, unused = 0;
	uint64_t baseline = run(false, true, CONTINUATION_PIN_RECORD, &before);
	assert(run(true, true, CONTINUATION_PIN_ON, &after) == baseline);
	for(unsigned tick = 0; tick < CONTINUATION_TICKS; tick++) {
		free(continuation[tick].scheduled);
		continuation[tick].scheduled = NULL;
	}
	baseline = run(false, true, CONTINUATION_RECORD, &unused);
	assert(run(false, true, CONTINUATION_COMPARE_OFF, &unused) == baseline);
	assert(continuation_comparisons == CONTINUATION_TICKS);
	server_local_test_startup_retirement_limit(0);
	baseline = run(false, false, CONTINUATION_NONE, &unused);
	assert(run(true, false, CONTINUATION_NONE, &unused) == baseline);
	for(unsigned tick = 0; tick < CONTINUATION_TICKS; tick++)
		free(continuation[tick].scheduled);
	/* G9X-6: every published client copy equals the server's stored planes. */
	printf("G9X-6 total client-vs-server stale cells across runs: %lu\n", red_stale_total);
	assert(red_stale_total == 0);
	printf("PASS startup far6 voxel replay + moving two-anchor production re-admission, default OFF and %u OFF comparisons; ON continuation diagnostic only; PC routed payload %.3f -> %.3f KiB\n",
		continuation_comparisons, (double)before / 1024, (double)after / 1024);
	dial_retirement_ring();
	return 0;
}

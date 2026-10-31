/* Copyright (c) 2026 CaveWii_v2 contributors
 * Slice 1 evidence. These allocations are observer-owned, outside the memory
 * model. Server callbacks never read live client objects; main never reads
 * live server arrays. The mutex protects ONLY the observer's copied image.
 */
#include "column_observer.h"
#include "column_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "chunk.h"
#include "game/game_state.h"
#include "network/client_interface.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "world.h"

#define CELLS 32768u
#define HALF (CELLS / 2u)
#define RAW_SLICE 10240u
#define PLANES 5u

struct observed_column {
	int64_t key;
	bool present;
	const struct chunk* identity[8];
	uint8_t raw[8][RAW_SLICE];
	uint8_t ids[CELLS], meta[HALF], sky[HALF], torch[HALF], height[256];
	uint32_t hashes[PLANES];
};

/* Shadow identities only: never substitute these for an engine pointer or
 * retain any engine allocation. History prevents malloc address reuse and
 * same-coordinate reloads from hiding a lease. Counts change at existing
 * publication/release/constructor/destructor boundaries under the observer lock. */
struct observed_generation {
	int64_t key;
	const uint8_t* ids;
	bool server_live;
	unsigned client_slices, transports;
};
struct observed_shell { struct chunk* chunk; uint32_t generation; };
struct observed_slot { const void* slot; uint32_t generation; };

static struct {
	bool active, compare, failed, measuring;
	unsigned traces;
	struct thread_mutex lock;
	gzFile census, server, client, render;
	struct observed_column** columns;
	size_t count, capacity;
	struct observed_shell* shells;
	size_t shells_count, shells_capacity;
	struct observed_generation* generations;
	size_t generation_count, generation_capacity;
	struct observed_slot* tags;
	size_t tags_count, tags_capacity;
	uint32_t install_generation, retained_peak;
	uint32_t live_server_columns, live_full_slices;
	uint32_t peak_occupied_slices, peak_allocated_slices;
	uint32_t client_seq, server_seq, tick, reset, slots, slots_peak;
	uint32_t client_hash[PLANES], client_full, client_slices, orphan_slices;
	int dimension;
	struct server_world* live_server; /* server-thread-only lifecycle identity */
	uint64_t render_bytes, state_records, census_rows;
} obs;

static void allocation_peaks(void) {
	uint32_t base = obs.live_server_columns * 8 + obs.live_full_slices;
	uint32_t occupied = base + obs.slots * 8, allocated = base + 16 * 8;
	if(occupied > obs.peak_occupied_slices) obs.peak_occupied_slices = occupied;
	if(allocated > obs.peak_allocated_slices) obs.peak_allocated_slices = allocated;
}

static unsigned retained_generations(void) {
	unsigned n = 0;
	for(size_t i = 0; i < obs.generation_count; i++) {
		struct observed_generation* g = &obs.generations[i];
		if(!g->server_live && (g->client_slices || g->transports)) n++;
	}
	if(n > obs.retained_peak) obs.retained_peak = n;
	return n;
}

static void fail(const char* reason) {
	if(!obs.failed) fprintf(stderr, "[column-observer] FAIL: %s\n", reason);
	obs.failed = true;
}

static bool grow(void** ptr, size_t* capacity, size_t size, size_t needed) {
	if(needed <= *capacity) return true;
	if(*capacity > SIZE_MAX / 2) { fail("observer capacity overflow"); return false; }
	size_t n = *capacity ? *capacity * 2 : 32;
	if(n < needed) n = needed;
	if(size && n > SIZE_MAX / size) { fail("observer byte count overflow"); return false; }
	void* next = realloc(*ptr, n * size);
	if(!next) { fail("observer allocation"); return false; }
	*ptr = next;
	*capacity = n;
	return true;
}

/* Exact stream comparison, no pointer addresses, capacity slack, wall clocks
 * or uninitialized struct padding in the frozen format. */
static void stream(gzFile file, const void* data, size_t size) {
	if(!file || obs.failed) return;
	const uint8_t* p = data;
	uint8_t buffer[4096];
	while(size) {
		unsigned n = size < sizeof(buffer) ? (unsigned)size : sizeof(buffer);
		if(obs.compare) {
			int got = gzread(file, buffer, n);
			if(got != (int)n || memcmp(p, buffer, n)) {
				fprintf(stderr, "[column-observer] %s offset=%lld tick=%u client=%u\n",
					file == obs.server ? "server" : file == obs.client ? "client" : "render",
					(long long)gztell(file) - (got > 0 ? got : 0), obs.tick, obs.client_seq);
				fail("frozen trace differs (or ended early)");
				return;
			}
		} else if(gzwrite(file, p, n) != (int)n) {
			fail("trace write");
			return;
		}
		p += n;
		size -= n;
	}
}

static void trace_u32(gzFile f, uint32_t n) {
	uint8_t bytes[4] = {n, n >> 8, n >> 16, n >> 24};
	stream(f, bytes, sizeof(bytes));
}

static void key(gzFile f, int64_t n) {
	trace_u32(f, (uint32_t)n);
	trace_u32(f, (uint32_t)((uint64_t)n >> 32));
}

static struct observed_column* lookup(int64_t id) {
	for(size_t i = 0; i < obs.count; i++)
		if(obs.columns[i]->key == id) return obs.columns[i];
	return NULL;
}

static void lifetime(struct chunk* c, bool added) {
	/* Main-thread-only, including worker/GPU retirement at chunk_unref. */
	thread_mutex_lock(&obs.lock);
	if(added) {
		if(grow((void**)&obs.shells, &obs.shells_capacity, sizeof(*obs.shells),
				obs.shells_count + 1)) {
			obs.shells[obs.shells_count++] = (struct observed_shell){c, obs.install_generation};
			if(obs.install_generation)
				obs.generations[obs.install_generation - 1].client_slices++, obs.live_full_slices++;
		}
	} else {
		for(size_t i = 0; i < obs.shells_count; i++) {
			if(obs.shells[i].chunk == c) {
				uint32_t g = obs.shells[i].generation;
				if(g) obs.generations[g - 1].client_slices--, obs.live_full_slices--;
				obs.shells[i] = obs.shells[--obs.shells_count];
				break;
			}
		}
	}
	retained_generations();
	allocation_peaks();
	thread_mutex_unlock(&obs.lock);
}

static void transpose_slice(struct observed_column* out, struct chunk* c,
							unsigned cy) {
	/* Decode through the public reader. Slice 2 may change storage; this must
 * continue to hash the logical view, not assume the old interleaving. */
	bool shared = c->ownership == LOCAL_SHARED_VIEW;
	if(shared) thread_stripe_lock(c->backing->mutex);
	for(unsigned x = 0; x < 16; x++)
		for(unsigned z = 0; z < 16; z++)
			for(unsigned y = 0; y < 16; y += 2) {
				size_t i = (x * 16 + z) * 128 + cy * 16 + y;
				struct block_data a = shared ? column_view_get_locked(c->view, i)
					: chunk_get_block(c, x, y, z);
				struct block_data b = shared ? column_view_get_locked(c->view, i + 1)
					: chunk_get_block(c, x, y + 1, z);
				out->ids[i] = a.type;
				out->ids[i + 1] = b.type;
				out->meta[i / 2] = a.metadata | (b.metadata << 4);
				out->sky[i / 2] = a.sky_light | (b.sky_light << 4);
				out->torch[i / 2] = a.torch_light | (b.torch_light << 4);
			}
	if(shared) thread_stripe_unlock(c->backing->mutex);
}

static uint32_t keyed_hash(uint32_t h, int64_t id) {
	uint64_t value = (uint64_t)id ^ ((uint64_t)h << 32) ^ h;
	value ^= value >> 30;
	value *= UINT64_C(0xbf58476d1ce4e5b9);
	value ^= value >> 27;
	value *= UINT64_C(0x94d049bb133111eb);
	value ^= value >> 31;
	return (uint32_t)value ^ (uint32_t)(value >> 32);
}

static void hash_column(struct observed_column* c) {
	const uint8_t* planes[] = {c->ids, c->meta, c->sky, c->torch, c->height};
	const unsigned sizes[] = {CELLS, HALF, HALF, HALF, 256};
	for(unsigned p = 0; p < PLANES; p++)
		c->hashes[p] = (uint32_t)crc32(0, planes[p], sizes[p]);
}

static void client_snapshot(struct world* w, int event, bool single, int64_t only) {
	if(!obs.active || obs.traces == 2) return;
	thread_mutex_lock(&obs.lock);
	if(event == CRPC_WORLD_RESET) obs.reset++;
	obs.client_seq++;
	obs.dimension = w->dimension;
	obs.client_slices = obs.orphan_slices = 0;
	if(!single) memset(obs.client_hash, 0, sizeof(obs.client_hash));
	for(size_t i = 0; i < obs.count; i++) {
		struct observed_column* c = obs.columns[i];
		if(single && c->key != only) continue;
		c->present = false;
		if(single) for(unsigned p = 0; p < PLANES; p++)
			obs.client_hash[p] ^= keyed_hash(c->hashes[p], c->key);
	}
	dict_wsection_it_t it;
	for(dict_wsection_it(it, w->sections); !dict_wsection_end_p(it);
		dict_wsection_next(it)) {
		struct world_section* sec = &dict_wsection_ref(it)->value;
		if(sec->is_far_lod) continue;
		int64_t id = dict_wsection_ref(it)->key;
		if(single && id != only) continue;
		bool full = true;
		for(unsigned cy = 0; cy < 8; cy++)
			if(!sec->column[cy]) full = false;
		if(!full) continue; /* Partial allocations are counted below. */
		struct observed_column* c = lookup(id);
		bool added = c == NULL;
		if(!c) {
			if(!grow((void**)&obs.columns, &obs.capacity, sizeof(*obs.columns),
					 obs.count + 1)) break;
			c = calloc(1, sizeof(*c));
			if(!c) { fail("column image allocation"); break; }
			c->key = id;
			obs.columns[obs.count++] = c;
		}
		c->present = true;
		bool changed = added;
		struct column_view* view = sec->column[0]->view;
		for(unsigned cy = 1; cy < 8; cy++)
			if(sec->column[cy]->view != view) view = NULL;
		if(view) {
			/* Compare every real presented byte under the stripe, including
			 * unchanged/quiet pages. A dirty-generation shortcut here would hide
			 * a missing server write barrier from the golden. */
			changed = column_view_copy(view, c->ids, c->meta, c->sky, c->torch) || changed;
			memset(c->identity, 0, sizeof(c->identity));
		} else for(unsigned cy = 0; cy < 8; cy++) {
			struct chunk* shell = sec->column[cy];
			/* This cache is ONLY an observer optimization on the baseline owned
 * storage. Non-owned providers always decode. No hash decides equality. */
			if(c->identity[cy] != shell || !shell->blocks
			   || memcmp(c->raw[cy], shell->blocks, RAW_SLICE)) {
				transpose_slice(c, shell, cy);
				if(shell->blocks) memcpy(c->raw[cy], shell->blocks, RAW_SLICE);
				c->identity[cy] = shell;
				changed = true;
			}
		}
		if(memcmp(c->height, sec->heightmap, 256)) changed = true;
		memcpy(c->height, sec->heightmap, 256);
		if(changed) hash_column(c);
		/* Order-independent world checksum, keyed so swapping equal-sized
 * columns cannot disappear. Plane hashes themselves are frozen too. */
		for(unsigned p = 0; p < PLANES; p++) {
			uint32_t h = c->hashes[p];
			h = keyed_hash(h, id);
			obs.client_hash[p] ^= h;
		}
	}
	for(size_t i = 0; i < obs.count;) {
		if(!obs.columns[i]->present) {
			free(obs.columns[i]);
			obs.columns[i] = obs.columns[--obs.count];
		} else i++;
	}
	obs.client_full = (uint32_t)obs.count;
	for(size_t i = 0; !obs.traces && i < obs.shells_count; i++) {
		struct chunk* c = obs.shells[i].chunk;
		if(c->world != w || c->ownership == FAR_SKIN) continue;
		int64_t id = S_CHUNK_ID(WCOORD_CHUNK_OFFSET(c->x),
								WCOORD_CHUNK_OFFSET(c->z));
		struct world_section* sec = dict_wsection_get(w->sections, id);
		obs.client_slices++;
		if(!sec || sec->column[c->y / 16] != c) obs.orphan_slices++;
	}
	if(obs.traces & 1) {
		trace_u32(obs.client, (uint32_t)event);
		trace_u32(obs.client, obs.client_seq);
		trace_u32(obs.client, obs.reset);
		trace_u32(obs.client, obs.client_full);
		for(unsigned p = 0; p < PLANES; p++) trace_u32(obs.client, obs.client_hash[p]);
		obs.state_records++;
	}
	thread_mutex_unlock(&obs.lock);
}

void column_observer_client(struct world* w, int event) {
	client_snapshot(w, event, false, 0);
}

static void rpc(const struct client_rpc* call) {
	/* The full builder can overwrite existing shells. Attach its publication
	 * identity after install too; old worker/GPU-only shells keep the old ID. */
	if(call->type == CRPC_CHUNK && obs.install_generation) {
		thread_mutex_lock(&obs.lock);
		int64_t id = S_CHUNK_ID(WCOORD_CHUNK_OFFSET(call->payload.chunk.x),
			WCOORD_CHUNK_OFFSET(call->payload.chunk.z));
		struct world_section* sec = dict_wsection_get(gstate.world.sections, id);
		if(sec && !sec->is_far_lod) for(size_t i = 0; i < obs.shells_count; i++) {
			struct observed_shell* shell = &obs.shells[i];
			for(unsigned cy = 0; cy < 8; cy++) if(sec->column[cy] == shell->chunk
				&& shell->generation != obs.install_generation) {
				if(shell->generation) obs.generations[shell->generation - 1].client_slices--;
				else obs.live_full_slices++;
				shell->generation = obs.install_generation;
				obs.generations[shell->generation - 1].client_slices++;
			}
		}
		retained_generations();
		allocation_peaks();
		thread_mutex_unlock(&obs.lock);
	}
	obs.install_generation = 0;
	if(!(obs.traces & 1)) {
		if(call->type == CRPC_WORLD_RESET) {
			thread_mutex_lock(&obs.lock);
			obs.reset++;
			thread_mutex_unlock(&obs.lock);
		}
		return;
	}
	switch(call->type) {
		case CRPC_CHUNK: case CRPC_UNLOAD_CHUNK: case CRPC_WORLD_RESET:
		case CRPC_STREAM_RESET: case CRPC_FAR_CHUNK: case CRPC_PLAYER_POS:
			column_observer_client(&gstate.world, call->type);
			return;
		case CRPC_SET_BLOCK:
			client_snapshot(&gstate.world, call->type, true,
				S_CHUNK_ID(WCOORD_CHUNK_OFFSET(call->payload.set_block.x),
					WCOORD_CHUNK_OFFSET(call->payload.set_block.z)));
			return;
		case CRPC_PISTON_MOVING:
			column_observer_client(&gstate.world, call->type);
			return;
		default: break;
	}
	/* The remaining handlers cannot write voxel/height arrays. Preserve their
	 * individual RPC boundaries with the unchanged full-world plane hashes. */
	thread_mutex_lock(&obs.lock);
	obs.client_seq++;
	trace_u32(obs.client, call->type);
	trace_u32(obs.client, obs.client_seq);
	trace_u32(obs.client, obs.reset);
	trace_u32(obs.client, obs.client_full);
	for(unsigned p = 0; p < PLANES; p++) trace_u32(obs.client, obs.client_hash[p]);
	obs.state_records++;
	thread_mutex_unlock(&obs.lock);
}

static void slot(int delta, const void* token) {
	thread_mutex_lock(&obs.lock);
	if(delta > 0) obs.slots++;
	else if(obs.slots) obs.slots--;
	else fail("slot accounting underflow");
	if(delta < 0) for(size_t i = 0; i < obs.tags_count; i++) {
		struct observed_slot* tag = &obs.tags[i];
		if(tag->slot == token && tag->generation) {
			obs.generations[tag->generation - 1].transports--;
			tag->generation = 0;
		}
	}
	if(obs.slots > obs.slots_peak) obs.slots_peak = obs.slots;
	allocation_peaks();
	thread_mutex_unlock(&obs.lock);
}

static void server_chunk_event(struct server_world* w, int32_t x, int32_t z,
	const struct server_chunk* sc, bool loaded) {
	(void)w;
	thread_mutex_lock(&obs.lock);
	if(loaded) {
		bool found = false;
		for(size_t i = 0; i < obs.generation_count; i++)
			if(obs.generations[i].server_live && obs.generations[i].ids == sc->ids)
				found = true;
		if(!found && grow((void**)&obs.generations, &obs.generation_capacity,
			sizeof(*obs.generations), obs.generation_count + 1)) {
			obs.generations[obs.generation_count++] = (struct observed_generation){
				.key = S_CHUNK_ID(x, z), .ids = sc->ids, .server_live = true};
			obs.live_server_columns++;
		}
	} else {
		for(size_t i = 0; i < obs.generation_count; i++)
			if(obs.generations[i].server_live && obs.generations[i].ids == sc->ids) {
				obs.generations[i].server_live = false;
				obs.live_server_columns--;
			}
	}
	retained_generations();
	allocation_peaks();
	thread_mutex_unlock(&obs.lock);
}

static void publish(const struct client_rpc* call) {
	if(call->type != CRPC_CHUNK) return;
	thread_mutex_lock(&obs.lock);
	int64_t id = S_CHUNK_ID(WCOORD_CHUNK_OFFSET(call->payload.chunk.x),
		WCOORD_CHUNK_OFFSET(call->payload.chunk.z));
	uint32_t generation = 0;
	for(size_t i = 0; i < obs.generation_count; i++)
		if(obs.generations[i].key == id && obs.generations[i].server_live)
			generation = (uint32_t)i + 1;
	if(!generation) fail("local publication without backing identity");
	struct observed_slot* tag = NULL;
	for(size_t i = 0; i < obs.tags_count; i++)
		if(obs.tags[i].slot == call->payload.chunk.slot) tag = &obs.tags[i];
	if(!tag && grow((void**)&obs.tags, &obs.tags_capacity, sizeof(*obs.tags),
		obs.tags_count + 1)) {
		tag = &obs.tags[obs.tags_count++];
		*tag = (struct observed_slot){.slot = call->payload.chunk.slot};
	}
	if(tag) {
		if(tag->generation) fail("publication reused a live transport tag");
		tag->generation = generation;
		if(generation) obs.generations[generation - 1].transports++;
	}
	thread_mutex_unlock(&obs.lock);
}

static void before_rpc(const struct client_rpc* call) {
	obs.install_generation = 0;
	if(call->type != CRPC_CHUNK) return;
	thread_mutex_lock(&obs.lock);
	for(size_t i = 0; i < obs.tags_count; i++)
		if(obs.tags[i].slot == call->payload.chunk.slot)
			obs.install_generation = obs.tags[i].generation;
	thread_mutex_unlock(&obs.lock);
}

static unsigned pages(const uint8_t* a, const uint8_t* b, unsigned stride) {
	unsigned different = 0;
	for(unsigned page = 0; page < 128; page++)
		different += memcmp(a + page * stride, b + page * stride, stride) != 0;
	return different;
}

static void server_tick(struct server_world* w) {
	if(w != obs.live_server) return;
	thread_mutex_lock(&obs.lock);
	obs.tick = w->current_tick;
	obs.server_seq++;
	uint32_t server_hash[PLANES] = {0};
	unsigned retained = retained_generations();
	/* Census header per simulation tick. All following C rows belong to it.
 * client_seq records precisely which completed client boundary was compared. */
	if(obs.census) gzprintf(obs.census,
		"T,%u,%u,%u,%u,%zu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
		obs.server_seq, obs.tick, obs.client_seq, obs.measuring,
		dict_server_chunks_size(w->chunks), obs.client_full, obs.client_slices,
		obs.slots, obs.slots_peak, retained, obs.orphan_slices, obs.reset,
		obs.retained_peak, obs.peak_occupied_slices, obs.peak_allocated_slices);
	if(obs.census) {
		struct column_view_stats views;
		column_view_get_stats(&views);
		gzprintf(obs.census, "V,%u,%u,%u,%u,%u,%u\n", obs.server_seq,
			views.live_views, views.pages, views.peak_pages,
			views.fallback_live, views.fallback_total);
	}
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it)) {
		const struct server_chunk* sc = &dict_server_chunks_ref(it)->value;
		int64_t id = dict_server_chunks_ref(it)->key;
		const uint8_t* planes[] = {sc->ids, sc->metadata, sc->lighting_sky,
			sc->lighting_torch, sc->heightmap};
		const unsigned sizes[] = {CELLS, HALF, HALF, HALF, 256};
		if(obs.traces & 1) for(unsigned p = 0; p < PLANES; p++) {
			uint32_t h = (uint32_t)crc32(0, planes[p], sizes[p]);
			server_hash[p] ^= keyed_hash(h, id);
		}
		struct observed_column* c = lookup(id);
		if(!c || w->dimension != obs.dimension) continue;
		unsigned d[] = {pages(sc->ids, c->ids, 256),
			pages(sc->metadata, c->meta, 128),
			pages(sc->lighting_sky, c->sky, 128),
			pages(sc->lighting_torch, c->torch, 128),
			memcmp(sc->heightmap, c->height, 256) != 0};
		if(obs.census && gzprintf(obs.census, "C,%d,%d,%u,%u,%u,%u,%u\n",
			S_CHUNK_X(id), S_CHUNK_Z(id), d[0], d[1], d[2], d[3], d[4]) <= 0)
			fail("census write");
		obs.census_rows++;
	}
	if(obs.traces & 1) {
		trace_u32(obs.server, obs.tick);
		trace_u32(obs.server, (uint32_t)dict_server_chunks_size(w->chunks));
		for(unsigned p = 0; p < PLANES; p++) trace_u32(obs.server, server_hash[p]);
		obs.state_records++;
	}
	thread_mutex_unlock(&obs.lock);
}

static void server_lifetime(struct server_world* w, bool created) {
	obs.live_server = created ? w : NULL;
}

static void mesh(const struct chunk* c, uint32_t generation,
				 const struct displaylist* lists, const bool* has,
				 const uint8_t* reachable, bool retry) {
	if(!(obs.traces & 2)) return;
	thread_mutex_lock(&obs.lock);
	key(obs.render, S_CHUNK_ID(c->x, c->z));
	trace_u32(obs.render, (uint32_t)c->y);
	trace_u32(obs.render, generation);
	trace_u32(obs.render, retry);
	stream(obs.render, reachable, 6);
	for(unsigned k = 0; k < CHUNK_MESH_LISTS; k++) {
		trace_u32(obs.render, has[k]);
		if(!has[k]) continue;
		const struct displaylist* l = &lists[k];
#ifdef PLATFORM_PC
		size_t bytes = l->index * (l->tinted ? 25u : 22u);
#else
		/* GX's finalized command stream includes its actual zero padding. */
		size_t bytes = (l->index + 31u) / 32u * 32u;
#endif
		trace_u32(obs.render, l->tinted);
		trace_u32(obs.render, (uint32_t)l->index);
		trace_u32(obs.render, (uint32_t)bytes);
		stream(obs.render, l->data, bytes);
		obs.render_bytes += bytes;
	}
	thread_mutex_unlock(&obs.lock);
}

void column_observer_window(bool measuring) {
	if(!obs.active) return;
	thread_mutex_lock(&obs.lock);
	obs.measuring = measuring;
	thread_mutex_unlock(&obs.lock);
}

static gzFile open_stream(const char* prefix, const char* suffix, bool compare) {
	char name[1024];
	if(snprintf(name, sizeof(name), "%s%s.gz", prefix, suffix) >= (int)sizeof(name))
		return NULL;
	return gzopen(name, compare ? "rb" : "wb1");
}

bool column_observer_start(const char* prefix, unsigned traces, bool compare) {
	if(obs.active) return false;
	memset(&obs, 0, sizeof(obs));
	if(!thread_mutex_init(&obs.lock)) return false;
	obs.traces = traces;
	obs.compare = compare;
	if(traces) {
		if(traces & 1) {
			obs.server = open_stream(prefix, "server", compare);
			obs.client = open_stream(prefix, "client", compare);
			if(!obs.server || !obs.client) fail("open state streams");
		}
		if(traces & 2) {
			obs.render = open_stream(prefix, "render", compare);
			if(!obs.render) fail("open render stream");
		}
	} else {
		obs.census = open_stream(prefix, "census", false);
		if(!obs.census) fail("open census");
	}
	obs.active = true;
	clin_column_observe_rpc = rpc;
	clin_column_observe_before_rpc = before_rpc;
	clin_column_observe_publish = publish;
	clin_column_observe_slot = slot;
	server_column_observe_tick = traces ? server_tick : NULL;
	server_column_observe_update = traces ? NULL : server_tick;
	server_column_observe_lifetime = server_lifetime;
	server_column_observe_chunk = server_chunk_event;
	chunk_column_observe_lifetime = lifetime;
	mesher_column_observe_complete = mesh;
	return !obs.failed;
}

bool column_observer_stop(void) {
	if(!obs.active) return true;
	clin_column_observe_rpc = NULL;
	clin_column_observe_before_rpc = NULL;
	clin_column_observe_publish = NULL;
	clin_column_observe_slot = NULL;
	server_column_observe_tick = NULL;
	server_column_observe_update = NULL;
	server_column_observe_lifetime = NULL;
	server_column_observe_chunk = NULL;
	chunk_column_observe_lifetime = NULL;
	mesher_column_observe_complete = NULL;
	gzFile streams[] = {obs.census, obs.server, obs.client, obs.render};
	for(unsigned i = 0; i < 4; i++) if(streams[i]) {
		if(obs.compare && i && gzgetc(streams[i]) != -1) fail("trailing golden records");
		if(gzclose(streams[i]) != Z_OK) fail("close evidence stream");
	}
	printf("column-observer ticks=%u client_boundaries=%u rows=%llu "
		   "mesh_bytes=%llu status=%s\n", obs.server_seq, obs.client_seq,
		   (unsigned long long)obs.census_rows,
		   (unsigned long long)obs.render_bytes, obs.failed ? "FAIL" : "PASS");
	for(size_t i = 0; i < obs.count; i++) free(obs.columns[i]);
	free(obs.columns);
	free(obs.shells);
	free(obs.generations);
	free(obs.tags);
	thread_mutex_destroy(&obs.lock);
	obs.active = false;
	return !obs.failed;
}

bool column_observer_ok(void) {
	if(!obs.active) return false;
	thread_mutex_lock(&obs.lock);
	bool ok = !obs.failed;
	thread_mutex_unlock(&obs.lock);
	return ok;
}

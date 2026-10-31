/* Opt-in first-divergence cell witness, independent of the golden observer.
 * CW_COLUMN_RPC_TRACE writes every consumed RPC through world tick25, with
 * every SET_BLOCK's coordinates and payload. */
#ifndef TESTS_SUPPORT_COLUMN_RPC_TRACE_H
#define TESTS_SUPPORT_COLUMN_RPC_TRACE_H
#include "network/client_interface.h"
#include "network/server_local_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "column_observer.h"
#include "chunk.h"
#include "game/game_state.h"
#include <zlib.h>
static FILE* column_rpc_trace_file;
static void column_rpc_trace_close(void) {
	if(column_rpc_trace_file) {
		assert(fclose(column_rpc_trace_file) == 0);
		column_rpc_trace_file = NULL;
	}
}
static void column_rpc_trace_consume(const struct client_rpc* call, void* user) {
	struct server_local* s = user;
	/* A new run's direct RESET can arrive before its seed callback re-arms.
	 * It is not a queued consume event; never retain a released session. */
	if(call->type == CRPC_WORLD_RESET) return;
	if(s->worlds[0].current_tick > 25) return;
	fprintf(column_rpc_trace_file, "%u %u", s->worlds[0].current_tick, call->type);
	if(call->type == CRPC_SET_BLOCK) fprintf(column_rpc_trace_file, " %d %d %d %u %u %u %u",
		call->payload.set_block.x,
		call->payload.set_block.y, call->payload.set_block.z,
		call->payload.set_block.block.type, call->payload.set_block.block.metadata,
		call->payload.set_block.block.sky_light, call->payload.set_block.block.torch_light);
	fputc('\n', column_rpc_trace_file);
	assert(fflush(column_rpc_trace_file) == 0);
}
static void column_rpc_trace_arm(struct server_local* s) {
	const char* prefix = getenv("CW_COLUMN_RPC_TRACE");
	if(!prefix || !*prefix) return;
	column_rpc_trace_close();
	char path[1024];
	int n = snprintf(path, sizeof path, "%s.%lld.log", prefix, (long long)s->worlds[0].seed);
	assert(n > 0 && (size_t)n < sizeof path);
	column_rpc_trace_file = fopen(path, "w"); assert(column_rpc_trace_file);
	clin_test_set_consume_observer(column_rpc_trace_consume, s);
}

/* CW_COLUMN_LINEAGE is a separate, opt-in paired-cell witness. All RPC and
 * checkpoint ordinals are retained. Plane images are explicitly bounded to
 * world ticks <=25, around the unresolved historical cells; this is not a
 * complete reconstruction. Hooks only read public logical block values. */
static gzFile column_lineage_planes;
static FILE* column_lineage_events;
static struct server_local* column_lineage_session;
static uint32_t column_lineage_sequence;
static void (*column_lineage_before_saved)(const struct client_rpc*);
static void (*column_lineage_after_saved)(const struct client_rpc*);

static void column_lineage_plane(const struct client_rpc* call, unsigned phase) {
	if(!column_lineage_session || call->type != CRPC_SET_BLOCK
	   || column_lineage_session->worlds[0].current_tick > 25) return;
	int x = call->payload.set_block.x, z = call->payload.set_block.z;
	int cx = WCOORD_CHUNK_OFFSET(x), cz = WCOORD_CHUNK_OFFSET(z);
	struct world_section* sec = dict_wsection_get(gstate.world.sections, S_CHUNK_ID(cx, cz));
	bool full = sec && !sec->is_far_lod;
	if(full) for(unsigned cy = 0; cy < 8; cy++) if(!sec->column[cy]) full = false;
	uint32_t header[] = {phase, column_lineage_sequence + (phase == 0),
		column_lineage_session->worlds[0].current_tick, (uint32_t)cx, (uint32_t)cz,
		(uint32_t)x, (uint32_t)call->payload.set_block.y, (uint32_t)z,
		call->payload.set_block.block.type, call->payload.set_block.block.metadata,
		call->payload.set_block.block.sky_light, call->payload.set_block.block.torch_light, full};
	assert(gzwrite(column_lineage_planes, header, sizeof header) == sizeof header);
	if(!full) return;
	uint8_t ids[32768], meta[16384], sky[16384], torch[16384];
	for(unsigned bx = 0; bx < 16; bx++) for(unsigned bz = 0; bz < 16; bz++)
		for(unsigned y = 0; y < 128; y += 2) {
			size_t i = (bx * 16 + bz) * 128 + y;
			struct block_data a = chunk_get_block(sec->column[y / 16], bx, y % 16, bz);
			struct block_data b = chunk_get_block(sec->column[y / 16], bx, y % 16 + 1, bz);
			ids[i] = a.type; ids[i + 1] = b.type;
			meta[i / 2] = a.metadata | (b.metadata << 4);
			sky[i / 2] = a.sky_light | (b.sky_light << 4);
			torch[i / 2] = a.torch_light | (b.torch_light << 4);
		}
	assert(gzwrite(column_lineage_planes, ids, sizeof ids) == sizeof ids);
	assert(gzwrite(column_lineage_planes, meta, sizeof meta) == sizeof meta);
	assert(gzwrite(column_lineage_planes, sky, sizeof sky) == sizeof sky);
	assert(gzwrite(column_lineage_planes, torch, sizeof torch) == sizeof torch);
	assert(gzwrite(column_lineage_planes, sec->heightmap, 256) == 256);
}
static void column_lineage_before(const struct client_rpc* call) {
	column_lineage_plane(call, 0);
	if(column_lineage_before_saved) column_lineage_before_saved(call);
}
static void column_lineage_after(const struct client_rpc* call) {
	if(column_lineage_after_saved) column_lineage_after_saved(call);
	column_lineage_sequence++;
	unsigned tick = column_lineage_session ? column_lineage_session->worlds[0].current_tick : 0;
	assert(fprintf(column_lineage_events, "%u %u %u\n", column_lineage_sequence, tick, call->type) > 0);
	column_lineage_plane(call, 1);
}
static void column_lineage_begin(int64_t seed) {
	const char* prefix = getenv("CW_COLUMN_LINEAGE");
	if(!prefix || !*prefix) return;
	char path[1024];
	int n = snprintf(path, sizeof path, "%s.%lld.planes.gz", prefix, (long long)seed);
	assert(n > 0 && (size_t)n < sizeof path);
	column_lineage_planes = gzopen(path, "wb1"); assert(column_lineage_planes);
	n = snprintf(path, sizeof path, "%s.%lld.events", prefix, (long long)seed);
	assert(n > 0 && (size_t)n < sizeof path);
	column_lineage_events = fopen(path, "wb"); assert(column_lineage_events);
	column_lineage_sequence = 0; column_lineage_session = NULL;
	column_lineage_before_saved = clin_column_observe_before_rpc;
	column_lineage_after_saved = clin_column_observe_rpc;
	clin_column_observe_before_rpc = column_lineage_before;
	clin_column_observe_rpc = column_lineage_after;
}
static void column_lineage_bind(struct server_local* s) {
	if(column_lineage_events) column_lineage_session = s;
}
static void column_lineage_checkpoint(void) {
	if(!column_lineage_events) return;
	column_lineage_sequence++;
	assert(fprintf(column_lineage_events, "%u %u 4294967295\n", column_lineage_sequence,
		column_lineage_session->worlds[0].current_tick) > 0);
}
static void column_lineage_end(void) {
	if(!column_lineage_events) return;
	clin_column_observe_before_rpc = column_lineage_before_saved;
	clin_column_observe_rpc = column_lineage_after_saved;
	assert(fclose(column_lineage_events) == 0); column_lineage_events = NULL;
	assert(gzclose(column_lineage_planes) == Z_OK); column_lineage_planes = NULL;
	column_lineage_session = NULL;
}
#endif

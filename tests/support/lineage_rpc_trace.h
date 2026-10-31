/* Opt-in, write-only envelope identities for complete-lineage replays.
 * CW_LINEAGE_RPC=<path>. The normal assertions/folded bytes are untouched.
 * Numeric ordinals 0/1/12/20/37 are stable across the archived fixtures.
 * Exact-executable PCs identify the publisher's world/network/tracker phase. */
#ifndef TESTS_SUPPORT_LINEAGE_RPC_TRACE_H
#define TESTS_SUPPORT_LINEAGE_RPC_TRACE_H
#include "network/client_interface.h"
#include "network/server_local_state.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
static FILE* lineage_rpc_output;
static void lineage_rpc_close(void) {
	if(lineage_rpc_output) {
		assert(fclose(lineage_rpc_output) == 0);
		lineage_rpc_output = NULL;
	}
}
static void lineage_rpc_emit(const struct client_rpc* rpc, void* user) {
	const char* path = getenv("CW_LINEAGE_RPC");
	if(!path || !*path) return;
	if(!lineage_rpc_output) {
		lineage_rpc_output = fopen(path, "wb"); assert(lineage_rpc_output);
		assert(atexit(lineage_rpc_close) == 0);
	}
	struct server_local* s = user;
	assert(s);
	uint32_t id = 0, bits[3] = {0};
	int x = 0, y = 0, z = 0;
	unsigned block = 0, metadata = 0, sky = 0, torch = 0;
	if((unsigned)rpc->type == 20) {
		id = rpc->payload.entity_move.entity_id;
		memcpy(bits, rpc->payload.entity_move.pos, sizeof bits);
	} else if((unsigned)rpc->type == 12) {
		x = rpc->payload.set_block.x; y = rpc->payload.set_block.y;
		z = rpc->payload.set_block.z;
		block = rpc->payload.set_block.block.type;
		metadata = rpc->payload.set_block.block.metadata;
		sky = rpc->payload.set_block.block.sky_light;
		torch = rpc->payload.set_block.block.torch_light;
	} else if((unsigned)rpc->type == 37) {
		id = rpc->payload.sound.sound_id;
		memcpy(bits, rpc->payload.sound.pos, sizeof bits);
	}
	assert(fprintf(lineage_rpc_output,
		"%u %u %u %08x %08x %08x %d %d %d %u %u %u %u",
		s->worlds[0].current_tick, (unsigned)rpc->type, id,
		bits[0], bits[1], bits[2], x, y, z, block, metadata, sky, torch) > 0);
#ifdef _WIN32
	void* stack[12] = {0};
	USHORT count = CaptureStackBackTrace(1, 12, stack, NULL);
	uintptr_t base = (uintptr_t)GetModuleHandle(NULL);
	for(unsigned i = 0; i < count; i++)
		assert(fprintf(lineage_rpc_output, " %" PRIxPTR,
			(uintptr_t)stack[i] - base) > 0);
#endif
	assert(fputc('\n', lineage_rpc_output) != EOF);
	assert(fflush(lineage_rpc_output) == 0);
}
#endif

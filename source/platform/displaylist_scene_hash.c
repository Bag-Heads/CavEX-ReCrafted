/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* colcap-1006 (ASTRA-CC-R5 item 2b): ordered GX scene hash accumulator
 * (displaylist_scene_hash.h). Main thread only: armed and read between
 * frames by pool_selftest.c, fed by the render path of the same thread. */
#include "displaylist_scene_hash.h"

volatile bool g_dl_scene_hash_armed = false;
static struct dl_scene_frame s_dl_scene;
static uint64_t s_dl_scene_chunk;	  /* current (chunk, pass) list hash */
static uint64_t s_dl_scene_chunk_key; /* its position/pass key */
static bool s_dl_scene_in_chunk;
static dl_scene_content_fn s_dl_scene_content_fn;

void dl_scene_hash_set_content(dl_scene_content_fn fn) {
	s_dl_scene_content_fn = fn;
}

void dl_scene_hash_frame_begin(void) {
	s_dl_scene = (struct dl_scene_frame) {
		.gx_ordered = DL_SCENE_FNV64_INIT,
		.pos_ordered = DL_SCENE_FNV64_INIT,
		.content_ordered = DL_SCENE_FNV64_INIT,
	};
	s_dl_scene_in_chunk = false;
	g_dl_scene_hash_armed = true;
}

void dl_scene_hash_frame_end(struct dl_scene_frame* out) {
	g_dl_scene_hash_armed = false;
	if(s_dl_scene_in_chunk)
		dl_scene_hash_chunk_end();
	if(out)
		*out = s_dl_scene;
}

void dl_scene_hash_list(const void* data, size_t bytes) {
	/* Chunk lists only: entity/item lists also go through
	 * displaylist_render, but mob poses are not part of the scene claim. */
	if(!g_dl_scene_hash_armed || !s_dl_scene_in_chunk)
		return;
	s_dl_scene.gx_ordered
		= dl_scene_fnv64_u32(s_dl_scene.gx_ordered, (uint32_t)bytes);
	s_dl_scene.gx_ordered = dl_scene_fnv64(s_dl_scene.gx_ordered, data, bytes);
	s_dl_scene.lists++;
	s_dl_scene.bytes += bytes;
	s_dl_scene_chunk = dl_scene_fnv64_u32(s_dl_scene_chunk, (uint32_t)bytes);
	s_dl_scene_chunk = dl_scene_fnv64(s_dl_scene_chunk, data, bytes);
}

static uint64_t dl_scene_key(int32_t x, int32_t y, int32_t z, uint32_t tag) {
	uint64_t h = DL_SCENE_FNV64_INIT;
	h = dl_scene_fnv64_u32(h, (uint32_t)x);
	h = dl_scene_fnv64_u32(h, (uint32_t)y);
	h = dl_scene_fnv64_u32(h, (uint32_t)z);
	return dl_scene_fnv64_u32(h, tag);
}

void dl_scene_hash_chunk_begin(int32_t x, int32_t y, int32_t z, bool pass,
							   const void* chunk) {
	if(!g_dl_scene_hash_armed)
		return;
	if(!pass && chunk && s_dl_scene_content_fn)
		dl_scene_hash_content(x, y, z, s_dl_scene_content_fn(chunk));
	if(s_dl_scene_in_chunk)
		dl_scene_hash_chunk_end();
	s_dl_scene_chunk_key = dl_scene_key(x, y, z, pass ? 1u : 0u);
	s_dl_scene_chunk = s_dl_scene_chunk_key;
	s_dl_scene_in_chunk = true;
	s_dl_scene.pos_ordered = dl_scene_fnv64(s_dl_scene.pos_ordered,
		&s_dl_scene_chunk_key, sizeof s_dl_scene_chunk_key);
	s_dl_scene.chunks++;
}

void dl_scene_hash_chunk_end(void) {
	if(!s_dl_scene_in_chunk)
		return;
	s_dl_scene_in_chunk = false;
	s_dl_scene.gx_set += dl_scene_mix64(s_dl_scene_chunk);
}

void dl_scene_hash_content(int32_t x, int32_t y, int32_t z, uint64_t content) {
	if(!g_dl_scene_hash_armed)
		return;
	uint64_t keyed = dl_scene_key(x, y, z, 2u) ^ content;
	s_dl_scene.content_ordered = dl_scene_fnv64(s_dl_scene.content_ordered,
		&keyed, sizeof keyed);
	s_dl_scene.content_set += dl_scene_mix64(keyed);
	s_dl_scene.content_chunks++;
}

/* R17: frozen with production sources at 2f18d992b, before R16.
 * Replay the remote adapter's real client entry points, without creating a
 * server or installing a provider. This is not a socket/codec/network test. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "column_view.h"
#include "config.h"
#include "game/game_state.h"
#include "item/items.h"
#include "lighting.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "particle.h"
#include "settle_observer.h"
#include "world.h"

static gzFile tape;
static bool record, untaped; /* untaped: REL-MP-FIX3b case, never on the tape */
static size_t bytes, writes, dirty_events;

/* REL-MP-R17-SHRINK: each maximal run of light-write (1,x,y,z,light) and
 * dirty (2,stage,x,y,z,gen) records is no longer taped word by word. The run's
 * exact former tape bytes (little-endian words, same order) are SHA-256
 * hashed, and the run is taped as 11,writes,dirties,digest[8] right before the
 * next taped word. Every other record is byte-for-byte the pre-shrink tape. */
static uint32_t sha[8], blk[16], run_writes, run_dirty;
static unsigned blk_n;
static uint64_t run_bytes;
static uint32_t ror(uint32_t v, int n) { return v >> n | v << (32 - n); }
static uint32_t bswap(uint32_t v) {
	return v >> 24 | (v >> 8 & 0xFF00u) | (v << 8 & 0xFF0000u) | v << 24;
}
static void sha_block(void) {
	static const uint32_t K[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
		0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
		0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
		0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
		0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
		0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
		0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
		0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
		0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
	uint32_t w[64], v[8];
	memcpy(w, blk, sizeof(blk));
	for(int i = 16; i < 64; i++)
		w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3)
			+ w[i - 7] + (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10);
	memcpy(v, sha, sizeof(v));
	for(int i = 0; i < 64; i++) {
		uint32_t t1 = v[7] + (ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25))
			+ ((v[4] & v[5]) ^ (~v[4] & v[6])) + K[i] + w[i];
		uint32_t t2 = (ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22))
			+ ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
		memmove(v + 1, v, 7 * sizeof(*v));
		v[4] += t1, v[0] = t1 + t2;
	}
	for(int i = 0; i < 8; i++)
		sha[i] += v[i];
	blk_n = 0;
}
static void sha_word(uint32_t be) { /* one big-endian message word */
	blk[blk_n++] = be;
	if(blk_n == 16)
		sha_block();
}
static void dig(uint32_t value) { /* a former tape word, LE bytes */
	if(untaped)
		return;
	if(!run_bytes) {
		static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
			0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
		memcpy(sha, iv, sizeof(sha)), blk_n = 0;
	}
	sha_word(bswap(value));
	run_bytes += 4;
}

static bool in_run; /* mismatch report: inside a run's digest record */
/* Explicit little-endian words: no padding, pointer or host-clock bytes. */
static void put(uint32_t value) {
	for(unsigned k = 0; k < 4; k++) {
		unsigned byte = (value >> (8 * k)) & 255u;
		if(record) {
			assert(gzputc(tape, byte) != EOF);
		} else {
			int expected = gzgetc(tape);
			if(expected != (int)byte) {
				fprintf(stderr, "remote golden mismatch byte %zu: %d != %u%s\n",
						bytes, expected, byte, in_run ? " (in a light-write/dirty"
						" run digest: count or SHA-256 differs)" : "");
				exit(1);
			}
		}
		bytes++;
	}
}

static void flush_run(void) {
	if(untaped || !run_bytes)
		return;
	const uint64_t bits = run_bytes * 8;
	sha_word(0x80000000u);
	while(blk_n != 14)
		sha_word(0);
	sha_word((uint32_t)(bits >> 32)), sha_word((uint32_t)bits);
	in_run = true;
	put(11); put(run_writes); put(run_dirty);
	for(int i = 0; i < 8; i++)
		put(bswap(sha[i])); /* tape bytes == the SHA-256 digest bytes */
	in_run = false;
	run_bytes = run_writes = run_dirty = 0;
}

static void emit(uint32_t value) {
	if(untaped)
		return;
	flush_run();
	put(value);
}

static void owned(const struct chunk* c) {
	assert(c && c->ownership == OWNED_VOXEL);
	assert(c->backing == NULL && c->view == NULL);
}

void __real_chunk_set_light(struct chunk*, c_coord_t, c_coord_t, c_coord_t,
						   uint8_t);
void __wrap_chunk_set_light(struct chunk* c, c_coord_t x, c_coord_t y,
						   c_coord_t z, uint8_t light) {
	owned(c);
	dig(1);
	dig(c->x + x); dig(c->y + y); dig(c->z + z); dig(light);
	run_writes += !untaped, writes++;
	__real_chunk_set_light(c, x, y, z, light);
}

static void dirty(const struct settle_input* in) {
	if(!in->chunk)
		return;
	owned(in->chunk);
	dig(2); dig(in->stage);
	dig(in->chunk->x); dig(in->chunk->y); dig(in->chunk->z);
	dig(in->chunk->dirty_generation);
	run_dirty += !untaped, dirty_events++;
}

static void mesh_state(void) {
	for(int z = -16; z <= 0; z += 16)
		for(int x = -16; x <= 0; x += 16)
			for(int y = 0; y < WORLD_HEIGHT; y += 16) {
				struct chunk* c = world_find_chunk(&gstate.world, x, y, z);
				emit(c != NULL);
				if(!c)
					continue;
				owned(c);
				emit(c->dirty_generation);
				emit(c->rebuild_displist); emit(c->mesh_in_flight);
				emit(c->mesh_urgent);
				for(unsigned k = 0; k < CHUNK_MESH_LISTS; k++)
					emit(c->has_displist[k]);
			}
}

static void canonical(void) {
	emit(3);
	mesh_state();
	for(int z = -16; z < 16; z++)
		for(int x = -16; x < 16; x++)
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				struct block_data b = world_get_block(&gstate.world, x, y, z);
				emit((uint32_t)b.type | ((uint32_t)b.metadata << 8)
					 | ((uint32_t)b.sky_light << 16)
					 | ((uint32_t)b.torch_light << 24));
			}
}

static bool load_failed_ok; /* FIX3d2: the reservation-failure case only */
static void dispatch(struct client_rpc* call) {
	emit(4); emit(call->type);
	clin_process_direct_remote(call);
	assert(call->world_generation == clin_remote_world_generation());
	assert(load_failed_ok || !gstate.world_load_failed);
}

/* Remote Packet51 full-column or rectangular section layout [x][z][y].
 * Adjacent low/high nibbles and sky/torch planes deliberately differ. */
static void volume(int x, int y, int z, int sx, int sy, int sz, unsigned salt) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	size_t count = (size_t)sx * sy * sz;
	for(size_t i = 0; i < count; i++) {
		unsigned ly = i % sy;
		slot->ids[i] = ly == 63 ? BLOCK_STONE : BLOCK_AIR;
		nibble_write(slot->metadata, i, (i * 3 + salt) & 15u);
		nibble_write(slot->lighting_sky, i, (i * 5 + salt + 1) & 15u);
		nibble_write(slot->lighting_torch, i, (i * 7 + salt + 2) & 15u);
	}
	memset(slot->heightmap, 64, sizeof(slot->heightmap));
	assert(slot->backing == NULL);
	emit(5); emit(x); emit(y); emit(z); emit(sx); emit(sy); emit(sz); emit(salt);
	uint32_t install = clin_chunk_install_seq();
	struct client_rpc call = {.type = CRPC_CHUNK,
		.payload.chunk = {.x = x, .y = y, .z = z, .sx = sx, .sy = sy,
			.sz = sz, .slot = slot}};
	dispatch(&call);
	bool full = sx == 16 && sy == WORLD_HEIGHT && sz == 16;
	assert(clin_chunk_install_seq() - install == (unsigned)full);
	emit(clin_chunk_install_seq() - install);
	/* Pin actual packed ingress before any relight can normalize its values. */
	for(int lx = 0; lx < sx; lx++)
		for(int lz = 0; lz < sz; lz++)
			for(int ly = 0; ly < sy; ly++) {
				size_t i = ((size_t)lx * sz + lz) * sy + ly;
				owned(world_find_chunk(&gstate.world, x + lx, y + ly, z + lz));
				struct block_data b = world_get_block(&gstate.world,
					x + lx, y + ly, z + lz);
				assert(b.type == (ly == 63 ? BLOCK_STONE : BLOCK_AIR));
				assert(b.metadata == ((i * 3 + salt) & 15u));
				assert(b.sky_light == ((i * 5 + salt + 1) & 15u));
				assert(b.torch_light == ((i * 7 + salt + 2) & 15u));
			}
}

static void install(unsigned salt) {
	for(int z = -16; z <= 0; z += 16)
		for(int x = -16; x <= 0; x += 16)
			volume(x, 0, z, 16, WORLD_HEIGHT, 16, salt++);
	canonical();
}

static void edit(int x, int y, int z, uint8_t type, uint8_t meta) {
	emit(6); emit(x); emit(y); emit(z); emit(type); emit(meta);
	struct client_rpc call = {.type = CRPC_SET_BLOCK,
		.payload.set_block = {.x = x, .y = y, .z = z,
			.block = {.type = type, .metadata = meta}}};
	dispatch(&call);
	struct block_data b = world_get_block(&gstate.world, x, y, z);
	assert(b.type == type && b.metadata == meta);
}

static bool pending(void) {
	return !stack_empty(&gstate.world.lighting_updates)
		|| !lighting_flood_queue_empty(&gstate.world.lighting_flood_queue)
		|| world_remote_relight_pending(&gstate.world);
}

static void frame(void) {
	world_update_lighting(&gstate.world);
	emit(7);
	emit(world_lighting_driver_last_work());
	emit(world_lighting_driver_last_stale_drops());
	emit(stack_size(&gstate.world.lighting_flood_queue));
	emit(stack_size(&gstate.world.lighting_updates));
	assert(world_lighting_driver_last_work() <= LIGHTING_CLIENT_POPS_PER_FRAME);
	mesh_state();
}

/* REL-MP-FIX3d2 (lead_note_tape2_1020): every taped drain must end at the
 * UNCAPPED reference relight (FIX3c1's reference_flood: growable LIFO, no pop
 * ceiling, lighting_flood's relaxation) of the same world state, seeded with
 * the bounded relight's pending cells. CW_R17_LEGACY=1 instead replays the
 * pre-FIX3d2 stack path (no reservation) and prints its 500k abandons. */
static bool legacy;
#define RW 32 /* the fixture's 2x2 columns, x and z in [-16, 16) */
static uint8_t ref_sky[RW][RW][WORLD_HEIGHT], ref_torch[RW][RW][WORLD_HEIGHT];
static uint8_t ref_type[RW][RW][WORLD_HEIGHT], ref_height[RW][RW];
static bool ref_col[2][2];
static bool ref_exists(int x, int y, int z) { /* world coords */
	return x >= -16 && x < 16 && z >= -16 && z < 16 && y >= 0
		&& y < WORLD_HEIGHT && ref_col[(x + 16) >> 4][(z + 16) >> 4];
}
static int (*ref_q)[3];
static size_t ref_len, ref_cap;
static void ref_push(int x, int y, int z) {
	if(ref_len == ref_cap) /* grow or die: never drops a successor */
		ref_q = realloc(ref_q, (ref_cap = ref_cap ? ref_cap * 2 : 4096)
			* sizeof(*ref_q)), assert(ref_q);
	ref_q[ref_len][0] = x, ref_q[ref_len][1] = y, ref_q[ref_len++][2] = z;
}
static size_t ref_pops, ref_seeds, ref_checked;
static bool ref_skip; /* legacy: a frontier already in flight */
static void ref_begin(void) {
	struct world* w = &gstate.world;
	for(int i = 0; i < 4; i++) {
		struct world_section* sec = dict_wsection_get(w->sections,
			SECTION_TO_ID(i / 2 - 1, i % 2 - 1));
		ref_col[i / 2][i % 2] = sec && !sec->is_far_lod
			&& world_find_chunk(w, (i / 2 - 1) * 16, 0, (i % 2 - 1) * 16);
	}
	for(int x = -16; x < 16; x++)
		for(int z = -16; z < 16; z++) {
			ref_height[x + 16][z + 16] = ref_exists(x, 0, z)
				? (uint8_t)world_get_height(w, x, z) : 0;
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				struct block_data b = world_get_block(w, x, y, z);
				ref_type[x + 16][z + 16][y] = b.type;
				ref_sky[x + 16][z + 16][y] = b.sky_light;
				ref_torch[x + 16][z + 16][y] = b.torch_light;
			}
		}
	/* Seeds = the pending bits, read through the FIX3c2 ledger layout:
	 * header(16) | rr_slot(16)[n] | u16 active[n] | pad 8 | u64 bits[n][512];
	 * bit = y | (z&15)<<7 | (x&15)<<11; a slot is live iff active_pos != 0xFFFF. */
	ref_len = ref_seeds = 0;
	if(legacy) { /* legacy forced seeds == the seed plus its six neighbours */
		ref_skip = !lighting_flood_queue_empty(&w->lighting_flood_queue);
		for(size_t i = 0; !ref_skip && i < stack_size(&w->lighting_updates); i++) {
			struct world_modification_entry e;
			stack_at(&w->lighting_updates, &e, i);
			ref_push(e.x, e.y, e.z), ref_seeds++;
			for(int d = 0; d < 6; d++)
				ref_push(e.x + (d == 0) - (d == 1), e.y + (d == 2) - (d == 3),
						 e.z + (d == 4) - (d == 5));
		}
	}
	const uint32_t* hdr = (const uint32_t*)w->remote_relight;
	assert(legacy || (hdr && hdr[0] == 0x52524C54u));
	size_t n = legacy ? 0 : hdr[1], off = (16 + 18 * n + 7) & ~(size_t)7;
	assert(legacy || lighting_remote_relight_bytes(n) == off + n * 4096);
	for(size_t sl = 0; sl < n; sl++) {
		const unsigned char* m = (const unsigned char*)hdr + 16 + 16 * sl;
		int32_t cx, cz; uint16_t pos;
		memcpy(&cx, m, 4), memcpy(&cz, m + 4, 4), memcpy(&pos, m + 14, 2);
		const uint64_t* bits = (const uint64_t*)((const char*)hdr + off) + sl * 512;
		for(uint32_t bit = 0; pos != 0xFFFFu && bit < 32768u; bit++)
			if(bits[bit >> 6] >> (bit & 63) & 1u)
				ref_push(cx * 16 + (int)(bit >> 11), (int)(bit & 127),
						 cz * 16 + (int)((bit >> 7) & 15)), ref_seeds++;
	}
	const bool nether = w->dimension == WORLD_DIM_NETHER;
	static const int off6[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
	ref_pops = 0;
	while(ref_len) {
		int* e = ref_q[--ref_len];
		int x = e[0], y = e[1], z = e[2];
		ref_pops++;
		if(!ref_exists(x, y, z))
			continue;
		struct block* b = blocks[ref_type[x + 16][z + 16][y]];
		int sky = !nether && y >= ref_height[x + 16][z + 16] ? 15 : 0;
		int torch = b ? b->luminance : 0, op = b && b->opacity > 1 ? b->opacity : 1;
		for(int d = 0; (!b || b->can_see_through) && d < 6; d++) {
			int nx = x + off6[d][0], ny = y + off6[d][1], nz = z + off6[d][2];
			if(!ref_exists(nx, ny, nz))
				continue;
			if(!nether && ref_sky[nx + 16][nz + 16][ny] - op > sky)
				sky = ref_sky[nx + 16][nz + 16][ny] - op;
			if(ref_torch[nx + 16][nz + 16][ny] - op > torch)
				torch = ref_torch[nx + 16][nz + 16][ny] - op;
		}
		if(sky == ref_sky[x + 16][z + 16][y] && torch == ref_torch[x + 16][z + 16][y])
			continue;
		ref_sky[x + 16][z + 16][y] = (uint8_t)sky;
		ref_torch[x + 16][z + 16][y] = (uint8_t)torch;
		for(int d = 0; d < 6; d++)
			if(ref_exists(x + off6[d][0], y + off6[d][1], z + off6[d][2]))
				ref_push(x + off6[d][0], y + off6[d][1], z + off6[d][2]);
	}
}

static void ref_check(unsigned frames, uint32_t abandons) {
	size_t wrong = 0;
	for(int x = -16; x < 16; x++)
		for(int z = -16; z < 16; z++)
			for(int y = 0; ref_exists(x, 0, z) && y < WORLD_HEIGHT; y++) {
				struct block_data b = world_get_block(&gstate.world, x, y, z);
				wrong += b.type != ref_type[x + 16][z + 16][y]
					|| b.sky_light != ref_sky[x + 16][z + 16][y]
					|| b.torch_light != ref_torch[x + 16][z + 16][y];
			}
	if(legacy) {
		printf("  R17 legacy drain %zu: %u frames, 500k ceiling abandons +%u, ",
			++ref_checked, frames, abandons);
		if(ref_skip) printf("frontier in flight: no reference\n");
		else printf("%zu cells differ from the uncapped reference of its own "
			"pre-drain state (%zu seeds, %zu pops)\n", wrong, ref_seeds, ref_pops);
		return;
	}
	printf("  R17 drain %zu: %zu pending seeds, uncapped reference %zu pops, "
		"%zu cells differ\n", ++ref_checked, ref_seeds, ref_pops, wrong);
	assert(wrong == 0 && "bounded remote relight != uncapped reference");
}

static void drain(void) {
	unsigned frames = 0;
	const uint32_t hits0 = lighting_flood_ceiling_hits();
	ref_begin();
	do {
		frame();
		assert(++frames < 2048 && "remote flood did not drain");
	} while(pending());
	ref_check(frames, lighting_flood_ceiling_hits() - hits0);
	canonical();
}

static void login(enum world_dim dim) {
	uint32_t previous = clin_remote_world_generation();
	emit(8); emit(dim);
	clin_force_world_reset_login(dim, 77);
	assert(clin_remote_world_generation() != 0);
	assert(clin_remote_world_generation() != previous);
	assert(gstate.world.dimension == dim && !pending());
	assert(dict_wsection_size(gstate.world.sections) == 0);
	assert(!gstate.world_load_failed);
	if(legacy && !untaped) /* pre-FIX3d2 path: no bounded reservation */
		world_remote_relight_end(&gstate.world);
	mesh_state();
}

static void fixture(enum world_dim dim) {
	login(dim);
	install(2);
	volume(-2, 30, -2, 4, 4, 4, 9); /* X/Z negative seams, Y 31/32 shells */
	canonical();
	static const int ys[] = {0, 15, 16, 31, 32, 127};
	for(unsigned i = 0; i < sizeof(ys) / sizeof(ys[0]); i++)
		edit(i & 1 ? 0 : -1, ys[i], i & 1 ? -1 : 0, BLOCK_TORCH, 1 + i % 5);
	drain();
	for(unsigned i = 0; i < sizeof(ys) / sizeof(ys[0]); i++)
		edit(i & 1 ? 0 : -1, ys[i], i & 1 ? -1 : 0, BLOCK_AIR, 0);
	for(int z = -2; z <= 1; z++)
		for(int x = -2; x <= 1; x++)
			edit(x, 63, z, BLOCK_AIR, 0);
	drain();
	for(int z = -2; z <= 1; z++)
		for(int x = -2; x <= 1; x++)
			edit(x, 63, z, BLOCK_STONE, (x - z) & 15);
	drain();

	/* Fresh asymmetric payload guarantees an active frontier, in both dims. */
	volume(-16, 0, -16, 16, WORLD_HEIGHT, 16, 11);
	edit(-1, 31, -1, BLOCK_TORCH, 4);
	frame(); /* FIX3d2: the remote frontier is the bounded relight's bitmap */
	assert(pending());
	emit(9);
	struct client_rpc unload = {.type = CRPC_UNLOAD_CHUNK,
		.payload.unload_chunk = {.x = -1, .z = -1}};
	dispatch(&unload);
	assert(world_find_chunk(&gstate.world, -1, 31, -1) == NULL);
	drain();
	volume(-16, 0, -16, 16, WORLD_HEIGHT, 16, 5);
	edit(-1, 32, -1, BLOCK_TORCH, 3);
	frame();
	assert(pending());
	login(dim); /* discard pending work at reset, reinstall owned columns */
	install(7);
	edit(0, 16, -1, BLOCK_TORCH, 2);
	drain();

	edit(-1, 31, 0, BLOCK_TORCH, 1);
	assert(pending());
	emit(10);
	clin_force_world_reset(); /* production disconnect teardown */
	assert(clin_remote_world_generation() == 0 && !pending());
	assert(dict_wsection_size(gstate.world.sections) == 0);
	mesh_state();
	login(dim); /* fresh remote identity; no previous-session flood survives */
	install(13);
	edit(-1, 15, -1, BLOCK_TORCH, 5);
	edit(0, 16, 0, BLOCK_TORCH, 2);
	drain();
	edit(-1, 15, -1, BLOCK_AIR, 0);
	drain();
	printf("remote dimension %d: owned ingress/seams/eviction/reset/rejoin PASS\n", dim);
}

/* REL-MP-FIX3b (NOT taped): a vanilla server streams light-only Packet53/52
 * with unchanged id+meta (server World.java:491-501, PlayerInstance.java:
 * 122-166); Beta's client drops them with no write and no light work
 * (Chunk.java:240-241). Real column with a cave whose x=-1 face touches a
 * FIX2b-expanded neighbour (all air, sky 15, heightmap 0): stored light is
 * deliberately inconsistent there, so any seed would flood. */
static bool no_sky; /* FIX3d1 Nether pass: every supplied sky nibble is 0 */
static struct block_data cave_cell(int lx, int lz, int ly, bool expanded) {
	bool cave = lx >= 8 && lz >= 4 && lz < 12 && ly >= 20 && ly < 40;
	bool solid = !expanded && ly < 64 && !cave;
	return (struct block_data) {.type = solid ? BLOCK_STONE : BLOCK_AIR,
		.sky_light = !no_sky && (expanded || ly >= 64) ? 15u : 0u};
}

static void cave_column(int x, bool expanded) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	for(int lx = 0; lx < 16; lx++)
		for(int lz = 0; lz < 16; lz++)
			for(int ly = 0; ly < WORLD_HEIGHT; ly++) {
				size_t i = ((size_t)lx * 16 + lz) * WORLD_HEIGHT + ly;
				struct block_data b = cave_cell(lx, lz, ly, expanded);
				slot->ids[i] = b.type;
				nibble_write(slot->lighting_sky, i, b.sky_light);
			}
	memset(slot->heightmap, expanded ? 0 : 64, sizeof(slot->heightmap));
	struct client_rpc call = {.type = CRPC_CHUNK,
		.payload.chunk = {.x = x, .y = 0, .z = 0, .sx = 16,
			.sy = WORLD_HEIGHT, .sz = 16, .slot = slot}};
	dispatch(&call);
}

static struct block_data snap[2][16][16][WORLD_HEIGHT];

static void snapshot(bool compare) {
	for(int c = 0; c < 2; c++)
		for(int lx = 0; lx < 16; lx++)
			for(int lz = 0; lz < 16; lz++)
				for(int y = 0; y < WORLD_HEIGHT; y++) {
					struct block_data b = world_get_block(&gstate.world,
						c * 16 - 16 + lx, y, lz);
					struct block_data* s = &snap[c][lx][lz][y];
					if(!compare) *s = b;
					else assert(b.type == s->type && b.metadata == s->metadata
						&& b.sky_light == s->sky_light
						&& b.torch_light == s->torch_light);
				}
}

static void unchanged_remote_edits(void) {
	untaped = true;
	login(WORLD_DIM_OVERWORLD);
	cave_column(-16, false);
	cave_column(0, true);
	assert(!pending());
	snapshot(false);
	const uint32_t ceil0 = lighting_flood_ceiling_hits();
	const uint32_t max0 = lighting_client_flood_max_pops();
	const uint32_t skip0 = world_remote_unchanged_skips();
	unsigned queued = 0;
	for(unsigned k = 0; k < 200; k++) {
		int x = -8 + (int)(k % 16), y = k & 1 ? 10 : 20 + (int)(k / 16) % 20;
		int z = 4 + (int)(k / 2) % 8;
		struct block_data b = world_get_block(&gstate.world, x, y, z);
		edit(x, y, z, b.type, b.metadata);
		queued += !stack_empty(&gstate.world.lighting_updates);
	}
	unsigned frames = 0;
	while(pending() && frames < 2048) {
		world_update_lighting(&gstate.world);
		frames++;
	}
	printf("FIX3b: 200 unchanged edits -> %u left a relight queued, %u frames,"
		" ceiling hits +%u, flood max %u -> %u, skips +%u\n", queued, frames,
		lighting_flood_ceiling_hits() - ceil0, max0,
		lighting_client_flood_max_pops(), world_remote_unchanged_skips() - skip0);
	fflush(stdout);
	assert(queued == 0 && frames == 0);
	assert(lighting_flood_ceiling_hits() == ceil0);
	assert(lighting_client_flood_max_pops() == max0);
	assert(world_remote_unchanged_skips() - skip0 == 200);
	snapshot(true); /* ids, meta and BOTH light nibbles untouched */
	clin_force_world_reset();
	untaped = false;
	printf("remote unchanged id+meta edits: no write, no relight PASS\n");
}

/* REL-MP-FIX3d1 (NOT taped): genuine remote edits enter the bounded relight
 * (FIX3c2), never the growing lighting_updates stack; frames progress within
 * budget; final light is a fixpoint of lighting.c's relaxation. */
static size_t legacy_seeds;
static void genuine(int x, int y, int z, uint8_t type, uint8_t meta) {
	edit(x, y, z, type, meta);
	legacy_seeds += !stack_empty(&gstate.world.lighting_updates);
}

static unsigned paced_drain(void) {
	unsigned frames = 0;
	for(; pending(); assert(++frames < 1024 && "remote relight did not converge")) {
		world_update_lighting(&gstate.world);
		size_t work = world_lighting_driver_last_work();
		assert(work > 0 && "pending remote relight made no progress");
		assert(work <= LIGHTING_CLIENT_POPS_PER_FRAME);
	}
	return frames;
}

static void fixpoint(void) {
	static const int off[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
	for(int x = -16; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				struct block_data b = world_get_block(&gstate.world, x, y, z), n;
				struct block* k = blocks[b.type];
				int sky = !no_sky && y >= world_get_height(&gstate.world, x, z) ? 15 : 0;
				int torch = k ? k->luminance : 0, op = k && k->opacity > 1 ? k->opacity : 1;
				for(int d = 0; d < 6 && (!k || k->can_see_through); d++) {
					int nx = x + off[d][0], ny = y + off[d][1], nz = z + off[d][2];
					if(ny < 0 || ny >= WORLD_HEIGHT /* unloaded: no read, as production */
					   || !world_find_chunk(&gstate.world, nx, ny, nz))
						continue;
					n = world_get_block(&gstate.world, nx, ny, nz);
					if(!no_sky && n.sky_light - op > sky) sky = n.sky_light - op;
					if(n.torch_light - op > torch) torch = n.torch_light - op;
				}
				if(b.sky_light != sky || b.torch_light != torch)
					fprintf(stderr, "FIX3d fixpoint (%d,%d,%d): sky %u/%d torch %u/%d\n",
						x, y, z, b.sky_light, sky, b.torch_light, torch), exit(1);
			}
}

static void is_payload(int x0, bool expanded) { /* no stale replay across a body */
	for(int i = 0; i < 16 * 16 * WORLD_HEIGHT; i++) {
		struct block_data e = cave_cell(i / 2048, (i / 128) % 16, i % 128, expanded);
		struct block_data b = world_get_block(&gstate.world, x0 + i / 2048, i % 128,
			(i / 128) % 16);
		assert(b.type == e.type && b.metadata == 0 && b.sky_light == e.sky_light
			&& b.torch_light == 0);
	}
}

static void glass_band(int x, int y, int z) { /* partial 4x4x4 Packet51 body */
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	for(size_t i = 0; i < 64; i++) { /* glass opacity 0: supplied sky consistent */
		slot->ids[i] = BLOCK_GLASS;
		nibble_write(slot->lighting_sky, i, no_sky ? 0u : 15u);
	}
	struct client_rpc call = {.type = CRPC_CHUNK, .payload.chunk = {.x = x,
		.y = y, .z = z, .sx = 4, .sy = 4, .sz = 4, .slot = slot}};
	dispatch(&call);
}

static void genuine_remote_edits(enum world_dim dim) {
	untaped = true;
	no_sky = dim == WORLD_DIM_NETHER;
	login(dim); /* dimension change: no previous-dimension obligation survives */
	cave_column(-16, false);
	genuine(-1, 30, 8, BLOCK_TORCH, 1); /* x=0 neighbour not loaded yet */
	cave_column(0, true);               /* delayed full body of the shaft */
	unsigned f1 = paced_drain();
	fixpoint();
	glass_band(4, 80, 4);
	genuine(-1, 30, 8, BLOCK_STONE, 0); /* darken beside the shaft */
	genuine(0, 70, 8, BLOCK_STONE, 0);  /* shadow inside the shaft */
	genuine(-6, 25, 7, BLOCK_TORCH, 3); /* deep cave */
	genuine(5, 79, 5, BLOCK_TORCH, 5);  /* under the partial band */
	unsigned f2 = paced_drain();
	fixpoint();
	for(int pass = 0; pass < 2; pass++) { /* overwrite, then unload + reload */
		genuine(-4, 30, 8, BLOCK_TORCH, 2);
		struct client_rpc unload = {.type = CRPC_UNLOAD_CHUNK,
			.payload.unload_chunk = {.x = -1, .z = 0}};
		if(pass) /* unload drops the column's obligations, then reload */
			dispatch(&unload), assert(!world_find_chunk(&gstate.world, -4, 30, 8));
		/* pass 0: the edit is pending; pass 1: the unload dropped it */
		assert(world_remote_relight_pending(&gstate.world) == !pass);
		cave_column(-16, false); /* authoritative body before any frame */
		assert(!world_remote_relight_pending(&gstate.world));
		paced_drain();
		is_payload(-16, false);
	}
	genuine(-4, 30, 8, BLOCK_TORCH, 2); /* left pending for the next login */
	printf("FIX3d genuine edits dim %d: %u + %u frames, fixpoints, overwrite/unload/"
		"reload exact; legacy-stack seeds so far %zu\n", dim, f1, f2, legacy_seeds);
	untaped = false;
}

/* REL-MP-FIX3d2 attempt 2: one cell's pending bit and the total pending count,
 * read through the FIX3c2 ledger layout (see ref_begin). */
static bool relight_bit(int x, int y, int z, size_t* total) {
	const uint32_t* hdr = (const uint32_t*)gstate.world.remote_relight;
	assert(hdr && hdr[0] == 0x52524C54u);
	size_t n = hdr[1], off = (16 + 18 * n + 7) & ~(size_t)7;
	bool hit = false;
	*total = 0;
	for(size_t sl = 0; sl < n; sl++) {
		const unsigned char* m = (const unsigned char*)hdr + 16 + 16 * sl;
		int32_t cx, cz; uint32_t count; uint16_t pos;
		memcpy(&cx, m, 4), memcpy(&cz, m + 4, 4), memcpy(&count, m + 8, 4);
		memcpy(&pos, m + 14, 2);
		if(pos == 0xFFFFu)
			continue;
		*total += count;
		if(cx == WCOORD_CHUNK_OFFSET(x) && cz == WCOORD_CHUNK_OFFSET(z)) {
			uint32_t bit = (uint32_t)y | (uint32_t)(z & 15) << 7
				| (uint32_t)(x & 15) << 11;
			hit = ((const uint64_t*)((const char*)hdr + off))[sl * 512 + (bit >> 6)]
				>> (bit & 63) & 1u;
		}
	}
	return hit;
}

/* Sol P2: a partial Packet51 clears ONLY the cells it overwrites. Two pending
 * regions (a cave torch in column -1, a shaft torch in column 0); the 4x4x4
 * glass body covers 6 of the shaft seed's 7 cells and none of the cave's. */
static void partial_body_clears_only_overwritten(void) {
	static const int c7[7][3] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
		{0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	static const int seed[2][3] = {{-6, 25, 7}, {5, 100, 5}};
	untaped = true;
	no_sky = false;
	login(WORLD_DIM_OVERWORLD);
	cave_column(-16, false);
	cave_column(0, true);
	genuine(seed[0][0], seed[0][1], seed[0][2], BLOCK_TORCH, 3);
	genuine(seed[1][0], seed[1][1], seed[1][2], BLOCK_TORCH, 4);
	size_t total;
	for(int r = 0; r < 2; r++)
		for(int k = 0; k < 7; k++)
			assert(relight_bit(seed[r][0] + c7[k][0], seed[r][1] + c7[k][1],
				seed[r][2] + c7[k][2], &total));
	assert(total == 14);
	glass_band(4, 100, 4); /* x 4..7, y 100..103, z 4..7 */
	size_t kept = 0;
	for(int r = 0; r < 2; r++)
		for(int k = 0; k < 7; k++) {
			int x = seed[r][0] + c7[k][0], y = seed[r][1] + c7[k][1],
				z = seed[r][2] + c7[k][2];
			bool inside = x >= 4 && x < 8 && y >= 100 && y < 104 && z >= 4 && z < 8;
			assert(relight_bit(x, y, z, &total) == !inside);
			kept += !inside;
		}
	assert(kept == 8 && total == 8 && "pending bits outside the body changed");
	ref_begin(); /* uncapped reference from the surviving obligations */
	unsigned frames = paced_drain();
	ref_check(frames, 0);
	printf("FIX3d2 partial Packet51: 6 overwritten bits cleared, 8 kept, %u frames,"
		" == uncapped reference PASS\n", frames);
	untaped = false;
}

/* Sol P2: the production login's reservation failure (injected allocation
 * failure) keeps readiness closed and refuses legacy ingress; the next login
 * recovers. */
void clin_test_fail_remote_relight_alloc(bool fail);
static void reservation_failure_closes_login(void) {
	untaped = true;
	gstate.remote_session = false;
	clin_force_world_reset(); /* a local reset releases the reservation */
	assert(!gstate.world.remote_relight && !gstate.world.remote_relight_failed);
	gstate.remote_session = true;
	clin_test_fail_remote_relight_alloc(true);
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 77);
	assert(gstate.world_load_failed && !gstate.world_loaded);
	assert(!gstate.world.remote_relight && gstate.world.remote_relight_failed);
	load_failed_ok = true; /* ingress after the failure: body, then an edit */
	cave_column(-16, false);
	struct client_rpc set = {.type = CRPC_SET_BLOCK, .payload.set_block = {
		.x = -6, .y = 25, .z = 7, .block = {.type = BLOCK_TORCH, .metadata = 1}}};
	dispatch(&set);
	load_failed_ok = false;
	/* The failed reset leaves the terrain gate closed: neither the body nor
	 * the edit reaches the world, so nothing can be seeded anywhere. */
	assert(!world_find_chunk(&gstate.world, -6, 25, 7)
		&& dict_wsection_size(gstate.world.sections) == 0);
	assert(!pending() && "failed reservation let a seed reach a relight path");
	world_update_lighting(&gstate.world);
	assert(world_lighting_driver_last_work() == 0 && gstate.world_load_failed);
	clin_test_fail_remote_relight_alloc(false);
	login(WORLD_DIM_OVERWORLD); /* recovers: reservation attached, load open */
	assert(gstate.world.remote_relight && !gstate.world.remote_relight_failed);
	cave_column(-16, false);
	genuine(-6, 25, 7, BLOCK_TORCH, 1);
	assert(world_remote_relight_pending(&gstate.world));
	paced_drain();
	puts("FIX3d2 reservation failure: load closed, no legacy ingress, next login "
		"recovers PASS");
	untaped = false;
}

int main(int argc, char** argv) {
	assert(argc == 3);
	record = !strcmp(argv[1], "--record");
	assert(record || !strcmp(argv[1], "--golden"));
	tape = gzopen(argv[2], record ? "wb" : "rb");
	assert(tape);
	legacy = getenv("CW_R17_LEGACY") != NULL;
	blocks_init();
	items_init();
	assert(svin_init() && clin_init());
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	gstate.remote_session = true;
	gstate.players[0].visible_window = -1;
	struct config config = {.root = json_value_init_object()};
	assert(config.root);
	for(unsigned mode = 0; mode < (record ? 1u : 2u); mode++) {
		assert(config_set_int(&config, "experimental.g9_settled_light_reuse_int", mode));
		column_light_configure(config_g9_settled_light_reuse(&config));
		assert(column_light_enabled() == (mode == 1));
		bytes = writes = dirty_events = 0;
		chunk_settle_observe = dirty;
		emit(0x52313702); /* R17 replay; write/dirty runs digested (SHRINK) */
		fixture(WORLD_DIM_OVERWORLD);
		fixture(WORLD_DIM_NETHER);
		clin_force_world_reset();
		flush_run();
		chunk_settle_observe = NULL;
		assert(writes > 0 && dirty_events > 0);
		if(!record && gzgetc(tape) != EOF) {
			fprintf(stderr, "remote golden has trailing bytes at %zu\n", bytes);
			return 1;
		}
		printf("remote frozen tape: %zu bytes, %zu writes, %zu dirty events reuse=%u %s\n",
			bytes, writes, dirty_events, mode, record ? "RECORDED" : "PASS");
		if(!record && mode == 0) assert(gzrewind(tape) == 0);
	}
	unchanged_remote_edits();
	genuine_remote_edits(WORLD_DIM_OVERWORLD);
	genuine_remote_edits(WORLD_DIM_NETHER);
	partial_body_clears_only_overwritten();
	reservation_failure_closes_login();
	clin_force_world_reset();
	assert(!pending() && !fflush(stdout));
	assert(legacy_seeds == 0
		&& "RED: genuine remote seeds still go to the growing lighting_updates stack");
	puts("remote genuine edits: bounded lossless relight PASS");
	struct column_light_stats stats;
	column_light_get_stats(&stats);
	assert(stats.hits == 0 && stats.live == 0);
	column_light_configure(false);
	config_destroy(&config);
	assert(gzclose(tape) == Z_OK);
	return 0;
}

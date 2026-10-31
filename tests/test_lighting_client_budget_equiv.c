/*
	The T1 byte-exactness guard for the far-ring freeze fix (the client/main-thread
	relight per-frame POP BUDGET). world_update_lighting used to run each deferred relight
	as ONE unbudgeted lighting_update_at_block flood on the render thread; a large surface
	skylight flood then ran multi-second on the 729 MHz Broadway core WITHOUT advancing the
	main-loop watchdog heartbeat -> the watchdog fired (row1 = WDM_LIGHTING). The fix carries
	the flood's BFS frontier across frames in a persistent queue and spends only a bounded
	pop budget per frame (lighting_flood_seed + repeated lighting_flood_resume).

	This pins the load-bearing claim that the pacing is PURE -- it changes only WHEN cells are
	visited, never WHICH or to WHAT value:

	    lighting_update_at_block(source)               // one unbudgeted flood (the old path)
	      ==  lighting_flood_seed(source);             // resumed in slices of ANY size
	          while(!empty) lighting_flood_resume(BUDGET)

	-- a BYTE-IDENTICAL sky+torch light field, for EVERY budget size from 1 pop/slice
	(maximally fragmented resume) up to one-shot, over a fuzz of random terrain + edits.
	If this fails, the budget is NOT a no-op on the world's rules and must not ship.

	Pure unit: links only source/lighting.c + source/stack.c with a local flat block_data
	grid + a minimal blocks[] table (the test_lighting_multi harness pattern).
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "lighting.h"
#include "stack.h"

// --- minimal block table (the flood reads can_see_through/opacity/luminance) ----
static struct block b_solid = {.can_see_through = false, .opacity = 15};
static struct block b_leaves = {.can_see_through = true, .opacity = 1};
static struct block b_lava
	= {.can_see_through = false, .opacity = 15, .luminance = 15};
static struct block b_torch
	= {.can_see_through = true, .opacity = 0, .luminance = 14};
struct block* blocks[256] = {0};
static void setup_blocks(void) {
	blocks[BLOCK_STONE] = blocks[BLOCK_DIRT] = blocks[BLOCK_GRASS]
		= blocks[BLOCK_BEDROCK] = blocks[BLOCK_LOG] = &b_solid;
	blocks[BLOCK_LEAVES] = &b_leaves;
	blocks[BLOCK_LAVA_STILL] = blocks[BLOCK_LAVA_FLOW] = &b_lava;
	blocks[BLOCK_TORCH] = &b_torch;
}

void blocks_side_offset(enum side s, int* x, int* y, int* z) {
	static const int off[6][3]
		= {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
	*x = off[s][0];
	*y = off[s][1];
	*z = off[s][2];
}
uint8_t nibble_read(uint8_t* base, size_t idx) {
	return (idx & 1) ? (uint8_t)(base[idx >> 1] >> 4)
					 : (uint8_t)(base[idx >> 1] & 0x0F);
}
void nibble_write(uint8_t* base, size_t idx, uint8_t v) {
	size_t i = idx >> 1;
	if(idx & 1)
		base[i] = (uint8_t)((base[i] & 0x0F) | ((v & 0x0F) << 4));
	else
		base[i] = (uint8_t)((base[i] & 0xF0) | (v & 0x0F));
}

// --- a flat block_data grid (LW must equal CHUNK_SIZE: heightmap stride) ----------
#define LW 16
#define H 128
struct cell {
	uint8_t type, sky, torch, meta;
};
static struct cell gworld[LW * LW * H];
static uint8_t gheight[LW * LW];
// Cells marked here are "UNLOADED" (their chunk evicted): g_get returns false for them
// AND g_set asserts it is never called on one -- the regression guard's contract. All
// false by default, so the equivalence cases above are unaffected.
static bool g_evicted[LW * LW * H];
static size_t gidx(int x, int y, int z) {
	return (size_t)y + ((size_t)z + (size_t)x * LW) * H;
}
static bool g_get(void* u, w_coord_t x, w_coord_t y, w_coord_t z,
				  struct block_data* blk, uint8_t* height) {
	(void)u;
	if(x < 0 || x >= LW || z < 0 || z >= LW || y < 0 || y >= H)
		return false;
	if(g_evicted[gidx(x, y, z)])
		return false; // chunk unloaded mid-flood -> out of world
	struct cell* c = &gworld[gidx(x, y, z)];
	if(blk) {
		blk->type = c->type;
		blk->metadata = c->meta;
		blk->sky_light = c->sky;
		blk->torch_light = c->torch;
	}
	if(height)
		*height = gheight[x + z * LW];
	return true;
}
static void g_set(void* u, w_coord_t x, w_coord_t y, w_coord_t z, uint8_t light) {
	(void)u;
	if(x < 0 || x >= LW || z < 0 || z >= LW || y < 0 || y >= H)
		return;
	// The crash this guards: on real (NDEBUG) hardware world_light_set_light deref'd a
	// NULL chunk when the paced flood relit an evicted cell. The lighting_flood get_block
	// guard must prevent ever reaching set_light for an unloaded cell.
	assert(!g_evicted[gidx(x, y, z)]
		   && "relight wrote an UNLOADED cell -- evict guard failed (the Wii crash)");
	struct cell* c = &gworld[gidx(x, y, z)];
	c->sky = light & 0x0F;
	c->torch = light >> 4;
}
static bool hm_get(void* u, c_coord_t x, w_coord_t y, c_coord_t z,
				   struct block_data* blk) {
	(void)u;
	if(x >= LW || z >= LW || y < 0 || y >= H)
		return false;
	struct cell* c = &gworld[gidx(x, y, z)];
	if(blk) {
		blk->type = c->type;
		blk->metadata = c->meta;
	}
	return true;
}

static void compute_height_all(void) {
	for(int x = 0; x < LW; x++)
		for(int z = 0; z < LW; z++) {
			int h = 0;
			for(int y = H - 1; y >= 0; y--) {
				uint8_t t = gworld[gidx(x, y, z)].type;
				if(blocks[t]
				   && (!blocks[t]->can_see_through || blocks[t]->opacity > 0)) {
					h = y + 1;
					break;
				}
			}
			gheight[x + z * LW] = (uint8_t)h;
		}
}

static struct world_modification_entry* g_allsrc = NULL;
static void relight_all(void) {
	size_t n = 0;
	for(int x = 0; x < LW; x++)
		for(int z = 0; z < LW; z++) {
			int h = gheight[x + z * LW];
			if(h < H)
				g_allsrc[n++]
					= (struct world_modification_entry) {.x = x, .y = h, .z = z};
			for(int y = 0; y < H; y++) {
				uint8_t t = gworld[gidx(x, y, z)].type;
				if(blocks[t] && blocks[t]->luminance > 0)
					g_allsrc[n++] = (struct world_modification_entry) {
						.x = x, .y = y, .z = z};
			}
		}
	lighting_update_at_blocks(g_allsrc, n, false, g_get, g_set, NULL);
}

struct edit {
	int x, y, z;
	uint8_t type, meta;
};

static struct cell W0[LW * LW * H];
static uint8_t W0h[LW * LW];
static struct cell snapRef[LW * LW * H];

// Apply `edits` then a relight, two ways from the captured W0, and assert the light
// field is byte-identical:
//   Arm REF -- each edit's relight is ONE unbudgeted lighting_update_at_block flood.
//   Arm BUD -- each edit's relight is seeded then resumed in slices of `budget` pops,
//              exactly as world_update_lighting paces it across frames.
static void run_case(const struct edit* edits, int n, size_t budget,
					 const char* name) {
	// ---- Arm REF: unbudgeted single-source flood per edit (the OLD client path) ----
	memcpy(gworld, W0, sizeof(gworld));
	memcpy(gheight, W0h, sizeof(gheight));
	for(int i = 0; i < n; i++) {
		struct cell* c = &gworld[gidx(edits[i].x, edits[i].y, edits[i].z)];
		c->type = edits[i].type;
		c->meta = edits[i].meta;
		lighting_heightmap_update(gheight, edits[i].x, edits[i].y, edits[i].z,
								  edits[i].type, hm_get, NULL);
		lighting_update_at_block(
			(struct world_modification_entry) {
				.x = edits[i].x, .y = edits[i].y, .z = edits[i].z},
			false, g_get, g_set, NULL);
	}
	memcpy(snapRef, gworld, sizeof(gworld));

	// ---- Arm BUD: seed + resume in `budget`-pop slices (the NEW paced path) ----
	memcpy(gworld, W0, sizeof(gworld));
	memcpy(gheight, W0h, sizeof(gheight));
	struct stack q;
	lighting_flood_queue_create(&q);
	for(int i = 0; i < n; i++) {
		struct cell* c = &gworld[gidx(edits[i].x, edits[i].y, edits[i].z)];
		c->type = edits[i].type;
		c->meta = edits[i].meta;
		lighting_heightmap_update(gheight, edits[i].x, edits[i].y, edits[i].z,
								  edits[i].type, hm_get, NULL);
		lighting_flood_seed(&q, edits[i].x, edits[i].y, edits[i].z, true);
		// Drain this flood in slices -- the resume boundary lands at an ARBITRARY point
		// in the BFS, which is exactly what a per-frame budget does on hardware.
		int guard = 0;
		while(!lighting_flood_queue_empty(&q)) {
			lighting_flood_resume(&q, false, budget, g_get, g_set, NULL);
			assert(++guard < 10000000 && "resume made no progress / never drains");
		}
	}
	lighting_flood_queue_destroy(&q);

	// ---- Compare the WHOLE light field, byte for byte ----
	size_t cells = (size_t)LW * LW * H;
	for(size_t i = 0; i < cells; i++) {
		if(gworld[i].sky != snapRef[i].sky
		   || gworld[i].torch != snapRef[i].torch) {
			size_t y = i % H, rest = i / H, z = rest % LW, x = rest / LW;
			fprintf(stderr,
					"%s (budget=%zu): light mismatch at (%zu,%zu,%zu): "
					"budgeted sky=%u torch=%u vs unbudgeted sky=%u torch=%u\n",
					name, budget, x, y, z, gworld[i].sky, gworld[i].torch,
					snapRef[i].sky, snapRef[i].torch);
			assert(!"budgeted/resumed flood != unbudgeted flood");
		}
	}
}

static uint32_t rng_state;
static uint32_t xr(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}
static int xr_range(int lo, int hi) {
	return lo + (int)(xr() % (uint32_t)(hi - lo));
}

// ===== REL-MP-FIX3c: remote relight saturation must be lossless ==============
// A 3x3-column world (9 slots), stone below FLOOR, open air above, every stored
// light byte stale 0: one forced seed must relight the whole air volume, which
// takes well over 500,000 pops (the legacy cumulative abandon point).
#define BW (3 * CHUNK_SIZE)
#define FLOOR 32
static struct cell bigw[BW * BW * H], bigref[BW * BW * H];
static const int TORCH[3][3] = {{5, 40, 5}, {30, 90, 17}, {44, 33, 41}};
static size_t bidx(int x, int y, int z) {
	return (size_t)y + ((size_t)z + (size_t)x * BW) * H;
}
static bool big_get(void* u, w_coord_t x, w_coord_t y, w_coord_t z,
					struct block_data* blk, uint8_t* height) {
	if(x < 0 || x >= BW || z < 0 || z >= BW || y < 0 || y >= H)
		return false;
	struct cell* c = &bigw[bidx(x, y, z)];
	if(blk)
		*blk = (struct block_data) {.type = c->type, .metadata = c->meta,
			.sky_light = c->sky, .torch_light = c->torch};
	if(height) *height = FLOOR;
	return true;
}
// Column-loaded predicate: user == NULL means all 9 columns, else a bit mask
// (bit cx * 3 + cz) of the loaded ones.
static bool big_loaded(void* u, w_coord_t cx, w_coord_t cz) {
	unsigned m = u ? *(const unsigned*)u : 0x1FFu;
	return cx >= 0 && cx < 3 && cz >= 0 && cz < 3 && (m >> (cx * 3 + cz) & 1u);
}
static void big_set(void* u, w_coord_t x, w_coord_t y, w_coord_t z, uint8_t l) {
	assert(big_get(u, x, y, z, NULL, NULL));
	bigw[bidx(x, y, z)].sky = l & 0x0F, bigw[bidx(x, y, z)].torch = l >> 4;
}
static void big_reset(void) {
	memset(bigw, 0, sizeof(bigw));
	for(size_t i = 0; i < (size_t)BW * BW * H; i++) // bidx: y == i % H
		bigw[i].type = (i % H) < FLOOR ? BLOCK_STONE : BLOCK_AIR;
	for(int t = 0; t < 3; t++)
		bigw[bidx(TORCH[t][0], TORCH[t][1], TORCH[t][2])].type = BLOCK_TORCH;
}
// UNCAPPED reference: the same relaxation as lighting_flood (Beta's
// MetadataChunkBlock.java:79-121: opacity 0 counts as 1, sky 15 where the
// column sees the sky, lightValue as the floor, max neighbour minus opacity),
// with a growable LIFO that asserts instead of dropping and no pop ceiling.
static size_t reference_flood(int sx, int sy, int sz) {
	size_t len = 0, cap = 1024, pops = 0;
	int(*q)[4] = malloc(cap * sizeof(*q));
	assert(q);
	q[len][0] = sx, q[len][1] = sy, q[len][2] = sz, q[len++][3] = 1;
	while(len) {
		int* e = q[--len];
		int cx = e[0], cy = e[1], cz = e[2], forced = e[3];
		pops++;
		struct cell* c = &bigw[bidx(cx, cy, cz)]; struct block* b = blocks[c->type];
		int sky = cy >= FLOOR ? 15 : 0, torch = b ? b->luminance : 0;
		int op = (b && b->opacity > 1) ? b->opacity : 1;
		for(enum side s = 0; (!b || b->can_see_through) && s < SIDE_MAX; s++) {
			int ox, oy, oz;
			struct block_data nb;
			blocks_side_offset(s, &ox, &oy, &oz);
			if(!big_get(NULL, cx + ox, cy + oy, cz + oz, &nb, NULL))
				continue;
			if(nb.sky_light - op > sky) sky = nb.sky_light - op;
			if(nb.torch_light - op > torch) torch = nb.torch_light - op;
		}
		if(sky == c->sky && torch == c->torch && !forced)
			continue;
		c->sky = (uint8_t)sky, c->torch = (uint8_t)torch;
		for(enum side s = 0; s < SIDE_MAX; s++) {
			int ox, oy, oz;
			blocks_side_offset(s, &ox, &oy, &oz);
			if(!big_get(NULL, cx + ox, cy + oy, cz + oz, NULL, NULL))
				continue;
			if(len == cap) // grow or die: the reference never drops a successor
				q = realloc(q, (cap *= 2) * sizeof(*q)), assert(q);
			q[len][0] = cx + ox, q[len][1] = cy + oy, q[len][2] = cz + oz;
			q[len++][3] = 0;
		}
	}
	free(q);
	return pops;
}
// Independent closed form: sky 15 above the floor, torch 14 minus the Manhattan
// distance to the nearest torch through open air, opaque stone 0/0.
static size_t count_wrong(const struct cell* w, bool vs_closed_form) {
	size_t bad = 0;
	for(int x = 0; x < BW; x++)
		for(int z = 0; z < BW; z++)
			for(int y = 0; y < H; y++) {
				size_t i = bidx(x, y, z); int sky = 0, torch = 0;
				if(!vs_closed_form) {
					sky = bigref[i].sky, torch = bigref[i].torch;
				} else if(y >= FLOOR) {
					sky = 15;
					for(int t = 0; t < 3; t++) {
						int d = abs(x - TORCH[t][0]) + abs(y - TORCH[t][1])
							+ abs(z - TORCH[t][2]);
						if(14 - d > torch) torch = 14 - d;
					}
				}
				bad += w[i].sky != sky || w[i].torch != torch;
			}
	return bad;
}
// Sol's saturation case: 2 slots, 2 loaded columns ((0,0) and (1,0)), a torch
// seed plus a seed on the edge facing the unloaded column (0,1). Before the
// loaded predicate the edge seed took a slot for (0,1), propagation into (1,0)
// then had no slot, and every drain returned NO_SLOT with zero work forever.
static void remote_relight_two_slots_never_stall(unsigned char* mem,
	size_t bytes) {
	unsigned mask = 1u << 0 | 1u << 3; // (0,0), (1,0); 0x1FF = the RED stall
	struct lighting_remote_relight* r = lighting_remote_relight_init(mem, bytes, 2);
	assert(r == (void*)mem);
	big_reset();
	assert(lighting_remote_relight_admit(r, 5, 41, 5, big_loaded, &mask)
		   == LIGHTING_REMOTE_PENDING);
	assert(lighting_remote_relight_admit(r, 8, 100, 15, big_loaded, &mask)
		   == LIGHTING_REMOTE_PENDING);
	size_t spent, calls = 0;
	enum lighting_remote_status st;
	while((st = lighting_remote_relight_drain(r, false, 7, &spent, big_get,
			   NULL, big_set, big_loaded, &mask)) != LIGHTING_REMOTE_IDLE) {
		assert(st == LIGHTING_REMOTE_PENDING && "stalled: NO_SLOT");
		assert(spent > 0 && ++calls < 50000000u && "no progress");
	}
	// Unloaded columns never written; loaded columns at the relaxation fixpoint.
	for(int x = 0; x < BW; x++)
		for(int z = 0; z < BW; z++)
			for(int y = 0; y < H; y++) {
				struct cell* c = &bigw[bidx(x, y, z)];
				if(!big_loaded(&mask, x / CHUNK_SIZE, z / CHUNK_SIZE)) {
					assert(!c->sky && !c->torch && "lit an unloaded column");
					continue;
				}
				struct block* b = blocks[c->type];
				int sky = y >= FLOOR ? 15 : 0, torch = b ? b->luminance : 0;
				int op = (b && b->opacity > 1) ? b->opacity : 1;
				for(enum side d = 0; (!b || b->can_see_through) && d < SIDE_MAX; d++) {
					int ox, oy, oz;
					struct block_data nb;
					blocks_side_offset(d, &ox, &oy, &oz);
					if(!big_get(NULL, x + ox, y + oy, z + oz, &nb, NULL))
						continue;
					if(nb.sky_light - op > sky) sky = nb.sky_light - op;
					if(nb.torch_light - op > torch) torch = nb.torch_light - op;
				}
				assert(c->sky == sky && c->torch == torch && "not a fixpoint");
				assert((y < FLOOR || c->sky == 15) && "flood did not spread");
			}
	printf("FIX3c: 2 slots / 2 loaded columns: drained in %zu calls, "
		   "no NO_SLOT stall, unloaded columns untouched\n", calls);
}

static void remote_relight_saturation_is_lossless(void) {
	const int S[3] = {24, 100, 24};
	big_reset();
	size_t ref_pops = reference_flood(S[0], S[1], S[2]);
	memcpy(bigref, bigw, sizeof(bigref));
	assert(ref_pops > 500000u && "fixture must exceed the legacy 500k abandon");
	assert(count_wrong(bigref, true) == 0 && "uncapped reference != Beta form");

	// Cause before LIGHT-CLIENT-LOSSLESS: the paced flood abandoned its frontier
	// past 500k pops. Now it parks with the frontier intact (counted as a client
	// park, never as a server ceiling hit); a caller that keeps resuming finishes
	// the SAME flood to the uncapped reference.
	big_reset();
	uint32_t hits0 = lighting_flood_ceiling_hits(), drops0 = stack_oom_drops(),
			 parks0 = lighting_client_ceiling_parks();
	struct stack q;
	lighting_flood_queue_create(&q);
	lighting_flood_seed(&q, S[0], S[1], S[2], true);
	size_t legacy = 0;
	while(!lighting_flood_queue_empty(&q))
		legacy += lighting_flood_resume(&q, false, LIGHTING_CLIENT_POPS_PER_FRAME,
										big_get, big_set, NULL);
	lighting_flood_queue_destroy(&q);
	printf("FIX3c: uncapped reference %zu pops; legacy resume %zu pops, "
		   "ceiling hits +%u (500000-pop abandon), stack drops +%u, "
		   "%zu cells wrong\n", ref_pops, legacy,
		   lighting_flood_ceiling_hits() - hits0, stack_oom_drops() - drops0,
		   count_wrong(bigw, false));
	fflush(stdout);
	assert(lighting_client_ceiling_parks() > parks0 && "fixture never parked");
	assert(lighting_flood_ceiling_hits() == hits0 && "client park counted as server");
	assert(stack_oom_drops() == drops0 && "a client push was dropped");
	assert(count_wrong(bigw, false) == 0 && "parked flood != uncapped reference");

	// The primitive: a failed allocation is an explicit init failure.
	size_t bytes = lighting_remote_relight_bytes(9);
	assert(!lighting_remote_relight_init(NULL, bytes, 9));
	assert(lighting_remote_relight_admit(NULL, 0, 0, 0, big_loaded, NULL) == LIGHTING_REMOTE_UNINIT);
	assert(!lighting_remote_relight_init((uint64_t[1]) {0}, 8, 9)); // short: refused
	// ONE reservation made here; a guard tail proves nothing writes past it,
	// and the primitive takes no allocator, so nothing allocates after init.
	unsigned char* mem = malloc(bytes + 64);
	assert(bytes && mem);
	memset(mem + bytes, 0xA5, 64);
	struct lighting_remote_relight* one = lighting_remote_relight_init(mem, bytes, 1);
	assert(one == (void*)mem && "1 slot: a column-edge seed needs 2 columns");
	assert(lighting_remote_relight_admit(one, 15, 100, 8, big_loaded, NULL) == LIGHTING_REMOTE_NO_SLOT);
	assert(lighting_remote_relight_admit(one, 8, 100, 8, big_loaded, NULL) == LIGHTING_REMOTE_PENDING);
	lighting_remote_relight_reset(one);
	size_t idle_spent = 1;
	assert(lighting_remote_relight_drain(one, false, 5, &idle_spent, big_get,
		NULL, big_set, big_loaded, NULL) == LIGHTING_REMOTE_IDLE && idle_spent == 0);
	for(int b = 0; b < 4; b++) {
		struct lighting_remote_relight* r = lighting_remote_relight_init(mem, bytes, 9);
		assert(r == (void*)mem && "init failed on a full-size reservation");
		big_reset();
		assert(lighting_remote_relight_admit(r, S[0], S[1], S[2], big_loaded, NULL) == LIGHTING_REMOTE_PENDING);
		size_t total = 0, spent, calls = 0,
			   RB[] = {1, 7, LIGHTING_CLIENT_POPS_PER_FRAME, 100000};
		enum lighting_remote_status st;
		while((st = lighting_remote_relight_drain(r, false, RB[b], &spent,
				   big_get, NULL, big_set, big_loaded, NULL)) == LIGHTING_REMOTE_PENDING) {
			assert(spent > 0 && spent <= RB[b] && "no progress / over budget");
			total += spent;
			assert(++calls < 50000000u && "drain never finishes");
		}
		assert(st == LIGHTING_REMOTE_IDLE && spent <= RB[b]);
		total += spent;
		assert(total > 500000u && "saturation fixture under the old ceiling");
		assert(count_wrong(bigw, false) == 0 && "remote != uncapped reference");
		assert(count_wrong(bigw, true) == 0 && "remote != Beta closed form");
		for(int g = 0; g < 64; g++)
			assert(mem[bytes + g] == 0xA5 && "wrote past the reservation");
		printf("FIX3c: budget %zu: %zu units over %zu calls, lossless\n", RB[b],
			   total, calls);
	}
	remote_relight_two_slots_never_stall(mem, bytes);
	free(mem);
	printf("test_lighting_client_budget_equiv: remote relight saturation "
		   "lossless (== uncapped reference == Beta form)\n");
}

// Budgets spanning maximal fragmentation (1 pop/slice) to effectively one-shot.
static const size_t BUDGETS[]
	= {1, 2, 7, 64, 999, LIGHTING_CLIENT_POPS_PER_FRAME, 100000};
#define NBUD ((int)(sizeof(BUDGETS) / sizeof(BUDGETS[0])))

int main(void) {
	setup_blocks();
	assert(CHUNK_SIZE == LW);
	g_allsrc = malloc((size_t)LW * LW * H * sizeof(*g_allsrc));
	assert(g_allsrc);

	// ===== Structured fixture: a deep open-sky column clear = a LARGE skylight flood
	// (the surface scenario that froze the Wii) =================================
	memset(gworld, 0, sizeof(gworld));
	for(int x = 0; x < LW; x++)
		for(int z = 0; z < LW; z++)
			for(int y = 0; y <= 80; y++)
				gworld[gidx(x, y, z)].type = BLOCK_STONE; // solid to y=80, open sky above
	gworld[gidx(8, 40, 8)].type = BLOCK_TORCH;            // a buried torch (block light)
	compute_height_all();
	relight_all();
	memcpy(W0, gworld, sizeof(gworld));
	memcpy(W0h, gheight, sizeof(gheight));
	assert(gworld[gidx(0, 81, 0)].sky == 15); // open sky above the surface

	// Carve a 4x4 shaft straight down from the surface to y=41: a big connected skylight
	// flood as sky pours down + spreads sideways through the opened volume.
	{
		struct edit e[4 * 4 * 40];
		int n = 0;
		for(int x = 6; x <= 9; x++)
			for(int z = 6; z <= 9; z++)
				for(int y = 41; y <= 80; y++)
					e[n++] = (struct edit) {x, y, z, BLOCK_AIR, 0};
		for(int b = 0; b < NBUD; b++)
			run_case(e, n, BUDGETS[b], "skylight-shaft");

		// Non-vacuous: the shaft actually flooded sky down to the bottom (so the budget
		// had a genuinely large flood to fragment, not a no-op).
		memcpy(gworld, W0, sizeof(gworld)); // (snapRef already holds the ref result)
		assert(snapRef[gidx(7, 41, 7)].sky == 15);
	}

	printf("test_lighting_client_budget_equiv: skylight shaft byte-identical across "
		   "%d budgets (1..100000 pops/slice)\n", NBUD);

	// ===== Eviction MID-FLOOD: the regression that crashed the Wii ==============
	// A paced flood's frontier can reference cells whose chunk is UNLOADED between resume
	// slices (death->respawn evicts the death area; walking evicts far chunks). The flood
	// must SKIP them (get_block false) and never relight a NULL chunk. Pre-guard, on the
	// NDEBUG .dol this deref'd NULL in world_light_set_light -> DSI -> watchdog freeze.
	{
		// Rebuild the lit shaft fixture.
		memset(g_evicted, 0, sizeof(g_evicted));
		memset(gworld, 0, sizeof(gworld));
		for(int x = 0; x < LW; x++)
			for(int z = 0; z < LW; z++)
				for(int y = 0; y <= 80; y++)
					gworld[gidx(x, y, z)].type = BLOCK_STONE;
		compute_height_all();
		relight_all();
		// Carve the full 16x16 surface down to y=41 -> a big skylight flood with a wide
		// frontier we can then evict a slab out from under.
		for(int x = 0; x < LW; x++)
			for(int z = 0; z < LW; z++)
				for(int y = 41; y <= 80; y++) {
					gworld[gidx(x, y, z)].type = BLOCK_AIR;
					lighting_heightmap_update(gheight, x, y, z, BLOCK_AIR, hm_get,
											  NULL);
				}
		struct stack q;
		lighting_flood_queue_create(&q);
		lighting_flood_seed(&q, 8, 80, 8, true);
		// Resume a few small slices so the BFS frontier spreads through the opened volume.
		for(int s = 0; s < 5 && !lighting_flood_queue_empty(&q); s++)
			lighting_flood_resume(&q, false, 64, g_get, g_set, NULL);
		assert(!lighting_flood_queue_empty(&q)
			   && "fixture too small -- flood drained before eviction");
		// EVICT a slab the frontier is sitting in / about to reach (the death area).
		for(int x = 0; x < LW; x++)
			for(int z = 0; z < LW; z++)
				for(int y = 41; y <= 70; y++)
					g_evicted[gidx(x, y, z)] = true;
		// Resume to completion: must terminate, must not crash, must not write an evicted
		// cell (g_set asserts that). guard catches any non-progress / non-termination.
		int guard = 0;
		while(!lighting_flood_queue_empty(&q)) {
			lighting_flood_resume(&q, false, 64, g_get, g_set, NULL);
			assert(++guard < 10000000 && "flood did not drain after eviction");
		}
		lighting_flood_queue_destroy(&q);
		printf("test_lighting_client_budget_equiv: eviction mid-flood handled "
			   "(no relight of unloaded cells, no NULL-chunk deref)\n");
	}

	// ===== Fuzz: random terrain + random edits, every budget ===================
	for(int seed = 1; seed <= 24; seed++) {
		rng_state = (uint32_t)seed * 2654435761u + 1u;
		memset(gworld, 0, sizeof(gworld));
		for(int x = 0; x < LW; x++)
			for(int z = 0; z < LW; z++) {
				int top = xr_range(40, 90);
				for(int y = 0; y <= top; y++) {
					int r = (int)(xr() % 16);
					uint8_t t = BLOCK_STONE;
					if(r == 0)
						t = BLOCK_LEAVES;
					else if(r == 1)
						t = BLOCK_AIR;
					else if(r == 2)
						t = BLOCK_LAVA_STILL;
					gworld[gidx(x, y, z)].type = t;
				}
				if((xr() % 4) == 0)
					gworld[gidx(x, top + 1 + (int)(xr() % 3), z)].type = BLOCK_TORCH;
			}
		compute_height_all();
		relight_all();
		memcpy(W0, gworld, sizeof(gworld));
		memcpy(W0h, gheight, sizeof(gheight));

		struct edit e[200];
		int n = xr_range(60, 160);
		for(int i = 0; i < n; i++) {
			int x = (int)(xr() % LW), z = (int)(xr() % LW), y = xr_range(1, H - 1);
			int pick = (int)(xr() % 10);
			uint8_t t = BLOCK_AIR;
			if(pick == 6)
				t = BLOCK_STONE;
			else if(pick == 7)
				t = BLOCK_LEAVES;
			else if(pick == 8)
				t = BLOCK_TORCH;
			else if(pick == 9)
				t = BLOCK_LAVA_STILL;
			e[i] = (struct edit) {x, y, z, t, 0};
		}
		// Two budgets per seed (max fragmentation + a realistic frame budget) keeps the
		// fuzz fast while still proving resume-boundary independence.
		run_case(e, n, 1, "fuzz");
		run_case(e, n, LIGHTING_CLIENT_POPS_PER_FRAME, "fuzz");
	}
	printf("test_lighting_client_budget_equiv: 24 fuzz seeds byte-identical "
		   "(budgeted/resumed flood == unbudgeted flood)\n");
	remote_relight_saturation_is_lossless();

	free(g_allsrc);
	return 0;
}

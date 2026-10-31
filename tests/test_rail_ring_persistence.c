/*
	Batch-0 save-integrity fixture for the 2026-08-07 hardware-video caveat
	"the circular rail track disappeared" (docs/audit + .handoff 2026-08-07).

	Card forensics (save_forensics/2026-08-07) found ZERO structural damage in
	any on-card region file -- 12 .mcr files, ~3,000 chunk entries: no sector
	overlaps, no range violations, no zlib/NBT failures, no xPos/zPos aliasing
	-- and zero rails in the worlds that reportedly lost the ring. The live
	suspicion is therefore HISTORICAL loss (the 2026-07-30 torn-region salvage
	regenerated unsalvageable chunks by design, erasing player edits in them),
	not a current-build archive defect. This fixture is the "reproduce first"
	gate the handoff demands: it proves (or catches) the CURRENT pipeline
	round-tripping a player-built rail ring byte-exactly through every
	persistence transition a real session performs.

	The ring deliberately mirrors the real saves' geometry risks:
	  - spans the x=0 REGION boundary (chunks -1,* in r.-1.0 / 0,* in r.0.0 --
	    negative region coords, where sign/shift bugs live),
	  - spans a z CHUNK boundary (z=16) inside both regions,
	  - mixes plain rail 66 (straight metas 0/1, curve metas 6..9, slope metas
	    2..5), powered rail 27 WITH the persisted power bit (meta 8|dir, the
	    exact 9/11 pattern the real New World 120 descent line carries), and
	    detector rail 28.

	Transitions exercised, all through PRODUCTION paths (server_world_set_block,
	server_world_save_chunk(erase=true) -- the evictor core --,
	server_world_sync_regions -- the durability barrier --, server_world_destroy
	-- session close --, then a FRESH server_world -- cold restart):
	  place -> evict+reload -> edit -> cold restart -> verify -> evict+reload.

	The superflat generator stays armed in every session on purpose (the
	test_region_boundary precedent): a persistence regression must FABRICATE
	wrong bytes that the table compare catches, not merely fail to load.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_rail_ring_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define PLAT_Y 60
#define RAIL_Y 61

/* Ring rectangle: x in [-8,7], z in [10,21]. Chunk x -1|0 (region -1|0),
 * chunk z 0|1 -- four chunks, two region files, both boundaries crossed. */
#define RING_X0 (-8)
#define RING_X1 7
#define RING_Z0 10
#define RING_Z1 21

static struct server_local s;
static volatile int g_drain_run = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

struct cell {
	int x, y, z;
	uint8_t id, meta;
};

/* The full expected-state table: every rail cell (and the support/torch cells
 * that must ride along). Compared byte-exactly after every transition. */
static struct cell table[256];
static size_t table_len;

static void expect(int x, int y, int z, uint8_t id, uint8_t meta) {
	assert(table_len < sizeof(table) / sizeof(*table));
	table[table_len++] = (struct cell) {x, y, z, id, meta};
}

static void set(int x, int y, int z, uint8_t type, uint8_t meta) {
	bool changed = server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = (uint8_t)(meta & 0x0F)});
	if(!changed) {
		struct block_data live;
		assert(server_world_get_block(&s.worlds[0], x, y, z, &live));
		assert(live.type == type && (live.metadata & 0x0F) == (meta & 0x0F));
	}
}

/* Place a cell AND record it in the expected table. */
static void put(int x, int y, int z, uint8_t id, uint8_t meta) {
	set(x, y, z, id, meta);
	expect(x, y, z, id, meta);
}

/* Re-read every table cell from the LIVE world, overwriting the expectation
 * with the settled state. Rail/redstone placement logic legitimately rewrites
 * metadata as neighbours land (power re-evaluation, track re-shaping) -- the
 * persistence contract is "whatever the live world settled to round-trips
 * byte-exactly", so the snapshot IS the contract. Coverage asserts below keep
 * the snapshot honest (it must actually contain the byte patterns the real
 * saves carry). */
static void snapshot_live(void) {
	for(size_t i = 0; i < table_len; i++) {
		struct block_data b;
		bool ok = server_world_get_block(&s.worlds[0], table[i].x, table[i].y,
										 table[i].z, &b);
		assert(ok && "snapshot cell must be resident");
		table[i].id = b.type;
		table[i].meta = (uint8_t)(b.metadata & 0x0F);
	}
}

static void assert_snapshot_coverage(void) {
	bool powered_bit = false, curve = false, slope = false, detector = false,
		 plain_straight = false;
	for(size_t i = 0; i < table_len; i++) {
		if(table[i].id == BLOCK_POWERED_RAIL && (table[i].meta & 0x8))
			powered_bit = true;
		if(table[i].id == BLOCK_RAIL && table[i].meta >= 6
		   && table[i].meta <= 9)
			curve = true;
		if((table[i].id == BLOCK_RAIL || table[i].id == BLOCK_POWERED_RAIL)
		   && (table[i].meta & 0x7) >= 2 && (table[i].meta & 0x7) <= 5)
			slope = true;
		if(table[i].id == BLOCK_DETECTOR_RAIL)
			detector = true;
		if(table[i].id == BLOCK_RAIL && table[i].meta <= 1)
			plain_straight = true;
	}
	assert(powered_bit
		   && "snapshot must persist at least one powered rail WITH bit 8 -- "
			  "the real descent line's meta 9/11 pattern");
	assert(curve && "snapshot must contain a curve meta (6..9)");
	assert(slope && "snapshot must contain a slope meta (2..5)");
	assert(detector && "snapshot must contain a detector rail (28)");
	assert(plain_straight && "snapshot must contain a plain straight rail");
}

static const struct {
	int cx, cz;
} ring_chunks[] = {{-1, 0}, {0, 0}, {-1, 1}, {0, 1}};
#define RING_CHUNKS (sizeof(ring_chunks) / sizeof(*ring_chunks))

static void load_ring_chunks(void) {
	for(size_t i = 0; i < RING_CHUNKS; i++) {
		struct server_chunk* sc = NULL;
		bool ok = server_world_load_chunk(&s.worlds[0], ring_chunks[i].cx,
										  ring_chunks[i].cz, &sc);
		assert(ok && sc);
	}
}

static void verify_table(const char* phase) {
	for(size_t i = 0; i < table_len; i++) {
		struct block_data b;
		bool ok = server_world_get_block(&s.worlds[0], table[i].x, table[i].y,
										 table[i].z, &b);
		if(!ok || b.type != table[i].id
		   || (b.metadata & 0x0F) != table[i].meta) {
			fprintf(stderr,
					"%s: cell (%d,%d,%d) expected id=%u meta=%u got id=%u "
					"meta=%u (ok=%d)\n",
					phase, table[i].x, table[i].y, table[i].z, table[i].id,
					table[i].meta, ok ? b.type : 0,
					ok ? (b.metadata & 0x0F) : 0, (int)ok);
			assert(0 && "rail ring persistence mismatch");
		}
	}
	printf("  %s: %zu cells byte-exact\n", phase, table_len);
}

/* Evict every ring chunk through the real evictor core, force the durability
 * barrier, then reload through the real load path. */
static void evict_sync_reload(const char* phase) {
	server_world_light_drain(&s.worlds[0]); /* G9: unloads wait for the drain */
	for(size_t i = 0; i < RING_CHUNKS; i++) {
		test_drain_quiesce(); /* quiesce-0926: free inbox token for the batch */
		bool ok = server_world_save_chunk(&s.worlds[0], true /* erase = evict */,
										  ring_chunks[i].cx, ring_chunks[i].cz);
		assert(ok && "evict-save must succeed");
	}
	assert(server_world_sync_regions(&s.worlds[0])
		   && "region durability barrier must succeed");
	load_ring_chunks();
	verify_table(phase);
}

static void build_ring(void) {
	/* Stone platform under every future rail cell (superflat ground is y 0..3;
	 * the ring floats on its own deck exactly like a player build). */
	for(int x = RING_X0; x <= RING_X1; x++) {
		set(x, PLAT_Y, RING_Z0, BLOCK_STONE, 0);
		set(x, PLAT_Y, RING_Z1, BLOCK_STONE, 0);
	}
	for(int z = RING_Z0; z <= RING_Z1; z++) {
		set(RING_X0, PLAT_Y, z, BLOCK_STONE, 0);
		set(RING_X1, PLAT_Y, z, BLOCK_STONE, 0);
	}

	/* Corners: the four Beta curve metas (6=SE 7=SW 8=NW 9=NE as persisted
	 * bytes -- persistence must not care which corner carries which). */
	put(RING_X0, RAIL_Y, RING_Z0, BLOCK_RAIL, 6);
	put(RING_X1, RAIL_Y, RING_Z0, BLOCK_RAIL, 7);
	put(RING_X1, RAIL_Y, RING_Z1, BLOCK_RAIL, 8);
	put(RING_X0, RAIL_Y, RING_Z1, BLOCK_RAIL, 9);

	/* North straight (z=RING_Z0): plain rail, EW meta 1. */
	for(int x = RING_X0 + 1; x < RING_X1; x++)
		put(x, RAIL_Y, RING_Z0, BLOCK_RAIL, 1);

	/* South straight (z=RING_Z1): POWERED rail. The torch lands FIRST so the
	 * rails' neighbour-change power re-evaluation settles bit 8 as they land
	 * (the exact meta 8|dir = 9/11 byte pattern the real descent line
	 * persists); snapshot_live() then records whatever the power cascade
	 * settled, and assert_snapshot_coverage() requires bit 8 to be present. */
	set(0, RAIL_Y, RING_Z1 + 1, BLOCK_REDSTONE_TORCH_LIT, 5);
	expect(0, RAIL_Y, RING_Z1 + 1, BLOCK_REDSTONE_TORCH_LIT, 5);
	for(int x = RING_X0 + 1; x < RING_X1; x++)
		put(x, RAIL_Y, RING_Z1, BLOCK_POWERED_RAIL, 9);

	/* East straight (x=RING_X1): plain rail NS meta 0.
	 * West straight (x=RING_X0): alternating detector / plain, NS meta 0 --
	 * detector 28 must round-trip exactly like 66. */
	for(int z = RING_Z0 + 1; z < RING_Z1; z++) {
		put(RING_X1, RAIL_Y, z, BLOCK_RAIL, 0);
		put(RING_X0, RAIL_Y, z, (z & 1) ? BLOCK_DETECTOR_RAIL : BLOCK_RAIL, 0);
	}

	/* Sloped spur off the NE corner, ascending east across the chunk-0 body:
	 * plain slope metas 2/3 and a powered slope 8|2=10 (the real 11 pattern is
	 * 8|3 -- both nibble halves get coverage between ring and spur). */
	set(RING_X1 + 1, PLAT_Y, RING_Z0, BLOCK_STONE, 0);
	set(RING_X1 + 1, PLAT_Y + 1, RING_Z0, BLOCK_STONE, 0);
	set(RING_X1 + 2, PLAT_Y, RING_Z0, BLOCK_STONE, 0);
	set(RING_X1 + 2, PLAT_Y + 1, RING_Z0, BLOCK_STONE, 0);
	set(RING_X1 + 2, PLAT_Y + 2, RING_Z0, BLOCK_STONE, 0);
	put(RING_X1 + 1, RAIL_Y + 1, RING_Z0, BLOCK_RAIL, 2);
	put(RING_X1 + 2, RAIL_Y + 2, RING_Z0, BLOCK_POWERED_RAIL, 10);
}

int main(void) {
	test_fixture_dir("test_rail_ring_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	/* Repeatable clean slate: both region files the ring spans. */
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);

	/* ---- session 1: build, evict+reload, edit ---------------------------- */
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat armed: regressions fabricate */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x9E3779B9u;
	dict_entity_init(s.entities);

	load_ring_chunks();
	build_ring();
	/* Rail/redstone logic legitimately rewrites metas during placement; the
	 * settled live state becomes the expectation, and coverage asserts prove
	 * the snapshot actually carries the byte patterns under test. */
	snapshot_live();
	assert_snapshot_coverage();
	printf("  session1 as-built: %zu cells snapshotted (coverage OK)\n",
		   table_len);
	evict_sync_reload("session1 evict+reload");

	/* Edit AFTER an evict cycle: the reloaded (disk-round-tripped) chunks take
	 * new writes, exactly like a player editing after re-entering the area.
	 * Edits fire neighbour updates that may re-shape adjacent rails, so the
	 * post-edit live state is re-snapshotted as the new expectation. */
	set(RING_X0, RAIL_Y, RING_Z0 + 3, BLOCK_AIR, 0); /* mine one west rail */
	set(RING_X1, RAIL_Y, RING_Z0 + 2, BLOCK_RAIL, 5); /* re-meta an east rail */
	set(RING_X0 - 1, PLAT_Y, RING_Z1 + 1, BLOCK_STONE, 0); /* extend SW */
	set(RING_X0 - 1, RAIL_Y, RING_Z1 + 1, BLOCK_RAIL, 1);
	expect(RING_X0 - 1, RAIL_Y, RING_Z1 + 1, BLOCK_RAIL, 1);
	snapshot_live();
	{
		/* The mined cell must genuinely be air in the new expectation. */
		struct block_data b;
		assert(server_world_get_block(&s.worlds[0], RING_X0, RAIL_Y, RING_Z0 + 3,
									  &b)
			   && b.type == BLOCK_AIR);
	}
	printf("  session1 after-edit: re-snapshotted %zu cells\n", table_len);

	server_world_destroy(&s.worlds[0]); /* session close: save + sync everything */
	dict_entity_clear(s.entities);

	/* ---- session 2: cold restart over the same directory ----------------- */
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat still armed */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x9E3779B9u;
	dict_entity_init(s.entities);

	load_ring_chunks();
	verify_table("session2 cold-restart");
	/* A second full evict cycle in the restarted session: the region files now
	 * contain REWRITTEN chunks (the d6d4804 gap-reuse/append-floor territory);
	 * re-evicting over them must still round-trip. */
	evict_sync_reload("session2 re-evict");

	server_world_destroy(&s.worlds[0]);
	dict_entity_clear(s.entities);
	string_clear(level);

	g_drain_run = 0;
	thread_join(&drain);

	printf("test_rail_ring_persistence: OK (place/evict/reload/edit/"
		   "cold-restart/re-evict, %zu cells, region boundary spanned)\n",
		   table_len);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}

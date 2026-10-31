/* IMP-TE-CAPS (2026-10-21): a synthetic dense world loads fully through the
 * LIVE server chunk path (region save -> region load -> te_store preflight/
 * insert), with no tile-entity cap anywhere.
 *
 * Shape of the real failure (tmp/imported-worlds-1021/REPORT.md bug 2): the
 * Dream Journal world holds 10,909 note blocks in one Wii r=5 window, chunks
 * with up to 384 note blocks and ~300 furnaces around spawn. Before the fix a
 * chunk with more than 64 Music/Furnace/Trap/RecordPlayer records was refused
 * whole and the world-wide 64-furnace / 256-sign tables made every further
 * chunk retry forever.
 *
 * Here: 25 chunks (r=2) carry 12,000 note blocks (480 per chunk), 300 signs
 * and 300 furnaces (12 + 12 per chunk). Every chunk is saved and evicted (the
 * tables must empty), then reloaded: every chunk must load on its first
 * attempt, every tile entity must come back with its exact position and
 * payload, and a second gather must equal the first (round trip identical). */
#include "support/split_residency_fixture.h"

#include "network/server_furnace.h"
#include "network/server_noteblock.h"
#include "network/tile_entities_store.h"

#include <stdio.h>
#include <time.h>

#define R 2
#define NOTES_PER_CHUNK 480
#define SIGNS_PER_CHUNK 12
#define FURNACES_PER_CHUNK 12
#define CHUNKS ((2 * R + 1) * (2 * R + 1))

static struct server_chunk* load_once(struct server_world* w, int cx, int cz) {
	struct server_chunk* c = NULL;
	server_world_begin_update(w);
	(void)server_world_load_chunk(w, cx, cz, &c);
	return c;
}

static void put(struct server_world* w, int x, int y, int z, uint8_t type) {
	bool ok = server_world_set_block_silent(w, x, y, z,
											(struct block_data) {.type = type});
	if(!ok)
		fprintf(stderr, "set_block_silent refused at (%d,%d,%d) halted=%d\n",
				x, y, z, (int)server_world_light_halted(w));
	assert(ok);
}

static struct chunk_tile_entities before[CHUNKS];

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	struct server_local* s = split_fixture();
	struct server_world* w = &s->worlds[0];
	int idx = 0;
	/* ---- build the dense world in memory ---- */
	for(int cx = -R; cx <= R; cx++)
		for(int cz = -R; cz <= R; cz++, idx++) {
			struct server_chunk* c = NULL;
			for(int n = 0; n < 200 && !c; n++)
				c = load_once(w, cx, cz);
			assert(c);
			for(int i = 0; i < NOTES_PER_CHUNK; i++) {
				int x = cx * 16 + i % 16, z = cz * 16 + (i / 16) % 16;
				int y = 1 + i / 256; /* replaces opaque superflat ground: no relight */
				/* No host-block write: the TE load path gates only Piston
				 * records on their host block (Beta setChunkBlockTileEntity);
				 * thousands of quiet writes would also exhaust the test's
				 * light reservation budget. */
				assert(noteblock_te_register_w(w, x, y, z) >= 0);
				*noteblock_te_lookup_w(w, x, y, z) = (int8_t)((i + idx) % 25);
			}
			struct te_store st = server_world_tile_store(w);
			for(int i = 0; i < SIGNS_PER_CHUNK; i++) {
				int x = cx * 16 + i, z = cz * 16 + 3;
				int row = te_store_claim(&st, TE_KIND_SIGN, x, 4, z, NULL);
				assert(row >= 0);
				memset(s->sign_texts[row], ' ', SIGN_SIZE);
				char line[16];
				int n = snprintf(line, sizeof(line), "c%d s%d", idx, i);
				memcpy(s->sign_texts[row], line, (size_t)n);
			}
			for(int i = 0; i < FURNACES_PER_CHUNK; i++) {
				int x = cx * 16 + i, z = cz * 16 + 5;
				put(w, x, 3, z, BLOCK_FURNACE); /* opaque -> opaque */
				assert(furnace_te_register_w(w, x, 3, z) >= 0);
				struct furnace_te* f = furnace_te_lookup_w(w, x, 3, z);
				f->items[0] = (struct item_data) {.id = BLOCK_COBBLESTONE,
					.count = (uint8_t)(1 + (idx + i) % 64)};
			}
			c->modified = true;
		}
	struct te_store st = server_world_tile_store(w);
	assert(te_store_count_owned(&st, TE_KIND_NOTE) == CHUNKS * NOTES_PER_CHUNK);
	assert(te_store_count_owned(&st, TE_KIND_SIGN) == CHUNKS * SIGNS_PER_CHUNK);
	assert(te_store_count_owned(&st, TE_KIND_FURNACE)
		   == CHUNKS * FURNACES_PER_CHUNK);
	printf("built: %d notes, %d signs, %d furnaces in %d chunks; tables "
		   "note=%d sign=%d furnace=%d rows, %zu B live\n",
		   CHUNKS * NOTES_PER_CHUNK, CHUNKS * SIGNS_PER_CHUNK,
		   CHUNKS * FURNACES_PER_CHUNK, CHUNKS, s->te_rows[TE_KIND_NOTE],
		   s->te_rows[TE_KIND_SIGN], s->te_rows[TE_KIND_FURNACE],
		   s->te_grow.bytes_live);

	/* ---- gather (the save image), save + evict every chunk ---- */
	idx = 0;
	for(int cx = -R; cx <= R; cx++)
		for(int cz = -R; cz <= R; cz++, idx++)
			assert(te_store_gather(&st, cx, cz, &before[idx])
				   && before[idx].n_note == NOTES_PER_CHUNK
				   && before[idx].n_sign == SIGNS_PER_CHUNK
				   && before[idx].n_furnace == FURNACES_PER_CHUNK);
	server_world_save_batch_begin(w);
	for(int cx = -R; cx <= R; cx++)
		for(int cz = -R; cz <= R; cz++)
			server_world_save_chunk(w, true, cx, cz);
	assert(server_world_save_batch_end(w));
	st = server_world_tile_store(w);
	assert(te_store_count_owned(&st, TE_KIND_NOTE) == 0
		   && te_store_count_owned(&st, TE_KIND_SIGN) == 0
		   && te_store_count_owned(&st, TE_KIND_FURNACE) == 0);

	/* ---- reload: each chunk must load on its FIRST attempt ---- */
	clock_t t0 = clock();
	for(int cx = -R; cx <= R; cx++)
		for(int cz = -R; cz <= R; cz++) {
			struct server_chunk* c = load_once(w, cx, cz);
			if(!c)
				fprintf(stderr, "chunk (%d,%d) refused on reload\n", cx, cz);
			assert(c && "a dense chunk loads on its first attempt");
		}
	double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
	st = server_world_tile_store(w);
	assert(te_store_count_owned(&st, TE_KIND_NOTE) == CHUNKS * NOTES_PER_CHUNK);
	assert(te_store_count_owned(&st, TE_KIND_SIGN) == CHUNKS * SIGNS_PER_CHUNK);
	assert(te_store_count_owned(&st, TE_KIND_FURNACE)
		   == CHUNKS * FURNACES_PER_CHUNK);

	/* ---- round trip: the reloaded image equals the saved one ---- */
	idx = 0;
	for(int cx = -R; cx <= R; cx++)
		for(int cz = -R; cz <= R; cz++, idx++) {
			struct chunk_tile_entities after;
			assert(te_store_gather(&st, cx, cz, &after));
			const struct chunk_tile_entities* b = &before[idx];
			assert(after.n_note == b->n_note && after.n_sign == b->n_sign
				   && after.n_furnace == b->n_furnace);
			for(int i = 0; i < b->n_note; i++)
				assert(memcmp(&after.notes[i], &b->notes[i],
							  sizeof(b->notes[i])) == 0);
			for(int i = 0; i < b->n_sign; i++)
				assert(after.signs[i].x == b->signs[i].x
					   && after.signs[i].y == b->signs[i].y
					   && after.signs[i].z == b->signs[i].z
					   && strcmp(after.signs[i].text, b->signs[i].text) == 0);
			for(int i = 0; i < b->n_furnace; i++)
				assert(after.furnaces[i].x == b->furnaces[i].x
					   && after.furnaces[i].z == b->furnaces[i].z
					   && after.furnaces[i].burn_time == b->furnaces[i].burn_time
					   && after.furnaces[i].cook_time == b->furnaces[i].cook_time
					   && memcmp(after.furnaces[i].items, b->furnaces[i].items,
								 sizeof(b->furnaces[i].items)) == 0);
			tile_entities_parse_free(&after);
			tile_entities_parse_free(&before[idx]);
		}
	assert(s->te_grow.grow_events > 0 && s->te_grow.grow_failures == 0
		   && "the dense world really grew the tables");
	printf("test_import_te_dense: OK -- %d chunks reloaded first try in %.0f ms; "
		   "%d notes / %d signs / %d furnaces round-trip identical; peak %zu B, "
		   "%u table growths, %u failures\n",
		   CHUNKS, ms, CHUNKS * NOTES_PER_CHUNK, CHUNKS * SIGNS_PER_CHUNK,
		   CHUNKS * FURNACES_PER_CHUNK, s->te_grow.bytes_peak,
		   (unsigned)s->te_grow.grow_events, (unsigned)s->te_grow.grow_failures);
	split_destroy(s);
	return 0;
}

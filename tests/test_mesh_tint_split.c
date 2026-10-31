/*
	Proves R1 -- the per-list biome-tint split in the chunk mesher -- preserves
	the emitted geometry exactly.

	BEFORE R1: every face emitted into one of CHUNK_MESH_PARTS (13) GX_VTXFMT4
	lists, 12 bytes/vertex; untinted faces carried a white (255,255,255) tint.

	AFTER R1: a face emits into either the 9-byte untinted list `dp` (GX_VTXFMT0,
	no tint channel) or the 12-byte tinted list `dp + CHUNK_MESH_PARTS`
	(GX_VTXFMT4), chosen by the SAME `tinted` flag the mesher already computed
	(tint != white). chunk_render draws each list under the matching gfx_tint()
	state; white == identity in the GX TEV (gfx_tint off omits the CLR1 multiply
	stage) and in the GL shader (constant-white a_color), so the rendered pixels
	are unchanged.

	This test mirrors source/platform/wii/displaylist.c (the 9/12-byte vertex
	writer) and source/chunk_mesher.c (the `emit_index = tinted ? dp + PARTS : dp`
	routing) and asserts, replaying a varied face set face-by-face:

	  1. ROUTING    -- a face lands in a tinted list iff it is tinted.
	  2. CONSERVE   -- the split loses no vertex (Sum new == Sum old == faces*4).
	  3. WHITE-STRIP-- an untinted face's 9-byte vertices equal the old 12-byte
	                   vertices with the (white) tint bytes removed: byte-identical
	                   pos/light/texcoord, only the identity tint dropped.
	  4. TINT-KEEP  -- a tinted face's 12-byte vertices are byte-identical to the
	                   old scheme (its exact biome tint preserved).

	If you change wii/displaylist.c's layout or the mesher routing, change this
	(same convention as test_displaylist_vertex_equiv.c).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_U8(b, i) (*((uint8_t*)(b) + (i)))
#define MEM_U16(b, i) (*(uint16_t*)((uint8_t*)(b) + (i)))

#define CHUNK_MESH_PARTS 13
#define CHUNK_MESH_LISTS (CHUNK_MESH_PARTS * 2)

/* ---- Wii displaylist backend (mirrors source/platform/wii/displaylist.c) -- */
#define CLL 32
#define CACHE_LINES(v, s) (((v) * (s) + 3 + CLL - 1) / CLL * CLL)

struct tdl {
	void* data;
	size_t length;
	size_t index;
	bool tinted;
	uint8_t tint[3];
};

static void w_init(struct tdl* l, bool tinted) {
	l->length = CACHE_LINES(64, tinted ? 12 : 9);
	l->data = NULL;
	l->index = CLL + 3;
	l->tinted = tinted;
	l->tint[0] = l->tint[1] = l->tint[2] = 255;
}
static void w_ensure(struct tdl* l, int vtx_bytes) {
	if(!l->data) {
		l->data = malloc(l->length + CLL);
		assert(l->data);
	}
	if(l->index + vtx_bytes > l->length) {
		l->length = (l->length * 3 / 2 + vtx_bytes + CLL - 1) / CLL * CLL;
		l->data = realloc(l->data, l->length + CLL);
		assert(l->data);
	}
}
static void w_set_tint(struct tdl* l, uint8_t r, uint8_t g, uint8_t b) {
	l->tint[0] = r;
	l->tint[1] = g;
	l->tint[2] = b;
}
static void w_vertex(struct tdl* l, int16_t x, int16_t y, int16_t z,
					 uint8_t light, uint8_t s, uint8_t t) {
	w_ensure(l, l->tinted ? 12 : 9);
	uint8_t* p = (uint8_t*)l->data + l->index;
	MEM_U16(p, 0) = x;
	MEM_U16(p, 2) = y;
	MEM_U16(p, 4) = z;
	MEM_U8(p, 6) = light;
	if(l->tinted) {
		MEM_U8(p, 7) = l->tint[0];
		MEM_U8(p, 8) = l->tint[1];
		MEM_U8(p, 9) = l->tint[2];
		MEM_U8(p, 10) = s;
		MEM_U8(p, 11) = t;
		l->index += 12;
	} else {
		MEM_U8(p, 7) = s;
		MEM_U8(p, 8) = t;
		l->index += 9;
	}
}
static size_t w_vertex_count(const struct tdl* l) {
	if(!l->data)
		return 0;
	return (l->index - (CLL + 3)) / (l->tinted ? 12 : 9);
}

/* ---- the mesher routing under test (mirrors chunk_mesher.c) --------------- */
static int emit_index(int dp, bool tinted) {
	return tinted ? dp + CHUNK_MESH_PARTS : dp;
}

/* ---- a varied face set ---------------------------------------------------- */
struct vtx {
	int16_t x, y, z;
	uint8_t light, s, t;
};
// 0xFFFFFF = untinted (white); anything else = a biome tint.
struct face {
	int dp;
	uint32_t tint;
	struct vtx v[4];
};

#define WHITE 0xFFFFFFu
#define GRASS 0x6F9F4Eu	  // an arbitrary non-white grass tint
#define FOLIAGE 0x4E8F3Au // an arbitrary non-white foliage tint

// Spread across draw buckets (opaque sides 0..5, transparent 6..11, double 12),
// with extreme S16 positions + full 0..255 light/texcoord ranges, and a mix of
// white / tinted faces -- including the same bucket receiving both (grass top
// tinted + stone top untinted both land in bucket 1, the classic mesher case).
static const struct face FACES[] = {
	{0, WHITE, {{0, 0, 0, 0, 0, 0}, {1, 2, 3, 4, 5, 6}, {7, 8, 9, 10, 11, 12}, {13, 14, 15, 16, 17, 18}}},
	{1, WHITE, {{32767, -32768, 1, 255, 255, 255}, {-1, 1, -32768, 15, 16, 240}, {256, 4096, -256, 200, 1, 254}, {12345, -6789, 4242, 7, 128, 64}}},
	{1, GRASS, {{100, 200, 300, 33, 99, 17}, {-12, 34, -56, 78, 90, 123}, {5, 6, 7, 8, 9, 10}, {11, 12, 13, 14, 15, 16}}},
	{3, WHITE, {{-32768, 32767, -32768, 240, 200, 8}, {1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2}, {3, 3, 3, 3, 3, 3}}},
	{5, FOLIAGE, {{9, 8, 7, 6, 5, 4}, {3, 2, 1, 0, 255, 254}, {100, 101, 102, 103, 104, 105}, {200, 201, 202, 203, 204, 205}}},
	{6, WHITE, {{-100, -200, -300, 50, 60, 70}, {-1, -2, -3, 80, 90, 100}, {0, 0, 0, 0, 0, 0}, {32767, 32767, 32767, 255, 255, 255}}},
	{12, GRASS, {{4, 4, 4, 4, 4, 4}, {5, 5, 5, 5, 5, 5}, {6, 6, 6, 6, 6, 6}, {7, 7, 7, 7, 7, 7}}},
	{12, WHITE, {{8, 9, 10, 11, 12, 13}, {14, 15, 16, 17, 18, 19}, {20, 21, 22, 23, 24, 25}, {26, 27, 28, 29, 30, 31}}},
	{1, FOLIAGE, {{-5, -6, -7, 200, 100, 50}, {-8, -9, -10, 11, 12, 13}, {-11, -12, -13, 14, 15, 16}, {-14, -15, -16, 17, 18, 19}}},
};
#define NFACES ((int)(sizeof(FACES) / sizeof(FACES[0])))

static void emit_face(struct tdl* lists, int idx, const struct face* f) {
	bool tinted = f->tint != WHITE;
	if(tinted)
		w_set_tint(&lists[idx], (f->tint >> 16) & 0xFF, (f->tint >> 8) & 0xFF,
				   f->tint & 0xFF);
	for(int k = 0; k < 4; k++)
		w_vertex(&lists[idx], f->v[k].x, f->v[k].y, f->v[k].z, f->v[k].light,
				 f->v[k].s, f->v[k].t);
	if(tinted)
		w_set_tint(&lists[idx], 255, 255, 255);
}

int main(void) {
	// OLD scheme: 13 all-tinted (12-byte) lists.
	struct tdl Old[CHUNK_MESH_PARTS];
	for(int k = 0; k < CHUNK_MESH_PARTS; k++)
		w_init(&Old[k], true);
	// NEW scheme: 13 untinted (9-byte) + 13 tinted (12-byte) lists.
	struct tdl New[CHUNK_MESH_LISTS];
	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		w_init(&New[k], k >= CHUNK_MESH_PARTS);

	for(int i = 0; i < NFACES; i++) {
		const struct face* f = &FACES[i];
		bool tinted = f->tint != WHITE;
		int ei = emit_index(f->dp, tinted);

		// (1) ROUTING: a face lands in a tinted list iff it is tinted, and the
		// target list's format matches.
		assert((ei >= CHUNK_MESH_PARTS) == tinted);
		assert(New[ei].tinted == tinted);

		emit_face(Old, f->dp, f);
		emit_face(New, ei, f);
	}

	// (2) CONSERVE: no vertex lost. Sum over all NEW lists == faces*4 == sum OLD.
	size_t new_total = 0, old_total = 0;
	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		new_total += w_vertex_count(&New[k]);
	for(int k = 0; k < CHUNK_MESH_PARTS; k++)
		old_total += w_vertex_count(&Old[k]);
	assert(new_total == (size_t)NFACES * 4);
	assert(old_total == (size_t)NFACES * 4);

	// (3)+(4) Replay faces in order, comparing each NEW vertex (white-normalised
	// to 12 bytes for untinted faces) to the corresponding OLD 12-byte vertex.
	// Per-list read cursors advance in emit order, so the per-bucket subsequence
	// order is checked too (untinted faces keep their order in New[dp]; tinted in
	// New[dp+PARTS]; both interleave back to Old[dp]'s emit order).
	size_t old_cur[CHUNK_MESH_PARTS];
	size_t new_cur[CHUNK_MESH_LISTS];
	for(int k = 0; k < CHUNK_MESH_PARTS; k++)
		old_cur[k] = CLL + 3;
	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		new_cur[k] = CLL + 3;

	int checked = 0;
	for(int i = 0; i < NFACES; i++) {
		const struct face* f = &FACES[i];
		bool tinted = f->tint != WHITE;
		int ei = emit_index(f->dp, tinted);

		for(int k = 0; k < 4; k++) {
			uint8_t* o = (uint8_t*)Old[f->dp].data + old_cur[f->dp];
			uint8_t* n = (uint8_t*)New[ei].data + new_cur[ei];

			// pos(6) + light(1) identical in both layouts.
			assert(memcmp(o, n, 7) == 0);

			if(tinted) {
				// NEW tinted vertex is 12 bytes, byte-identical to OLD.
				assert(memcmp(o, n, 12) == 0);
				old_cur[f->dp] += 12;
				new_cur[ei] += 12;
			} else {
				// OLD carried a white tint at [7,8,9]; assert that, then assert
				// the texcoord matches with the 3 tint bytes stripped.
				assert(o[7] == 255 && o[8] == 255 && o[9] == 255);
				assert(o[10] == n[7] && o[11] == n[8]); // texcoord s,t
				old_cur[f->dp] += 12;
				new_cur[ei] += 9;
			}
			checked++;
		}
	}
	assert(checked == NFACES * 4);

	for(int k = 0; k < CHUNK_MESH_PARTS; k++)
		free(Old[k].data);
	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		free(New[k].data);

	printf("test_mesh_tint_split: all assertions passed (%d faces / %d vertices; "
		   "routing tinted<->list>=%d, %zu verts conserved, untinted 9B == old "
		   "12B minus white tint, tinted 12B byte-identical)\n",
		   NFACES, NFACES * 4, CHUNK_MESH_PARTS, new_total);
	return 0;
}

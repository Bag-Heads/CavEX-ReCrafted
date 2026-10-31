/*
	Proves displaylist_render_immediate() walks a display list with the SAME
	stride and field offsets displaylist_vertex() wrote it with -- for TINTED
	lists as well as untinted ones, on both backends.

	THE BUG THIS PINS (2026-07-31 console crash). The immediate renderer was
	hardcoded to the untinted layout on both backends:

	  Wii: GX_Begin(..., GX_VTXFMT0, ...), `base += 9`, texcoord read from
	       offsets 7,8 -- while a tinted vertex is 12 bytes with the biome tint
	       at 7,8,9 and the texcoord at 10,11. Worse, the caller had already run
	       gfx_tint(true) -> GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT), so the GP was
	       promised a CLR1 colour per vertex that the emitter never wrote. An
	       under-fed vertex stream makes the GP keep eating FIFO bytes past the
	       quad = GX FIFO DESYNC, which is the "3D world collapses into
	       horizontal streaks while the immediate-mode GUI and held item keep
	       drawing normally" failure the maintainer photographed.
	  PC:  stride hardcoded to 22 while a tinted vertex is 25.

	Reachability: the ONLY tinted immediate list in the build is the piston's
	moving-block renderer (render_piston_moving.c:164 sets tinted from
	block->biome_tint), and the only biome-tinted blocks are the grass block and
	leaves -- i.e. a piston pushing grass or leaves, which is exactly the
	redstone-torch + piston rig being driven when the console died.

	Pattern note: like test_displaylist_vertex_equiv.c this carries
	self-contained MIRRORS of the backend byte logic (the Wii backend cannot be
	compiled or executed on the host). If you change
	source/platform/{wii,pc}/displaylist.c, change these mirrors.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_U8(b, i) (*((uint8_t*)(b) + (i)))
#define MEM_U16(b, i) (*(uint16_t*)((uint8_t*)(b) + (i)))
#define MEM_FLT(b, i) (*(float*)((uint8_t*)(b) + (i)))

#define CLL 32
#define VERTICES 16

struct vtx {
	int16_t x, y, z;
	uint8_t light, s, t;
	uint8_t tint[3];
};

/* A decoded vertex plus which colour channels the renderer actually emitted.
   `emits_clr1` is the invariant that matters for FIFO safety: it MUST equal
   the list's tinted flag, because the tinted flag is what the caller passed to
   gfx_tint() and therefore what the GP was told to expect. */
struct decoded {
	int16_t x, y, z;
	uint8_t light, s, t;
	uint8_t tint[3];
	bool emits_clr1;
	int vtxfmt;
};

static struct vtx make_vertex(int k) {
	struct vtx v;
	v.x = (int16_t)(k * 37 - 500);
	v.y = (int16_t)(k * -13 + 200);
	v.z = (int16_t)(k * 91);
	v.light = (uint8_t)(k * 7 + 1);
	v.s = (uint8_t)(k * 3 + 11);
	v.t = (uint8_t)(k * 5 + 23);
	v.tint[0] = (uint8_t)(k * 11 + 2);
	v.tint[1] = (uint8_t)(k * 13 + 3);
	v.tint[2] = (uint8_t)(k * 17 + 5);
	return v;
}

/* ---- Wii backend ------------------------------------------------------- */
/* WRITER: mirrors displaylist_vertex() in source/platform/wii/displaylist.c. */
static size_t wii_write(uint8_t* buf, size_t index, const struct vtx* v,
						bool tinted) {
	uint8_t* p = buf + index;
	MEM_U16(p, 0) = (uint16_t)v->x;
	MEM_U16(p, 2) = (uint16_t)v->y;
	MEM_U16(p, 4) = (uint16_t)v->z;
	MEM_U8(p, 6) = v->light;
	if(tinted) {
		MEM_U8(p, 7) = v->tint[0];
		MEM_U8(p, 8) = v->tint[1];
		MEM_U8(p, 9) = v->tint[2];
		MEM_U8(p, 10) = v->s;
		MEM_U8(p, 11) = v->t;
		return index + 12;
	}
	MEM_U8(p, 7) = v->s;
	MEM_U8(p, 8) = v->t;
	return index + 9;
}

/* READER: mirrors displaylist_render_immediate() in the same file. Each
   GX_* emit is recorded rather than issued. */
static size_t wii_read_immediate(const uint8_t* base, struct decoded* out,
								 bool tinted) {
	out->vtxfmt = tinted ? 4 : 0; /* GX_VTXFMT4 : GX_VTXFMT0 */
	out->x = (int16_t)MEM_U16(base, 0);
	out->y = (int16_t)MEM_U16(base, 2);
	out->z = (int16_t)MEM_U16(base, 4);
	out->light = MEM_U8(base, 6);
	if(tinted) {
		out->tint[0] = MEM_U8(base, 7);
		out->tint[1] = MEM_U8(base, 8);
		out->tint[2] = MEM_U8(base, 9);
		out->s = MEM_U8(base, 10);
		out->t = MEM_U8(base, 11);
		out->emits_clr1 = true;
		return 12;
	}
	out->s = MEM_U8(base, 7);
	out->t = MEM_U8(base, 8);
	out->emits_clr1 = false;
	out->tint[0] = out->tint[1] = out->tint[2] = 0;
	return 9;
}

/* ---- PC backend -------------------------------------------------------- */
static size_t pc_write(uint8_t* buf, size_t index, const struct vtx* v,
					   bool tinted) {
	uint8_t* p = buf + index;
	MEM_FLT(p, 0) = (float)v->x / 256.0F;
	MEM_FLT(p, 4) = (float)v->y / 256.0F;
	MEM_FLT(p, 8) = (float)v->z / 256.0F;
	MEM_U8(p, 12) = v->light % 16;
	MEM_U8(p, 13) = v->light / 16;
	if(tinted) {
		MEM_U8(p, 14) = v->tint[0];
		MEM_U8(p, 15) = v->tint[1];
		MEM_U8(p, 16) = v->tint[2];
		MEM_FLT(p, 17) = (float)v->s / 256.0F;
		MEM_FLT(p, 21) = (float)v->t / 256.0F;
		return index + 25;
	}
	MEM_FLT(p, 14) = (float)v->s / 256.0F;
	MEM_FLT(p, 18) = (float)v->t / 256.0F;
	return index + 22;
}

/* The PC immediate renderer is expressed as glVertexAttribPointer strides and
   offsets; mirror exactly those numbers. */
static size_t pc_read_immediate(const uint8_t* base, struct vtx* out,
								bool tinted) {
	size_t stride = tinted ? 25 : 22;
	out->x = (int16_t)(MEM_FLT(base, 0) * 256.0F);
	out->y = (int16_t)(MEM_FLT(base, 4) * 256.0F);
	out->z = (int16_t)(MEM_FLT(base, 8) * 256.0F);
	out->light = (uint8_t)(MEM_U8(base, 12) + MEM_U8(base, 13) * 16);
	if(tinted) {
		/* attrib 1 at 2 + 3*sizeof(float) = 14, attrib 2 at 2 + 3 + 12 = 17 */
		out->tint[0] = MEM_U8(base, 14);
		out->tint[1] = MEM_U8(base, 15);
		out->tint[2] = MEM_U8(base, 16);
		out->s = (uint8_t)(MEM_FLT(base, 17) * 256.0F);
		out->t = (uint8_t)(MEM_FLT(base, 21) * 256.0F);
	} else {
		out->tint[0] = out->tint[1] = out->tint[2] = 0;
		out->s = (uint8_t)(MEM_FLT(base, 14) * 256.0F);
		out->t = (uint8_t)(MEM_FLT(base, 18) * 256.0F);
	}
	return stride;
}

static void check_wii(bool tinted) {
	uint8_t* buf = calloc(1, CLL + 3 + VERTICES * 16);
	assert(buf);
	size_t index = CLL + 3;
	size_t start = index;
	for(int k = 0; k < VERTICES; k++) {
		struct vtx v = make_vertex(k);
		index = wii_write(buf, index, &v, tinted);
	}
	/* The writer's total span defines the stride the reader MUST use. */
	size_t written_stride = (index - start) / VERTICES;
	assert(written_stride == (size_t)(tinted ? 12 : 9));

	const uint8_t* base = buf + start;
	for(int k = 0; k < VERTICES; k++) {
		struct vtx want = make_vertex(k);
		struct decoded got;
		size_t step = wii_read_immediate(base, &got, tinted);

		assert(step == written_stride
			   && "immediate reader stride must equal the writer's stride");
		assert(got.x == want.x && got.y == want.y && got.z == want.z);
		assert(got.light == want.light);
		assert(got.s == want.s
			   && "texcoord S must survive -- reading it from the tint bytes is "
				  "the pre-fix failure");
		assert(got.t == want.t);
		/* THE FIFO-SAFETY INVARIANT: gfx_tint(list->tinted) declared CLR1 to
		 * the GP iff tinted, so the emitter must write CLR1 iff tinted. Any
		 * mismatch under-feeds (or over-feeds) the vertex stream. */
		assert(got.emits_clr1 == tinted
			   && "CLR1 must be emitted exactly when the descriptor declares it");
		assert(got.vtxfmt == (tinted ? 4 : 0)
			   && "a tinted list must draw as GX_VTXFMT4 (the only format with a "
				  "CLR1 attribute)");
		if(tinted) {
			assert(got.tint[0] == want.tint[0] && got.tint[1] == want.tint[1]
				   && got.tint[2] == want.tint[2]);
		}
		base += step;
	}
	free(buf);
}

static void check_pc(bool tinted) {
	uint8_t* buf = calloc(1, VERTICES * 32);
	assert(buf);
	size_t index = 0;
	for(int k = 0; k < VERTICES; k++) {
		struct vtx v = make_vertex(k);
		index = pc_write(buf, index, &v, tinted);
	}
	size_t written_stride = index / VERTICES;
	assert(written_stride == (size_t)(tinted ? 25 : 22));

	const uint8_t* base = buf;
	for(int k = 0; k < VERTICES; k++) {
		struct vtx want = make_vertex(k);
		struct vtx got;
		size_t step = pc_read_immediate(base, &got, tinted);
		assert(step == written_stride
			   && "PC immediate stride must equal the writer's stride");
		assert(got.x == want.x && got.y == want.y && got.z == want.z);
		assert(got.light == want.light);
		assert(got.s == want.s && got.t == want.t);
		if(tinted) {
			assert(got.tint[0] == want.tint[0] && got.tint[1] == want.tint[1]
				   && got.tint[2] == want.tint[2]);
		}
		base += step;
	}
	free(buf);
}

int main(void) {
	check_wii(false);
	check_wii(true);
	check_pc(false);
	check_pc(true);
	printf("test_displaylist_immediate_layout: tinted + untinted immediate "
		   "draws round-trip on both backends OK\n");
	return 0;
}

/*
	Microbenchmark for R1 (per-list biome-tint split): how many fewer bytes does a
	realistic chunk mesh emit (and stream to GX) when the ~97% of faces with no
	biome tint drop from the 12-byte GX_VTXFMT4 vertex to the 9-byte GX_VTXFMT0
	vertex? Emits the SAME face set the OLD way (all 12-byte/tinted) and the NEW
	way (untinted faces 9-byte, tinted faces 12-byte) and reports the byte delta
	and the emit-time delta.

	The byte delta is exact and is the real prize: it is both CPU store traffic at
	build time AND GX FIFO bandwidth every frame the chunk is drawn (the latter is
	the bottleneck on real Wii silicon, which x86 + Dolphin do not model). The
	time delta here is the build-side store cost only; x86's out-of-order stores
	understate the in-order PowerPC cost, so treat it as a LOWER bound. Always
	exits 0; read stdout.
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MEM_U8(b, i) (*((uint8_t*)(b) + (i)))
#define MEM_U16(b, i) (*(uint16_t*)((uint8_t*)(b) + (i)))
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
	if(!l->data)
		l->data = malloc(l->length + CLL);
	if(l->index + vtx_bytes > l->length) {
		l->length = (l->length * 3 / 2 + vtx_bytes + CLL - 1) / CLL * CLL;
		l->data = realloc(l->data, l->length + CLL);
	}
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

struct vtx {
	int16_t x, y, z;
	uint8_t light, s, t;
};

#define NFACES 8000
#define REPS 2000

int main(void) {
	// A realistic-ish chunk mix: a small fraction of faces are biome-tinted
	// (grass tops + leaves); the rest (terrain, wood, stone, ore...) are not.
	// Mark every 1-in-32 face tinted (~3.1%), in line with a grassy/forested
	// overworld chunk.
	const int tint_stride = 32;
	struct vtx* fv = malloc((size_t)NFACES * 4 * sizeof(struct vtx));
	bool* ftint = malloc((size_t)NFACES * sizeof(bool));
	int n_tinted = 0;
	for(int i = 0; i < NFACES; i++) {
		ftint[i] = (i % tint_stride) == 0;
		if(ftint[i])
			n_tinted++;
		for(int k = 0; k < 4; k++) {
			int j = i * 4 + k;
			fv[j].x = (int16_t)(j * 7);
			fv[j].y = (int16_t)(j * 3);
			fv[j].z = (int16_t)(j * 5);
			fv[j].light = (uint8_t)(j & 0xFF);
			fv[j].s = (uint8_t)((j * 2) & 0xFF);
			fv[j].t = (uint8_t)((j * 3) & 0xFF);
		}
	}

	// OLD: one all-tinted (12-byte) list. NEW: an untinted (9-byte) + a tinted
	// (12-byte) list; each face routes by its tint flag.
	struct tdl old_l, new_u, new_t;

	double old_ms = 0, new_ms = 0;
	size_t old_bytes = 0, new_bytes = 0;

	clock_t c0 = clock();
	for(int r = 0; r < REPS; r++) {
		w_init(&old_l, true);
		old_l.index = CLL + 3;
		if(r == 0)
			w_ensure(&old_l, NFACES * 4 * 12); // grow once, time steady state
		old_l.index = CLL + 3;
		for(int i = 0; i < NFACES; i++) {
			old_l.tint[0] = 90;
			old_l.tint[1] = 160;
			old_l.tint[2] = 60;
			for(int k = 0; k < 4; k++) {
				struct vtx* v = &fv[i * 4 + k];
				w_vertex(&old_l, v->x, v->y, v->z, v->light, v->s, v->t);
			}
		}
		old_bytes = old_l.index - (CLL + 3);
	}
	clock_t c1 = clock();
	old_ms = (double)(c1 - c0) / CLOCKS_PER_SEC * 1000.0 / REPS;

	clock_t c2 = clock();
	for(int r = 0; r < REPS; r++) {
		w_init(&new_u, false);
		w_init(&new_t, true);
		if(r == 0) {
			w_ensure(&new_u, NFACES * 4 * 9);
			w_ensure(&new_t, NFACES * 4 * 12);
		}
		new_u.index = CLL + 3;
		new_t.index = CLL + 3;
		for(int i = 0; i < NFACES; i++) {
			if(ftint[i]) {
				new_t.tint[0] = 90;
				new_t.tint[1] = 160;
				new_t.tint[2] = 60;
				for(int k = 0; k < 4; k++) {
					struct vtx* v = &fv[i * 4 + k];
					w_vertex(&new_t, v->x, v->y, v->z, v->light, v->s, v->t);
				}
			} else {
				for(int k = 0; k < 4; k++) {
					struct vtx* v = &fv[i * 4 + k];
					w_vertex(&new_u, v->x, v->y, v->z, v->light, v->s, v->t);
				}
			}
		}
		new_bytes = (new_u.index - (CLL + 3)) + (new_t.index - (CLL + 3));
	}
	clock_t c3 = clock();
	new_ms = (double)(c3 - c2) / CLOCKS_PER_SEC * 1000.0 / REPS;

	printf("bench_mesh_tint_split: %d faces (%d vertices), %d tinted (%.1f%%)\n",
		   NFACES, NFACES * 4, n_tinted, 100.0 * n_tinted / NFACES);
	printf("  OLD all-tinted 12B/vertex : %zu bytes/chunk, %.4f ms/build\n",
		   old_bytes, old_ms);
	printf("  NEW split 9B untinted+12B : %zu bytes/chunk, %.4f ms/build\n",
		   new_bytes, new_ms);
	printf("  -> %.1f%% fewer mesh bytes (CPU store + GX FIFO every frame), "
		   "%.2fx emit time\n",
		   100.0 * (old_bytes - new_bytes) / old_bytes,
		   new_ms > 0 ? old_ms / new_ms : 0.0);
	printf("  NOTE: byte delta is exact + recurs every drawn frame on real Wii; "
		   "x86 store timing is a LOWER bound for in-order PPC.\n");
	free(fv);
	free(ftint);
	free(old_l.data);
	free(new_u.data);
	free(new_t.data);
	return 0;
}

/*
	Microbenchmark: does batching a whole FACE (quad) into one display-list call
	measurably beat the current per-vertex displaylist_vertex (4 calls/face)?

	The per-vertex primitive already reads l->data/index/length/tinted/tint and
	advances l->index ONCE PER VERTEX. A per-face primitive would read them once
	per FACE (4 vertices) instead. This bench isolates exactly that overhead (the
	byte writes are identical) on the Wii S16/U8 tinted layout, so we can decide
	whether the bigger emitter restructure + the weaker (render-only, not
	allocation) byte-exactness it forces are worth it. Always exits 0; read
	stdout. (x86 OOO understates the in-order PowerPC reload cost, so treat the
	ratio as a LOWER bound for the real Wii; and the real mesher also pays the
	renderBlock per-vertex compute this bench omits, which DILUTES the win.)
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MEM_U8(b, i) (*((uint8_t*)(b) + (i)))
#define MEM_U16(b, i) (*(uint16_t*)((uint8_t*)(b) + (i)))

struct tdl {
	void* data;
	size_t length;
	size_t index;
	bool tinted;
	uint8_t tint[3];
};
#define CLL 32
#define CACHE_LINES(v, s) (((v) * (s) + 3 + CLL - 1) / CLL * CLL)

static void w_init(struct tdl* l, size_t vertices, size_t vertex_size) {
	l->length = CACHE_LINES(vertices, vertex_size);
	l->data = NULL;
	l->index = CLL + 3;
	l->tinted = false;
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

// REFERENCE: the current per-vertex primitive (mirrors displaylist_vertex).
static void w_vertex(struct tdl* l, int16_t x, int16_t y, int16_t z,
					 uint8_t light, uint8_t s, uint8_t t) {
	w_ensure(l, l->tinted ? 12 : 9);
	uint8_t* restrict p = (uint8_t*)l->data + l->index;
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

// OPTIMIZED: one call per face. data/index/length/tinted/tint read once; the
// reserve, cursor and index-advance are per-face. Byte writes are identical.
static void w_quad(struct tdl* l, const struct vtx v[4]) {
	int vtx_bytes = l->tinted ? 12 : 9;
	w_ensure(l, 4 * vtx_bytes);
	bool tinted = l->tinted;
	uint8_t t0 = l->tint[0], t1 = l->tint[1], t2 = l->tint[2];
	uint8_t* restrict p = (uint8_t*)l->data + l->index;
	for(int k = 0; k < 4; k++) {
		MEM_U16(p, 0) = v[k].x;
		MEM_U16(p, 2) = v[k].y;
		MEM_U16(p, 4) = v[k].z;
		MEM_U8(p, 6) = v[k].light;
		if(tinted) {
			MEM_U8(p, 7) = t0;
			MEM_U8(p, 8) = t1;
			MEM_U8(p, 9) = t2;
			MEM_U8(p, 10) = v[k].s;
			MEM_U8(p, 11) = v[k].t;
			p += 12;
		} else {
			MEM_U8(p, 7) = v[k].s;
			MEM_U8(p, 8) = v[k].t;
			p += 9;
		}
	}
	l->index += 4 * vtx_bytes;
}

#define NFACES 16384
#define REPS 400

int main(void) {
	struct vtx* faces = malloc((size_t)NFACES * 4 * sizeof(struct vtx));
	for(int i = 0; i < NFACES * 4; i++) {
		faces[i].x = (int16_t)(i * 7);
		faces[i].y = (int16_t)(i * 3);
		faces[i].z = (int16_t)(i * 5);
		faces[i].light = (uint8_t)(i & 0xFF);
		faces[i].s = (uint8_t)((i * 2) & 0xFF);
		faces[i].t = (uint8_t)((i * 3) & 0xFF);
	}

	struct tdl a, b;
	w_init(&a, 64, 12);
	a.tinted = true;
	a.tint[0] = 90;
	a.tint[1] = 160;
	a.tint[2] = 60;
	w_init(&b, 64, 12);
	b.tinted = true;
	b.tint[0] = 90;
	b.tint[1] = 160;
	b.tint[2] = 60;

	// Warm up: grow both buffers to full size so the timed loops never realloc
	// (we want the steady-state per-vertex check/reload cost, not grow cost).
	for(int i = 0; i < NFACES; i++) {
		for(int k = 0; k < 4; k++)
			w_vertex(&a, faces[i * 4 + k].x, faces[i * 4 + k].y,
					 faces[i * 4 + k].z, faces[i * 4 + k].light,
					 faces[i * 4 + k].s, faces[i * 4 + k].t);
		w_quad(&b, &faces[i * 4]);
	}
	// Render-byte equivalence sanity (the drawn stream must match).
	int rc = memcmp((uint8_t*)a.data + CLL + 3, (uint8_t*)b.data + CLL + 3,
					a.index - (CLL + 3));

	clock_t c0 = clock();
	for(int r = 0; r < REPS; r++) {
		a.index = CLL + 3;
		for(int i = 0; i < NFACES; i++)
			for(int k = 0; k < 4; k++) {
				const struct vtx* v = &faces[i * 4 + k];
				w_vertex(&a, v->x, v->y, v->z, v->light, v->s, v->t);
			}
	}
	clock_t c1 = clock();
	for(int r = 0; r < REPS; r++) {
		b.index = CLL + 3;
		for(int i = 0; i < NFACES; i++)
			w_quad(&b, &faces[i * 4]);
	}
	clock_t c2 = clock();

	double ref_ms = (double)(c1 - c0) / CLOCKS_PER_SEC * 1000.0 / REPS;
	double opt_ms = (double)(c2 - c1) / CLOCKS_PER_SEC * 1000.0 / REPS;

	printf("bench_displaylist_emit: %d faces/rep (%d vertices), Wii tinted "
		   "layout\n",
		   NFACES, NFACES * 4);
	printf("  ref  4x displaylist_vertex/face : %.4f ms/rep\n", ref_ms);
	printf("  opt  1x displaylist_quad/face   : %.4f ms/rep  (%.2fx faster)\n",
		   opt_ms, ref_ms / opt_ms);
	printf("  render bytes identical: %s\n", rc == 0 ? "yes" : "NO");
	printf("  NOTE: write-path only (real mesher also pays per-vertex "
		   "renderBlock compute, diluting this); x86 understates in-order PPC.\n");
	free(a.data);
	free(b.data);
	free(faces);
	return 0;
}

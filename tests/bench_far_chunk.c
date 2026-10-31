/*
  Standalone microbenchmark + equivalence guard for the far-ring chunk build.
  far_chunk_direct MIRRORS source/world.c world_build_far_section;
  far_chunk_reference produces the SAME current synthetic section through the slow
  per-cell world_set_block path. The equivalence gate runs both Overworld and Nether
  fixtures and asserts byte-identical blocks + heightmap, including full spanned-air
  fill, mixed FAR_NO_SURFACE columns, zero filler light, and a lava surface. It also
  reports the direct fill's speedup over the slow reference.
  Faithfully replicates the data structures and the hot code paths from:
    - source/world.c                      world_build_far_section, world_set_block
    - source/chunk.c                      chunk_set_block (176-191),
                                          chunk_trigger_neighbour_update (137-160),
                                          chunk_init (38-66)
    - source/lighting.c                   lighting_heightmap_update (24-50)
  Models a realistic far ring: chunks 1-3 sections tall, surface terrain.

  This is COMPUTE-only; it does not model dict/malloc allocator jitter the same
  way the real game does, but it captures the dominant per-cell work (bit-packed
  store, heightmap branch, neighbour-trigger branch, indexing) faithfully and the
  dict_get + malloc are modeled as a cheap hash + arena to keep the comparison
  honest (both paths pay them equally for chunk creation).
*/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHUNK_SIZE 16
#define WORLD_HEIGHT 128
#define COLUMN_HEIGHT ((WORLD_HEIGHT + CHUNK_SIZE - 1) / CHUNK_SIZE)
#define CHUNK_INDEX(x, y, z) ((x) + ((z) + (y) * CHUNK_SIZE) * CHUNK_SIZE)
#define BLOCK_AIR 0
#define BLOCK_STONE 1
#define BLOCK_GRASS 2
#define BLOCK_LAVA_STILL 11
#define FAR_NO_SURFACE 0xFF

enum world_dim {
    WORLD_DIM_NETHER = -1,
    WORLD_DIM_OVERWORLD = 0,
};

// ---- block table (only the predicates the heightmap/mesher need) ----
struct block { bool can_see_through; uint8_t opacity; };
static struct block btable[256];
static struct block* blocks[256];
// Match the production properties relevant to heightmap + far-surface output.
static void blocks_init(void) {
    for (int i = 0; i < 256; i++) blocks[i] = NULL;
    btable[BLOCK_STONE].can_see_through = false; btable[BLOCK_STONE].opacity = 15;
    blocks[BLOCK_STONE] = &btable[BLOCK_STONE];
    btable[BLOCK_GRASS].can_see_through = false; btable[BLOCK_GRASS].opacity = 15;
    blocks[BLOCK_GRASS] = &btable[BLOCK_GRASS];
    // Production lava is see-through to the LOD surface scan but opacity 15, so it
    // still contributes to the lighting heightmap once extraction selects its top.
    btable[BLOCK_LAVA_STILL].can_see_through = true;
    btable[BLOCK_LAVA_STILL].opacity = 15;
    blocks[BLOCK_LAVA_STILL] = &btable[BLOCK_LAVA_STILL];
    // air id 0 stays NULL like the real table (blocks[0] is air, can_see_through)
    btable[0].can_see_through = true;  btable[0].opacity = 0;   blocks[0] = &btable[0];
}

struct block_data {
    uint8_t type, metadata, sky_light, torch_light;
};

// ---- chunk ----
struct chunk {
    int32_t x, y, z;
    uint8_t* blocks;
    bool rebuild_displist;
    uint32_t dirty_generation;
    int reference_count;
};

struct world_section {
    uint8_t heightmap[CHUNK_SIZE * CHUNK_SIZE];
    uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE];
    uint8_t grass_j[CHUNK_SIZE * CHUNK_SIZE];
    struct chunk* column[COLUMN_HEIGHT];
};

// Minimal world: one section (the far chunk under test) plus the 4 neighbour
// sections so chunk_trigger_neighbour_update finds real neighbours (worst case:
// neighbours present => the boundary writes do real work).
struct world {
    struct world_section* sec;       // chunk under test
    struct world_section* nb[4];     // -x +x -z +z neighbours (one column each)
    enum world_dim dimension;
};

// ---- allocator arena (so malloc jitter doesn't dominate) ----
#define ARENA_CHUNKS 4096
static struct chunk arena_chunks[ARENA_CHUNKS];
static int arena_next;
static uint8_t arena_blocks[ARENA_CHUNKS][CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*5/2];

static struct chunk* chunk_alloc(int cx, int cy, int cz) {
    struct chunk* c = &arena_chunks[arena_next];
    c->blocks = arena_blocks[arena_next];
    arena_next++;
    memset(c->blocks, BLOCK_AIR, CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*5/2);
    c->x = cx*CHUNK_SIZE; c->y = cy*CHUNK_SIZE; c->z = cz*CHUNK_SIZE;
    c->rebuild_displist = false; c->dirty_generation = 0; c->reference_count = 1;
    return c;
}

// ---- chunk_set_block: exact bit-packing from chunk.c:176-191 ----
static inline void chunk_set_block(struct chunk* c, int x, int y, int z,
                                   struct block_data blk) {
    size_t idx = CHUNK_INDEX(x,y,z)/2*5;
    size_t off = CHUNK_INDEX(x,y,z)%2;
    c->blocks[idx+off+0] = blk.type;
    c->blocks[idx+off+2] = (blk.torch_light<<4)|blk.sky_light;
    c->blocks[idx+4] = (c->blocks[idx+4] & ~(0x0F<<(off*4))) | (blk.metadata<<(off*4));
    c->dirty_generation++;          // chunk_mark_dirty
    c->rebuild_displist = true;
    // chunk_trigger_neighbour_update: 6 boundary checks; on boundary, find nb & dirty it
    bool cond[6] = {x==0, x==CHUNK_SIZE-1, y==0, y==CHUNK_SIZE-1, z==0, z==CHUNK_SIZE-1};
    // Only the 4 horizontal neighbours modeled (top/bottom in-column not present here).
    extern struct world* gW;
    if (cond[0] && gW->nb[0]) { /* find chunk in nb column */ for(int k=0;k<COLUMN_HEIGHT;k++) if(gW->nb[0]->column[k]) gW->nb[0]->column[k]->rebuild_displist=true; }
    if (cond[1] && gW->nb[1]) { for(int k=0;k<COLUMN_HEIGHT;k++) if(gW->nb[1]->column[k]) gW->nb[1]->column[k]->rebuild_displist=true; }
    if (cond[4] && gW->nb[2]) { for(int k=0;k<COLUMN_HEIGHT;k++) if(gW->nb[2]->column[k]) gW->nb[2]->column[k]->rebuild_displist=true; }
    if (cond[5] && gW->nb[3]) { for(int k=0;k<COLUMN_HEIGHT;k++) if(gW->nb[3]->column[k]) gW->nb[3]->column[k]->rebuild_displist=true; }
}
struct world* gW;

static inline struct block_data chunk_get_block(struct chunk* c, int x, int y, int z) {
    size_t idx = CHUNK_INDEX(x,y,z)/2*5;
    size_t off = CHUNK_INDEX(x,y,z)%2;
    struct block_data b;
    b.type = c->blocks[idx+off+0];
    b.metadata = (c->blocks[idx+4]>>(off*4))&0xF;
    b.sky_light = c->blocks[idx+off+2]&0xF;
    b.torch_light = c->blocks[idx+off+2]>>4;
    return b;
}

// ---- heightmap get_block callback (world.c:239-247) ----
static bool wsection_heightmap_get_block(void* user, int x, int y, int z, struct block_data* blk) {
    struct world_section* s = user;
    struct chunk* c = s->column[y / CHUNK_SIZE];
    if (c) *blk = chunk_get_block(c, x, y & (CHUNK_SIZE-1), z);
    return c != NULL;
}

// ---- lighting_heightmap_update: exact from lighting.c:24-50 ----
static void lighting_heightmap_update(uint8_t* heightmap, int x, int y, int z,
                                      uint8_t type,
                                      bool (*get_block)(void*,int,int,int,struct block_data*),
                                      void* user) {
    uint8_t* height = heightmap + x + z*CHUNK_SIZE;
    if (blocks[type] && (!blocks[type]->can_see_through || blocks[type]->opacity > 0)) {
        if (y >= *height) *height = y + 1;
    } else if (y < *height) {
        while (*height > 0) {
            struct block_data blk;
            if (get_block(user, x, *height-1, z, &blk) && blocks[blk.type]
                && (!blocks[blk.type]->can_see_through || blocks[blk.type]->opacity > 0))
                break;
            (*height)--;
        }
    }
}

// ---- world_set_block: exact path from world.c:249-303 (no light_update) ----
static void world_set_block(struct world* w, int x, int y, int z, struct block_data blk) {
    if (y < 0 || y >= WORLD_HEIGHT) return;
    struct world_section* s = w->sec;     // dict_wsection_get modeled as the single section
    int cy = y / CHUNK_SIZE;
    struct chunk* c = (y>=0 && y<WORLD_HEIGHT && s) ? s->column[cy] : NULL;
    if (!c) {
        c = chunk_alloc(0, cy, 0);
        s->column[cy] = c;
    }
    chunk_set_block(c, x & (CHUNK_SIZE-1), y & (CHUNK_SIZE-1), z & (CHUNK_SIZE-1), blk);
    lighting_heightmap_update(s->heightmap, x & (CHUNK_SIZE-1), y, z & (CHUNK_SIZE-1),
                              blk.type, wsection_heightmap_get_block, s);
}

struct far_span {
    int chunk_min, chunk_max;
    int cy_lo, cy_hi;
    int y_top;
};

// Exact vertical-span calculation from world_build_far_section.
static bool far_span_get(const uint8_t* skin_y, struct far_span* out) {
    out->chunk_min = WORLD_HEIGHT;
    out->chunk_max = 0;
    for (int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; i++) {
        int sy = skin_y[i];
        if (sy >= WORLD_HEIGHT) continue;
        if (sy < out->chunk_min) out->chunk_min = sy;
        if (sy + 2 > out->chunk_max) out->chunk_max = sy + 2;
    }
    if (out->chunk_min >= WORLD_HEIGHT) return false;
    if (out->chunk_max >= WORLD_HEIGHT) out->chunk_max = WORLD_HEIGHT - 1;
    out->cy_lo = out->chunk_min / CHUNK_SIZE;
    out->cy_hi = out->chunk_max / CHUNK_SIZE;
    out->y_top = (out->cy_hi + 1) * CHUNK_SIZE - 1;
    if (out->y_top >= WORLD_HEIGHT) out->y_top = WORLD_HEIGHT - 1;
    return true;
}

static uint8_t far_open_sky(const struct world* w) {
    return w->dimension == WORLD_DIM_OVERWORLD ? 15 : 0;
}

// =========================================================================
//  SLOW REFERENCE: current synthetic output via per-cell world_set_block
// =========================================================================
static void far_chunk_reference(struct world* w, const uint8_t* skin_y,
                                const uint8_t* top_id, const uint8_t* grass_i,
                                const uint8_t* grass_j) {
    struct far_span span;
    if (!far_span_get(skin_y, &span)) return;
    const uint8_t open_sky = far_open_sky(w);

    for (int lx = 0; lx < CHUNK_SIZE; lx++) {
        for (int lz = 0; lz < CHUNK_SIZE; lz++) {
            int i = lx*CHUNK_SIZE + lz;
            uint8_t sy = skin_y[i];
            if (sy >= WORLD_HEIGHT) {
                for (int y = span.cy_lo * CHUNK_SIZE; y <= span.y_top; y++)
                    world_set_block(w, lx, y, lz,
                        (struct block_data){.type=BLOCK_AIR,.metadata=0,
                                            .sky_light=open_sky,.torch_light=0});
                w->sec->heightmap[lx + lz*CHUNK_SIZE] = 0;
                continue;
            }
            uint8_t tid = top_id[i];
            for (int y = span.chunk_min; y <= (int)sy; y++)
                world_set_block(w, lx, y, lz,
                    (struct block_data){.type=tid,.metadata=0,.sky_light=0,.torch_light=0});
            for (int y = (int)sy+1; y <= span.y_top; y++)
                world_set_block(w, lx, y, lz,
                    (struct block_data){.type=BLOCK_AIR,.metadata=0,
                                        .sky_light=open_sky,.torch_light=0});
        }
    }
    // tint
    memcpy(w->sec->grass_i, grass_i, CHUNK_SIZE*CHUNK_SIZE);
    memcpy(w->sec->grass_j, grass_j, CHUNK_SIZE*CHUNK_SIZE);
}

// =========================================================================
//  DIRECT-FILL path: write the chunk blocks buffer + heightmap directly,
//  bypassing per-cell world_set_block, neighbour-trigger, and per-cell
//  heightmap scan. Marks dirty once per chunk, dirties the 4 neighbours once.
// =========================================================================
static inline void chunk_set_block_raw(struct chunk* c, int x, int y, int z,
                                       struct block_data blk) {
    size_t idx = CHUNK_INDEX(x,y,z)/2*5;
    size_t off = CHUNK_INDEX(x,y,z)%2;
    c->blocks[idx+off+0] = blk.type;
    c->blocks[idx+off+2] = (blk.torch_light<<4)|blk.sky_light;
    c->blocks[idx+4] = (c->blocks[idx+4] & ~(0x0F<<(off*4))) | (blk.metadata<<(off*4));
}

static void far_chunk_direct(struct world* w, const uint8_t* skin_y,
                             const uint8_t* top_id, const uint8_t* grass_i,
                             const uint8_t* grass_j) {
    struct world_section* s = w->sec;
    struct far_span span;
    if (!far_span_get(skin_y, &span)) return;
    const uint8_t open_sky = far_open_sky(w);

    // Pre-create the chunks this fill spans (chunk_min .. max surface+2), once.
    for (int cy = span.cy_lo; cy <= span.cy_hi; cy++)
        if (!s->column[cy]) s->column[cy] = chunk_alloc(0, cy, 0);

    // Fill directly + maintain the heightmap as a simple max (the surface is the
    // top opaque cell of each column, known directly here -> no downward scan).
    for (int lx = 0; lx < CHUNK_SIZE; lx++) {
        for (int lz = 0; lz < CHUNK_SIZE; lz++) {
            int i = lx*CHUNK_SIZE + lz;
            uint8_t sy = skin_y[i];
            if (sy >= WORLD_HEIGHT) {
                for (int y = span.cy_lo * CHUNK_SIZE; y <= span.y_top; y++)
                    chunk_set_block_raw(s->column[y/CHUNK_SIZE], lx,
                        y&(CHUNK_SIZE-1), lz,
                        (struct block_data){.type=BLOCK_AIR,.metadata=0,
                                            .sky_light=open_sky,.torch_light=0});
                s->heightmap[lx + lz*CHUNK_SIZE] = 0;
                continue;
            }
            uint8_t tid = top_id[i];
            for (int y = span.chunk_min; y <= (int)sy; y++) {
                struct chunk* c = s->column[y/CHUNK_SIZE];
                chunk_set_block_raw(c, lx, y&(CHUNK_SIZE-1), lz,
                    (struct block_data){.type=tid,.metadata=0,.sky_light=0,.torch_light=0});
            }
            for (int y = (int)sy+1; y <= span.y_top; y++) {
                struct chunk* c = s->column[y/CHUNK_SIZE];
                chunk_set_block_raw(c, lx, y&(CHUNK_SIZE-1), lz,
                    (struct block_data){.type=BLOCK_AIR,.metadata=0,
                                        .sky_light=open_sky,.torch_light=0});
            }
            // surface is the top opaque cell -> heightmap = sy+1 (matches the
            // lighting_heightmap_update fixpoint: highest opaque y, +1)
            s->heightmap[lx + lz*CHUNK_SIZE] = (uint8_t)(sy + 1);
        }
    }

    // Mark each touched chunk dirty ONCE, and dirty the 4 horizontal neighbours ONCE.
    for (int cy = span.cy_lo; cy <= span.cy_hi; cy++) {
        struct chunk* c = s->column[cy];
        c->dirty_generation++;
        c->rebuild_displist = true;
    }
    for (int n = 0; n < 4; n++)
        if (w->nb[n])
            for (int k = 0; k < COLUMN_HEIGHT; k++)
                if (w->nb[n]->column[k]) w->nb[n]->column[k]->rebuild_displist = true;

    memcpy(s->grass_i, grass_i, CHUNK_SIZE*CHUNK_SIZE);
    memcpy(s->grass_j, grass_j, CHUNK_SIZE*CHUNK_SIZE);
}

// =========================================================================
//  Driver
// =========================================================================
static double now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec*1e6 + ts.tv_nsec/1e3;
}

// Build a realistic far-column: rolling surface around y0 with +-amp, all opaque.
static void make_column(uint8_t* skin_y, uint8_t* top_id, int y0, int amp, int seed) {
    unsigned s = seed;
    for (int lx = 0; lx < CHUNK_SIZE; lx++)
        for (int lz = 0; lz < CHUNK_SIZE; lz++) {
            int i = lx*CHUNK_SIZE + lz;
            s = s*1103515245 + 12345;
            int d = ((s>>16) % (2*amp+1)) - amp;
            int sy = y0 + d; if (sy<1) sy=1; if (sy>WORLD_HEIGHT-4) sy=WORLD_HEIGHT-4;
            skin_y[i] = (uint8_t)sy;
            top_id[i] = ((s>>8)&1) ? BLOCK_GRASS : BLOCK_STONE;
        }
}

static void make_mixed_column(uint8_t* skin_y, uint8_t* top_id) {
    make_column(skin_y, top_id, 72, 6, 999);
    skin_y[0 * CHUNK_SIZE + 0] = FAR_NO_SURFACE;
    top_id[0 * CHUNK_SIZE + 0] = BLOCK_AIR;
    skin_y[0 * CHUNK_SIZE + 1] = 31;
    top_id[0 * CHUNK_SIZE + 1] = BLOCK_LAVA_STILL;
    skin_y[0 * CHUNK_SIZE + 2] = 47;
    top_id[0 * CHUNK_SIZE + 2] = BLOCK_STONE;
}

static int count_cells(const uint8_t* skin_y) {
    struct far_span span;
    if (!far_span_get(skin_y, &span)) return 0;
    int cells=0;
    for (int i=0;i<256;i++) {
        uint8_t sy=skin_y[i];
        cells += (sy >= WORLD_HEIGHT)
            ? span.y_top - span.cy_lo * CHUNK_SIZE + 1
            : span.y_top - span.chunk_min + 1;
    }
    return cells;
}

static void reset_model(struct world* w) {
    arena_next = 0;
    memset(w->sec, 0, sizeof(*w->sec));
    for (int n=0;n<4;n++) {
        memset(w->nb[n], 0, sizeof(*w->nb[n]));
        w->nb[n]->column[4] = chunk_alloc(0,4,0);
    }
}

static bool model_get_block(struct world_section* s, int x, int y, int z,
                            struct block_data* out) {
    struct chunk* c = s->column[y / CHUNK_SIZE];
    if (!c) return false;
    *out = chunk_get_block(c, x, y & (CHUNK_SIZE-1), z);
    return true;
}

static int model_cell_diff(struct world_section* s, int x, int y, int z,
                           uint8_t type, uint8_t sky, uint8_t torch) {
    struct block_data b;
    return !model_get_block(s, x, y, z, &b) || b.type != type
        || b.metadata != 0 || b.sky_light != sky || b.torch_light != torch;
}

// Independent semantic checks keep a shared mistake in the two fill arms from
// making the byte-equivalence comparison vacuously green.
static int verify_mixed_output(struct world* w, const uint8_t* skin_y) {
    struct far_span span;
    if (!far_span_get(skin_y, &span)) return 1;
    uint8_t open_sky = far_open_sky(w);
    int diff = 0;
    for (int cy=span.cy_lo;cy<=span.cy_hi;cy++)
        diff += w->sec->column[cy] == NULL;

    // (0,0): FAR_NO_SURFACE -> full allocated vertical span is dimension-lit air.
    diff += model_cell_diff(w->sec, 0, span.cy_lo * CHUNK_SIZE, 0,
                            BLOCK_AIR, open_sky, 0);
    diff += model_cell_diff(w->sec, 0, span.y_top, 0,
                            BLOCK_AIR, open_sky, 0);
    diff += w->sec->heightmap[0 + 0 * CHUNK_SIZE] != 0;

    // (0,1): selected lava surface is dark in stored sky/torch; renderer luminance
    // supplies its Beta full-bright minimum. All air above reaches y_top.
    diff += model_cell_diff(w->sec, 0, 31, 1, BLOCK_LAVA_STILL, 0, 0);
    diff += model_cell_diff(w->sec, 0, 32, 1, BLOCK_AIR, open_sky, 0);
    diff += model_cell_diff(w->sec, 0, span.y_top, 1,
                            BLOCK_AIR, open_sky, 0);
    diff += w->sec->heightmap[0 + 1 * CHUNK_SIZE] != 32;

    // (0,2): opaque filler stays sky=0 from global chunk_min through the surface.
    diff += model_cell_diff(w->sec, 0, span.chunk_min, 2, BLOCK_STONE, 0, 0);
    diff += model_cell_diff(w->sec, 0, 47, 2, BLOCK_STONE, 0, 0);
    diff += model_cell_diff(w->sec, 0, 48, 2, BLOCK_AIR, open_sky, 0);
    diff += model_cell_diff(w->sec, 0, span.y_top, 2,
                            BLOCK_AIR, open_sky, 0);
    diff += w->sec->heightmap[0 + 2 * CHUNK_SIZE] != 48;
    return diff;
}

static int run_equivalence(struct world* w, const uint8_t* skin_y,
                           const uint8_t* top_id, const uint8_t* gi,
                           const uint8_t* gj, enum world_dim dimension,
                           const char* label) {
    static uint8_t hm_ref[CHUNK_SIZE * CHUNK_SIZE];
    static uint8_t blocks_ref[COLUMN_HEIGHT]
        [CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*5/2];
    int present_ref[COLUMN_HEIGHT];

    w->dimension = dimension;
    reset_model(w);
    far_chunk_reference(w, skin_y, top_id, gi, gj);
    memcpy(hm_ref, w->sec->heightmap, sizeof(hm_ref));
    for (int k=0;k<COLUMN_HEIGHT;k++) {
        present_ref[k] = w->sec->column[k] != NULL;
        if (present_ref[k])
            memcpy(blocks_ref[k], w->sec->column[k]->blocks,
                   sizeof(blocks_ref[k]));
    }

    reset_model(w);
    far_chunk_direct(w, skin_y, top_id, gi, gj);

    int heightmap_diff = 0, blocks_diff = 0;
    for (int i=0;i<CHUNK_SIZE*CHUNK_SIZE;i++)
        heightmap_diff += hm_ref[i] != w->sec->heightmap[i];
    for (int k=0;k<COLUMN_HEIGHT;k++) {
        int present_direct = w->sec->column[k] != NULL;
        if (present_ref[k] != present_direct) {
            blocks_diff += 1000;
        } else if (present_direct) {
            for (size_t j=0;j<sizeof(blocks_ref[k]);j++)
                blocks_diff += blocks_ref[k][j] != w->sec->column[k]->blocks[j];
        }
    }
    int semantic_diff = verify_mixed_output(w, skin_y);
    bool ok = heightmap_diff == 0 && blocks_diff == 0 && semantic_diff == 0;
    printf("\nEQUIVALENCE %-9s heightmap_diff=%d blocks_diff=%d semantic_diff=%d %s\n",
           label, heightmap_diff, blocks_diff, semantic_diff,
           ok ? "IDENTICAL" : "DIFFER");
    return ok ? 0 : 1;
}

int main(void) {
    blocks_init();
    static struct world_section sec, nb0, nb1, nb2, nb3;
    static struct world w;
    w.sec = &sec; w.nb[0]=&nb0; w.nb[1]=&nb1; w.nb[2]=&nb2; w.nb[3]=&nb3;
    w.dimension = WORLD_DIM_OVERWORLD;
    gW = &w;

    uint8_t skin_y[256], top_id[256], gi[256], gj[256];
    for (int i=0;i<256;i++){ gi[i]=i&7; gj[i]=(i>>3)&7; }

    struct { const char* name; int y0, amp; } cases[] = {
        {"flat plains  (surface y~70, amp 1)", 70, 1},
        {"rolling hills (surface y~72, amp 6)", 72, 6},
        {"mountainous  (surface y~80, amp 18)", 80, 18},
    };

    printf("%-38s  %8s  %12s  %12s  %7s\n",
           "case", "cells", "reference(us)", "direct(us)", "speedup");

    for (size_t ci=0; ci<sizeof(cases)/sizeof(cases[0]); ci++) {
        make_column(skin_y, top_id, cases[ci].y0, cases[ci].amp, 1234+ci);
        int cells = count_cells(skin_y);

        const int ITER = 20000;
        double tc=0, td=0;

        // warm + reset between every iteration (each builds into a fresh slot)
        for (int it=0; it<ITER; it++) {
            reset_model(&w);
            double t0=now_us();
            far_chunk_reference(&w, skin_y, top_id, gi, gj);
            tc += now_us()-t0;
        }
        for (int it=0; it<ITER; it++) {
            reset_model(&w);
            double t0=now_us();
            far_chunk_direct(&w, skin_y, top_id, gi, gj);
            td += now_us()-t0;
        }
        printf("%-38s  %8d  %12.3f  %12.3f  %6.2fx\n",
               cases[ci].name, cells, tc/ITER, td/ITER, tc/td);
    }

    // Byte-exact slow-reference vs direct-fill gates for both dimension light modes.
    make_mixed_column(skin_y, top_id);
    int failed = 0;
    failed |= run_equivalence(&w, skin_y, top_id, gi, gj,
                              WORLD_DIM_OVERWORLD, "overworld");
    failed |= run_equivalence(&w, skin_y, top_id, gi, gj,
                              WORLD_DIM_NETHER, "nether");
    return failed ? 1 : 0;
}

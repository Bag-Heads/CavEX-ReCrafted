/*
  Isolate the per-cell dict_wsection_get cost that world_set_block pays on EVERY
  far cell (world.c:268) vs hoisting it once per chunk. Uses the real M*LIB dict
  with realistic far-streaming occupancy (~1000 loaded sections). The current
  far path does ~5000 dict lookups/chunk into the same bucket; a direct fill (or
  even just hoisting the section lookup) does 1.
*/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "third_party/m-lib/m-dict.h"

struct world_section { uint8_t pad[ 256*3 + 8*8 ]; int id; };
#define SECTION_TO_ID(x,z) (((int64_t)(z)<<32)|(((int64_t)(x)&0xFFFFFFFF)))
DICT_DEF2(dict_wsection, int64_t, M_BASIC_OPLIST, struct world_section, M_POD_OPLIST)

static double now_us(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1e6+ts.tv_nsec/1e3; }

int main(void){
    for (int loaded = 256; loaded <= 1600; loaded += loaded) {
        dict_wsection_t sections; dict_wsection_init(sections);
        // Populate a realistic ring of loaded sections (loaded entries).
        int R = 1; while ((2*R+1)*(2*R+1) < loaded) R++;
        int target_x=0, target_z=0; int n=0;
        for (int x=-R; x<=R && n<loaded; x++)
          for (int z=-R; z<=R && n<loaded; z++) {
            struct world_section s; s.id=n;
            dict_wsection_set_at(sections, SECTION_TO_ID(x,z), s); n++;
          }

        // CURRENT: per-cell lookup, ~5000 cells/chunk, all same section
        const int CELLS = 4500;  // representative far-chunk cell count (rolling)
        const int CHUNKS = 4000;
        volatile uint64_t sink=0;
        double t0=now_us();
        for (int ch=0; ch<CHUNKS; ch++)
            for (int c=0; c<CELLS; c++) {
                struct world_section* s = dict_wsection_get(sections, SECTION_TO_ID(target_x,target_z));
                sink += (uintptr_t)s;
            }
        double t_cur = now_us()-t0;

        // HOISTED/DIRECT: one lookup per chunk
        t0=now_us();
        for (int ch=0; ch<CHUNKS; ch++) {
            struct world_section* s = dict_wsection_get(sections, SECTION_TO_ID(target_x,target_z));
            for (int c=0; c<CELLS; c++) sink += (uintptr_t)s;
        }
        double t_hoist = now_us()-t0;

        printf("loaded=%-5d  per-chunk dict cost: current=%.3f us  hoisted=%.3f us  saved=%.3f us/chunk\n",
               n, t_cur/CHUNKS, t_hoist/CHUNKS, (t_cur-t_hoist)/CHUNKS);
        dict_wsection_clear(sections);
        (void)sink;
    }
    return 0;
}

#ifndef PISTON_RENDER_H
#define PISTON_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blocks_data.h"

struct block_info;
struct displaylist;

/* Beta RenderBlocks.func_31080_c head emitter; full_rod=false is the moving p<0.5 variant. */
size_t piston_render_head_variant(struct displaylist* d, struct block_info* info,
								  enum side side, uint8_t vertex_light[24],
								  bool full_rod, bool count_only);

#endif

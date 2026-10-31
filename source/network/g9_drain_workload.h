/* G9 diagnostic/PC bench shared edits. Frozen 1200-tick production workload. */
#ifndef G9_DRAIN_WORKLOAD_H
#define G9_DRAIN_WORKLOAD_H
#include "server_world.h"
#include "client_interface.h"
#include "../block/blocks_data.h"
#define G9_DRAIN_TICKS 1200
#define G9_DRAIN_RADIUS 3
static bool g9_drain_discard_publications; /* boot diagnostic only */
static inline void g9_drain_set(struct server_world* w, int x, int y, int z,
							  int type, int meta) {
	(void)server_world_set_block(w, x, y, z,
		(struct block_data){.type = type, .metadata = meta});
	if(g9_drain_discard_publications) clin_drain_discard();
}
static inline void g9_drain_floor(struct server_world* w, int y) {
	for(int dx = 0; dx < 16; dx++)
		for(int dz = 0; dz < 16; dz++)
			g9_drain_set(w, -16 + dx, y - 1, 16 + dz, BLOCK_STONE, 0);
}
static inline void g9_drain_edits(struct server_world* w, int y, int t) {
	if(t % 40 == 0) {
		bool on = ((t / 40) & 1) != 0;
		for(int dx = 4; dx < 12; dx++)
			for(int dz = 4; dz < 12; dz++)
				g9_drain_set(w, -16 + dx, y + 4, 16 + dz,
							 on ? BLOCK_STONE : BLOCK_AIR, 0);
	}
	if(t % 20 == 0)
		for(int k = 0; k < 16; k++) {
			bool on = ((k + t / 20) & 1) != 0;
			g9_drain_set(w, -15 + 2 * (k % 4), y, 17 + 2 * (k / 4),
						 on ? BLOCK_TORCH : BLOCK_AIR, on ? 5 : 0);
		}
}
#endif

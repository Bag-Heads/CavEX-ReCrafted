/*
	Compute-only benchmark for the client lighting flood budget.

	The far-ring freeze fix depends on LIGHTING_CLIENT_POPS_PER_FRAME being small
	enough that one render-thread lighting slice cannot approach the Wii watchdog
	threshold. This does not certify Broadway timing; it gives a repeatable
	single-threaded harness around the shipped lighting_flood_resume hot loop so
	the budget can be measured instead of hand-waved.
*/

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef PLATFORM_WII
#include <fat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#else
#include <time.h>
#endif

#include "block/blocks.h"
#include "lighting.h"

#define GRID_X 48
#define GRID_Y 96
#define GRID_Z 48
#define GRID_INDEX(x, y, z) ((x) + GRID_X * ((z) + GRID_Z * (y)))
#define BENCH_ITERS 200
#define RESULT_PATH "sd:/lighting_bench.txt"

struct block* blocks[256];

static struct block s_stone;
static struct block_data s_cells[GRID_X * GRID_Y * GRID_Z];
static uint8_t s_height[GRID_X * GRID_Z];
static uint32_t s_light_writes;

void blocks_side_offset(enum side s, int* x, int* y, int* z) {
	assert(x && y && z);
	switch(s) {
		default:
		case SIDE_TOP: *x = 0; *y = 1; *z = 0; break;
		case SIDE_BOTTOM: *x = 0; *y = -1; *z = 0; break;
		case SIDE_LEFT: *x = -1; *y = 0; *z = 0; break;
		case SIDE_RIGHT: *x = 1; *y = 0; *z = 0; break;
		case SIDE_FRONT: *x = 0; *y = 0; *z = -1; break;
		case SIDE_BACK: *x = 0; *y = 0; *z = 1; break;
	}
}

static void bench_blocks_init(void) {
	memset(blocks, 0, sizeof(blocks));
	memset(&s_stone, 0, sizeof(s_stone));
	s_stone.can_see_through = false;
	s_stone.opacity = 15;
	blocks[BLOCK_STONE] = &s_stone;
}

static void reset_grid(void) {
	memset(s_cells, 0, sizeof(s_cells));
	memset(s_height, 0, sizeof(s_height));
	s_light_writes = 0;

	for(int x = 0; x < GRID_X; x++)
		for(int z = 0; z < GRID_Z; z++)
			s_cells[GRID_INDEX(x, 0, z)].type = BLOCK_STONE;
}

static bool bench_get_block(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
							struct block_data* blk, uint8_t* height) {
	(void)user;
	if(x < 0 || y < 0 || z < 0 || x >= GRID_X || y >= GRID_Y || z >= GRID_Z)
		return false;
	if(blk)
		*blk = s_cells[GRID_INDEX(x, y, z)];
	if(height)
		*height = s_height[x + z * GRID_X];
	return true;
}

static void bench_set_light(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
							uint8_t light) {
	(void)user;
	assert(x >= 0 && y >= 0 && z >= 0 && x < GRID_X && y < GRID_Y
		   && z < GRID_Z);
	struct block_data* blk = &s_cells[GRID_INDEX(x, y, z)];
	blk->sky_light = light & 0x0F;
	blk->torch_light = light >> 4;
	s_light_writes++;
}

#ifdef PLATFORM_WII
typedef uint64_t bench_tick_t;

static bench_tick_t bench_now(void) {
	return gettime();
}

static double bench_elapsed_us(bench_tick_t start, bench_tick_t end) {
	return (double)ticks_to_microsecs(diff_ticks(start, end));
}

#ifdef CAVEWII_WII_BENCH_STANDALONE
static void bench_video_init(void) {
	GXRModeObj* rmode = NULL;
	void* framebuffer = NULL;

	VIDEO_Init();
	WPAD_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(framebuffer, 20, 20, rmode->fbWidth, rmode->xfbHeight,
				 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(framebuffer);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
}

static bool bench_fat_init(void) {
	for(int attempt = 1; attempt <= 5; ++attempt) {
		if(fatInitDefault())
			return true;
		printf("fatInitDefault failed, attempt %d/5\n", attempt);
		for(int frame = 0; frame < 60; ++frame)
			VIDEO_WaitVSync();
	}
	return false;
}

static void bench_write_result_file(const char* result) {
	if(!bench_fat_init()) {
		printf("could not mount SD; no %s written\n", RESULT_PATH);
		return;
	}

	FILE* f = fopen(RESULT_PATH, "wb");
	if(!f) {
		printf("open %s failed errno=%d\n", RESULT_PATH, errno);
		return;
	}
	fputs(result, f);
	if(fclose(f) == 0)
		printf("wrote %s\n", RESULT_PATH);
	else
		printf("close %s failed errno=%d\n", RESULT_PATH, errno);
}

static void bench_wait_exit(void) {
	printf("HOME exits\n");
	while(SYS_MainLoop()) {
		WPAD_ScanPads();
		if(WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
			break;
		VIDEO_WaitVSync();
	}
}
#endif
#else
typedef clock_t bench_tick_t;

static bench_tick_t bench_now(void) {
	return clock();
}

static double bench_elapsed_us(bench_tick_t start, bench_tick_t end) {
	return 1000000.0 * (double)(end - start) / (double)CLOCKS_PER_SEC;
}
#endif

static int bench_run(char* result, size_t result_size) {
	bench_blocks_init();
	struct stack q;
	lighting_flood_queue_create(&q);

	double total_us = 0.0;
	size_t total_pops = 0;
	uint32_t total_writes = 0;

	for(int i = 0; i < BENCH_ITERS; i++) {
		reset_grid();
		lighting_flood_queue_clear(&q);
		lighting_flood_seed(&q, GRID_X / 2, GRID_Y / 2, GRID_Z / 2, true);

		bench_tick_t start = bench_now();
		size_t pops = lighting_flood_resume(
			&q, false, LIGHTING_CLIENT_POPS_PER_FRAME, bench_get_block,
			bench_set_light, NULL);
		bench_tick_t end = bench_now();

		assert(pops == LIGHTING_CLIENT_POPS_PER_FRAME);
		assert(!lighting_flood_queue_empty(&q));
		assert(s_light_writes > 0);
		total_us += bench_elapsed_us(start, end);
		total_pops += pops;
		total_writes += s_light_writes;
	}

	lighting_flood_queue_destroy(&q);

	double avg_us = total_us / (double)BENCH_ITERS;
	double ns_per_pop = (total_us * 1000.0) / (double)total_pops;
	snprintf(result, result_size,
			 "bench_lighting_budget: budget=%u iters=%d avg_slice_us=%.2f "
			 "ns_per_pop=%.2f avg_light_writes=%.1f grid=%dx%dx%d\n",
			 (unsigned)LIGHTING_CLIENT_POPS_PER_FRAME, BENCH_ITERS, avg_us,
			 ns_per_pop, (double)total_writes / (double)BENCH_ITERS, GRID_X,
			 GRID_Y, GRID_Z);
	printf("%s", result);
	return 0;
}

int main(void) {
	char result[512];

#if defined(PLATFORM_WII) && defined(CAVEWII_WII_BENCH_STANDALONE)
	bench_video_init();
	printf("CaveWii lighting budget bench\n");
#endif

	int rc = bench_run(result, sizeof(result));

#if defined(PLATFORM_WII) && defined(CAVEWII_WII_BENCH_STANDALONE)
	bench_write_result_file(result);
	bench_wait_exit();
#endif

	return rc;
}

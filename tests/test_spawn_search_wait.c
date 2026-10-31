/* hw1030n: the new-world spawn search must keep the run's telemetry
 * heartbeat alive. A real-Wii far_walk_6m on seed 22 (a long Beta walk) sat
 * in spawn_search_wait_idle for > 5 s with no heartbeat and the collector
 * called it a hang. This drives the real spawn_search_wait.c against a fake
 * clock through a 60 s fake search and proves:
 *   - the registered heartbeat service is offered at least every 250 ms;
 *   - a main-style 1 s cadence on top of it never leaves a gap over 1.25 s;
 *   - the screen shows the probe count once there is one, and the old text
 *     before (and with no hooks registered at all). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/gui/spawn_search_wait.h"
#include "platform/time.h"

/* ---- fake platform ------------------------------------------------------ */
static int64_t g_now_ms;
static char g_last_text[128];
static int g_presents;

ptime_t time_get(void) {
	ptime_t t;
	t.tv_sec = (time_t)(g_now_ms / 1000);
	t.tv_nsec = (long)((g_now_ms % 1000) * 1000000);
	return t;
}
int32_t time_diff_ms(ptime_t f, ptime_t s) {
	return (int32_t)((s.tv_sec - f.tv_sec) * 1000
		+ (s.tv_nsec - f.tv_nsec) / 1000000);
}
void thread_msleep(size_t ms) { g_now_ms += (int64_t)ms; }
void watchdog_heartbeat(void) {}
int gfx_width(void) { return 640; }
int gfx_height(void) { return 480; }
int gutil_font_width(const char* str, int scale) {
	return (int)strlen(str) * scale;
}
void gutil_text(int x, int y, const char* str, int scale, bool shadow) {
	(void)x; (void)y; (void)scale; (void)shadow;
	snprintf(g_last_text, sizeof g_last_text, "%s", str);
}
/* Headless shape: paint, then report "nothing presented" so the idle sleeps
 * its 1 ms (the fake clock's only advance besides the frame cost below). */
int loading_notice_present_next(void (*paint)(void* user), void* user) {
	paint(user);
	g_presents++;
	g_now_ms += 15; /* a presented frame costs real time on the console */
	return -1;
}

/* ---- fake main ---------------------------------------------------------- */
static int64_t g_offers, g_offer_gap_max, g_last_offer = -1;
static int64_t g_beats, g_beat_gap_max, g_last_beat = -1;
static uint32_t g_probes;
static bool g_searching;

static void fake_heartbeat_service(void) {
	if(g_last_offer >= 0 && g_now_ms - g_last_offer > g_offer_gap_max)
		g_offer_gap_max = g_now_ms - g_last_offer;
	g_last_offer = g_now_ms;
	g_offers++;
	/* main.c wait_heartbeat_service: its own 1 s cadence. */
	if(g_last_beat >= 0 && g_now_ms - g_last_beat < 1000)
		return;
	if(g_last_beat >= 0 && g_now_ms - g_last_beat > g_beat_gap_max)
		g_beat_gap_max = g_now_ms - g_last_beat;
	g_last_beat = g_now_ms;
	g_beats++;
}
static bool fake_progress(uint32_t* probes) {
	if(!g_searching)
		return false;
	*probes = g_probes;
	return true;
}

int main(void) {
	/* No hooks: the old screen text, and nothing else is called. */
	spawn_search_wait_idle(NULL);
	assert(!strcmp(g_last_text, "Finding a spawn point..."));

	spawn_search_wait_set_hooks(fake_heartbeat_service, fake_progress);
	g_searching = true;
	g_probes = 0;
	g_now_ms += 1000;
	spawn_search_wait_idle(NULL);
	assert(!strcmp(g_last_text, "Finding a spawn point..."));

	/* A 60 s search that loads a chunk every 150 ms. */
	const int64_t start = g_now_ms;
	while(g_now_ms - start < 60000) {
		g_probes = (uint32_t)((g_now_ms - start) / 150);
		spawn_search_wait_idle(NULL);
	}
	printf("offers=%lld (max gap %lld ms) beats=%lld (max gap %lld ms) "
		   "presents=%d text=\"%s\"\n", (long long)g_offers,
		(long long)g_offer_gap_max, (long long)g_beats,
		(long long)g_beat_gap_max, g_presents, g_last_text);
	assert(g_offer_gap_max <= 250 + 16 && "heartbeat offered every 250 ms");
	assert(g_beats >= 55 && "about one heartbeat per second for 60 s");
	assert(g_beat_gap_max <= 1250 && "never a heartbeat gap the collector reads as stale");
	char want[64];
	snprintf(want, sizeof want, "Finding a spawn point... (%u chunks)",
		(unsigned)g_probes);
	assert(!strcmp(g_last_text, want) && "the probe count is on screen");

	/* Search over (progress inactive): back to the plain text. */
	g_searching = false;
	g_now_ms += 100;
	spawn_search_wait_idle(NULL);
	assert(!strcmp(g_last_text, "Finding a spawn point..."));
	printf("test_spawn_search_wait: PASS\n");
	return 0;
}

/* Copyright (c) 2026 CaveWii_v2 contributors
 * Slice 1b bounded recorder/hash assertion. This is NOT a live-walk replayer.
 * Schema coverage and missing nondeterminism are listed in the slice report.
 */
#include "support/split_residency_fixture.h"
#include "platform/audio.h"
#include "tps_observer.h"
#include <inttypes.h>
#include <math.h>

#define REPLAY_TICKS 24
struct input {
	double x, y, z;
	unsigned count, state_first;
};
struct record { struct input input; uint64_t hash; };
static uint64_t hash;

static void bytes(const void* data, size_t size) {
	const uint8_t* p = data;
	while(size--) { hash ^= *p++; hash *= UINT64_C(1099511628211); }
}
#define FIELD(v) bytes(&(v), sizeof(v))

/* Field-wise: no pointers, struct padding, allocator slack or wall clocks.
 * Dictionary traversal order is deliberately covered (it can affect updates).
 * This is a declared partial state schema, not a whole-server-state claim. */
static uint64_t state_hash(struct server_local* s) {
	hash = UINT64_C(14695981039346656037);
	FIELD(s->world_time); FIELD(s->world.current_tick);
	FIELD(s->rand_src.seed); FIELD(s->rand_src.has_next_gaussian);
	FIELD(s->rand_src.next_gaussian); FIELD(s->world.hell_populate_rng.seed);
	FIELD(s->player.x); FIELD(s->player.y); FIELD(s->player.z);
	FIELD(s->player.health); FIELD(s->player.oxygen);
	FIELD(s->player.finished_loading); FIELD(s->player.correction_seq);
	FIELD(s->anchor_sep); FIELD(s->near_send_seq);
	if(s->residency) {
		FIELD(s->residency->epoch);
		FIELD(s->residency->next_near); FIELD(s->residency->next_far);
		for(unsigned b = 0; b < RESIDENCY_BUCKETS; b++)
			for(unsigned a = 0; a < RESIDENCY_ANCHORS; a++) {
				const struct residency_debt* d = &s->residency->debt[b][a];
				FIELD(d->used);
				if(!d->used) continue;
				FIELD(d->x); FIELD(d->z); FIELD(d->first_wait);
				FIELD(d->attempted); FIELD(d->retry); FIELD(d->near_owners);
				FIELD(d->far_owners); FIELD(d->attempted_once);
			}
	}
	uint64_t count = dict_server_chunks_size(s->world.chunks);
	FIELD(count);
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, s->world.chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it)) {
		const struct server_chunk* c = &dict_server_chunks_ref(it)->value;
		FIELD(dict_server_chunks_ref(it)->key);
		bytes(c->ids, 32768); bytes(c->metadata, 16384);
		bytes(c->lighting_sky, 16384); bytes(c->lighting_torch, 16384);
		bytes(c->heightmap, 256);
		FIELD(c->modified); FIELD(c->populated); FIELD(c->sent);
		FIELD(c->persisted_tile_entities_complete); FIELD(c->persisted_entities_nonempty);
		FIELD(c->load_ticks_rearmed); FIELD(c->load_tick_rearm_cursor);
		FIELD(c->near_send_fail_streak); FIELD(c->near_send_fail_seq);
	}
	count = s->world.scheduled_tick_count; FIELD(count);
	for(size_t i = 0; i < s->world.scheduled_tick_count; i++) {
		const struct scheduled_tick* t = &s->world.scheduled_ticks[
			(s->world.scheduled_tick_head + i) % s->world.scheduled_tick_capacity];
		FIELD(t->x); FIELD(t->y); FIELD(t->z); FIELD(t->fire_at_tick);
		FIELD(t->block_type); FIELD(t->queue_flags);
	}
	return hash;
}

static void drain(const struct client_rpc* rpc, void* unused) {
	(void)rpc; (void)unused;
	clin_drain_discard();
}

static void dispatch(struct server_local* s, const struct input* input) {
	struct server_rpc pos = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = input->x, .y = input->y, .z = input->z,
			.on_ground = true}};
	struct server_rpc state = {.type = SRPC_PLAYER_STATE,
		.payload.player_state = {.on_ground = true}};
	/* The tape records consumed RPC count and order, including zero-input ticks.
	 * Dispatch through the actual server handler, not a replacement simulation. */
	if(input->count == 2 && input->state_first) server_local_test_process_rpc(s, &state);
	if(input->count) server_local_test_process_rpc(s, &pos);
	if(input->count == 2 && !input->state_first) server_local_test_process_rpc(s, &state);
}

static bool run(struct record records[REPLAY_TICKS], int sep, bool compare,
	bool corrupt_state) {
	struct server_local* s = split_fixture();
	s->player.finished_loading = true;
	s->render_demo_mob = -1;
	s->rand_src = (struct random_gen){.seed = 12345};
	*server_local_sound_rng() = (struct random_gen){.seed = 6789};
	*audio_cosmetic_rng() = (struct random_gen){.seed = 9876};
	server_local_test_seed_math_random(7654);
	s->far_view_distance = MAX_VIEW_DISTANCE;
	assert(server_local_set_anchor_sep(s, sep));
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	assert(split_populate(s, 0, 0));
	clin_test_set_rpc_observer(drain, NULL);
	bool ok = true;
	for(unsigned tick = 0; tick < REPLAY_TICKS; tick++) {
		struct input* input = &records[tick].input;
		if(!compare) *input = (struct input){.x = (tick / 8) * 16 + 0.5,
			.y = 4.0 + PLAYER_EYE_HEIGHT, .z = 0.5,
			.count = tick % 3, .state_first = tick % 2};
		dispatch(s, input);
		if(input->count) assert(s->player.x == input->x);
		server_local_test_update_once(s);
		if(corrupt_state && tick == 5) {
			dict_server_chunks_it_t it;
			dict_server_chunks_it(it, s->world.chunks);
			assert(!dict_server_chunks_end_p(it));
			dict_server_chunks_ref(it)->value.lighting_sky[0] ^= 1;
		}
		uint64_t actual = state_hash(s);
		if(compare && actual != records[tick].hash) {
			fprintf(stderr, "replay divergence tick=%u expected=%016" PRIx64
				" actual=%016" PRIx64 "\n", tick, records[tick].hash, actual);
			ok = false; break;
		}
		if(!compare) records[tick].hash = actual;
	}
	clin_drain_discard();
	clin_test_set_rpc_observer(NULL, NULL);
	split_destroy(s);
	return ok;
}

static bool tape(const char* path, struct record* records, int sep, bool read) {
	FILE* f = fopen(path, read ? "rb" : "wb");
	if(!f) return false;
	char line[256];
	char header[128];
	snprintf(header, sizeof(header), "CWREPLAY1 partial-state sep=%d ticks=%d abi=%zu\n",
		sep, REPLAY_TICKS, sizeof(size_t));
	bool ok = true;
	if(read) ok = fgets(line, sizeof(line), f) && !strcmp(line, header);
	else ok = fputs(header, f) != EOF;
	for(unsigned tick = 0; ok && tick < REPLAY_TICKS; tick++) {
		struct record* r = &records[tick];
		if(read) {
			unsigned index; int used = 0;
			ok = fgets(line, sizeof(line), f) && sscanf(line,
				"%u %lf %lf %lf %u %u %" SCNx64 "%n", &index,
				&r->input.x, &r->input.y, &r->input.z, &r->input.count,
				&r->input.state_first, &r->hash, &used) == 7 && index == tick
				&& !strcmp(line + used, "\n") && r->input.count <= 2
				&& r->input.state_first <= 1 && isfinite(r->input.x)
				&& isfinite(r->input.y) && isfinite(r->input.z);
		} else ok = fprintf(f, "%u %.17g %.17g %.17g %u %u %016" PRIx64 "\n",
			tick, r->input.x, r->input.y, r->input.z, r->input.count,
			r->input.state_first, r->hash) > 0;
	}
	if(read && fgetc(f) != EOF) ok = false;
	if(ferror(f)) ok = false;
	if(fclose(f)) ok = false;
	return ok;
}

int main(int argc, char** argv) {
	assert(clin_init()); blocks_init();
	struct record records[REPLAY_TICKS];
	if(argc == 4) {
		int sep = atoi(argv[3]);
		if(sep != 96 && sep != 256) return 2;
		if(!strcmp(argv[1], "--record"))
			return run(records, sep, false, false) && tape(argv[2], records, sep, false) ? 0 : 1;
		if(!strcmp(argv[1], "--assert")) {
			char prefix[1024];
			int n = snprintf(prefix, sizeof(prefix), "%s.cpu_", argv[2]);
			if(n < 0 || (size_t)n >= sizeof(prefix)
			   || !tape(argv[2], records, sep, true) || !tps_observer_start(prefix)) return 1;
			tps_observer_frame(true);
			bool ok = run(records, sep, true, false);
			tps_observer_frame(false);
			if(!tps_observer_stop()) ok = false;
			return ok ? 0 : 1;
		}
		return 2;
	}
	char dir[256], path[300], prefix[300];
	test_fixture_dir("test_server_replay_record", dir, sizeof(dir));
#ifdef _WIN32
	assert(_mkdir(dir) == 0);
#else
	assert(mkdir(dir, 0777) == 0);
#endif
	snprintf(path, sizeof(path), "%s/tape.txt", dir);
	snprintf(prefix, sizeof(prefix), "%s/cpu_", dir);
	for(unsigned i = 0; i < 2; i++) {
		int sep = i ? 256 : 96;
		assert(run(records, sep, false, false));
		assert(tape(path, records, sep, false));
		memset(records, 0, sizeof(records));
		assert(tape(path, records, sep, true));
		assert(tps_observer_start(prefix));
		tps_observer_frame(true);
		assert(run(records, sep, true, false));
		tps_observer_frame(false);
		assert(tps_observer_stop());
		assert(!run(records, sep, true, true));
		double original_x = records[1].input.x;
		records[1].input.x += 16;
		assert(!run(records, sep, true, false));
		records[1].input.x = original_x;
		records[0].hash ^= 1;
		assert(!run(records, sep, true, false));
		assert(!tape(path, records, sep == 96 ? 256 : 96, true));
		FILE* f = fopen(path, "ab"); assert(f);
		assert(fputs("trailing\n", f) != EOF && fclose(f) == 0);
		assert(!tape(path, records, sep, true));
		f = fopen(path, "wb"); assert(f);
		assert(fputs("CWREPLAY1", f) != EOF && fclose(f) == 0);
		assert(!tape(path, records, sep, true));
	}
	test_fixture_dir_cleanup(dir);
	puts("PASS recorder + per-update partial-state hash assertion at 96/256; "
		"CPU observer armed; light/hash/header/trailing corruption rejected");
	return 0;
}

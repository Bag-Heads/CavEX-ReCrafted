/* ci-1101/04: production retention, without a pump between server packets.
 * Beta server PlayerManager.java:121-147 loads a new coordinate BEFORE
 * unloading its translated old coordinate; PlayerInstance.java:62-70 sends
 * Packet50 immediately. Radius 15 is legal (PlayerManager constructor).
 * Reuse the admission suite's production helpers and Wii-sized backing. */
#define MP_ADMISSION_HELPERS_ONLY
#include "test_mp_admission.c"

static bool burst_load(int x, int z) {
	bool ok = remote_pre_chunk_locked(s_r, x, z, true);
	if(!ok)
		fprintf(stderr, "FAIL load (%d,%d), entries=%zu: %s\n",
			x, z, s_r->retained_entries, s_r->reason);
	return ok && !s_r->stop_now;
}

static bool burst_grid(int radius, int centre) {
	for(int x = centre - radius; x <= centre + radius; x++)
		for(int z = centre - radius; z <= centre + radius; z++)
			if(!burst_load(x, z))
				return false;
	return true;
}

static int burst_move(int radius, int delta, bool respawn) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	server_remote_set_admit_radius(&adm_owner, radius);
	if(!burst_grid(radius, 0))
		return 1;
	/* Publish all 441 old columns through the real promotion path. PC memory
	 * permits this maximum; the Wii admission radius remains unchanged. */
	for(int x = -radius; x <= radius; x++)
		for(int z = -radius; z <= radius; z++)
			adm_retain_partial(x, z, 128, BLOCK_STONE);
	adm_pump_n(441 * 5);
	assert(s_r->resident_cols == 441);
	if(respawn) {
		/* NetClientHandler.func_9448_a preserves the world for an unchanged
		 * dimension. ServerConfigurationManager.recreatePlayerEntity removes
		 * old memberships, sends Packet9, then adds the new spawn view. */
		thread_mutex_lock(&s_r->lock);
		for(int x = -radius; x <= radius; x++)
			for(int z = -radius; z <= radius; z++)
				assert(remote_pre_chunk_locked(s_r, x, z, false));
		thread_mutex_unlock(&s_r->lock);
		struct remote_event e = {.kind = REMOTE_EVENT_PACKET,
			.generation = s_r->generation, .world_epoch = s_r->world_epoch,
			.packet = {.type = MP_EVENT_RESPAWN,
				.payload.respawn_dimension = WORLD_DIM_OVERWORLD}};
		assert(process_event(&adm_owner, &e));
	}

	/* Exact PlayerManager loop order for a diagonal 20-column move. No
	 * frame/promotion/unload emit occurs in this loop. */
	thread_mutex_lock(&s_r->lock);
	bool ok = true;
	for(int x = delta - radius; ok && x <= delta + radius; x++)
		for(int z = delta - radius; ok && z <= delta + radius; z++) {
			if(respawn || x > radius || z > radius || x < -radius || z < -radius)
				ok = burst_load(x, z);
			int ox = x - delta, oz = z - delta;
			if(!respawn && (ox < delta - radius || oz < delta - radius
						   || ox > delta + radius || oz > delta + radius))
				ok = ok && remote_pre_chunk_locked(s_r, ox, oz, false);
		}
	thread_mutex_unlock(&s_r->lock);
	if(!ok)
		return 1;
	server_remote_pump(&adm_owner); /* one frame retires installed old entries */
	size_t expected = (size_t)(2 * radius + 1) * (2 * radius + 1);
	assert(!s_r->stop_now && s_r->retained_entries == expected);
	for(int x = delta - radius; x <= delta + radius; x++)
		for(int z = delta - radius; z <= delta + radius; z++)
			assert(adm_entry(x, z) != REMOTE_CHUNK_ENTRY_CAPACITY);
	printf("PASS %zu-column %s burst: session alive, one frame leaves %zu entries\n",
		expected, respawn ? "same-dimension respawn" : "teleport", expected);
	return 0;
}

static int burst_login_and_overlap(void) {
	adm_fresh_session(ADM_WII_BACKING);
	if(!burst_grid(15, 0))
		return 1;
	assert(s_r->retained_entries == 961 && !s_r->stop_now);
	/* The entire next view can precede retirement of the first view. This
	 * also covers ordinary-queue/chunk-queue scheduling skew. */
	if(!burst_grid(15, 100))
		return 1;
	assert(s_r->retained_entries == 1922 && !s_r->stop_now);
	for(int x = -15; x <= 15; x++)
		for(int z = -15; z <= 15; z++)
			assert(remote_pre_chunk_locked(s_r, x, z, false));
	assert(s_r->retained_entries == 961
		&& "never-installed unloads retire before the frame boundary");
	server_remote_pump(&adm_owner);
	assert(s_r->retained_entries == 961 && !s_r->stop_now);
	printf("PASS radius-15 login + overlapping views: peak 1922, live 961\n");
	return 0;
}

/* PlayerManager.func_543_c loads each new coordinate before unloading its
 * translated old one (server decompile, lines 129-147). Two disjoint moves
 * can arrive before the main thread gets even one frame. */
static bool burst_far_teleport(int from, int to) {
	bool ok = true;
	thread_mutex_lock(&s_r->lock);
	for(int x = -15; ok && x <= 15; x++)
		for(int z = -15; ok && z <= 15; z++)
			ok = burst_load(to + x, to + z)
				&& remote_pre_chunk_locked(s_r, from + x, from + z, false);
	thread_mutex_unlock(&s_r->lock);
	return ok;
}

static void burst_assert_final_view(int centre) {
	assert(!s_r->stop_now && s_r->retained_entries == 961);
	for(int x = centre - 15; x <= centre + 15; x++)
		for(int z = centre - 15; z <= centre + 15; z++)
			assert(adm_entry(x, z) != REMOTE_CHUNK_ENTRY_CAPACITY);
}

static int burst_double_teleport(void) {
	adm_fresh_session(ADM_WII_BACKING);
	if(!burst_grid(15, 0))
		return 1;
	/* Retained bodies are reader-owned until publication; unloading also
	 * releases this head and partial chain, without a client-world RPC. */
	adm_retain_partial(0, 0, 128, BLOCK_STONE);
	adm_retain_partial(0, 0, 8, BLOCK_DIRT);
	assert(s_r->retained_bytes > 0);
	if(!burst_far_teleport(0, 100)
	   || !burst_far_teleport(100, 200))
		return 1;
	/* No pump, including between the two teleports. */
	burst_assert_final_view(200);
	assert(s_r->retained_bytes == 0 && s_r->resident_cols == 0);
	printf("PASS two radius-15 teleports without a pump: alive, live 961\n");
	return 0;
}

static int burst_recovery_teleport(void) {
	adm_fresh_session(ADM_WII_BACKING);
	adm_anchor(0, 0);
	if(!burst_grid(15, 0))
		return 1;
	/* One installed column proves the real client gate refuses retirement;
	 * the other 960 must not wait for that unrelated main-thread work. */
	adm_retain_partial(0, 0, 128, BLOCK_STONE);
	adm_pump_n(8);
	assert(adm_installed(0, 0) && s_r->resident_cols == 1);
	clin_test_arm_stream_reset();
	if(!burst_far_teleport(0, 100))
		return 1;
	assert(!s_r->stop_now && s_r->retained_entries == 962
		   && "only the installed column waits for stream recovery");
	thread_mutex_lock(&s_r->lock);
	remote_emit_pending_unloads_locked(s_r);
	thread_mutex_unlock(&s_r->lock);
	assert(adm_installed(0, 0) && s_r->unload_refused_retries == 1);
	assert(!s_r->stop_now && s_r->retained_entries == 962
		   && "only the installed column waits for stream recovery");
	/* A second pump still cannot retire the installed column. */
	server_remote_pump(&adm_owner);
	assert(s_r->retained_entries == 962 && s_r->resident_cols == 1);
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 42);
	server_remote_pump(&adm_owner);
	burst_assert_final_view(100);
	assert(s_r->resident_cols == 0);
	printf("PASS radius-15 teleport during stream recovery: installed unload "
		   "retried, never-installed entries retired, final live 961\n");
	return 0;
}

int main(int argc, char** argv) {
	assert(svin_init() && clin_init());
	blocks_init();
	items_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	assert(server_remote_init(&adm_owner));
	s_r = adm_owner.impl;
	int rc = 0;
	if(argc == 1 || strcmp(argv[1], "teleport") == 0)
		rc |= burst_move(10, 20, false);
	if(argc == 1 || strcmp(argv[1], "login") == 0)
		rc |= burst_login_and_overlap();
	if(argc == 1 || strcmp(argv[1], "respawn") == 0)
		rc |= burst_move(10, 100, true);
	if(argc > 1 && strcmp(argv[1], "double-teleport") == 0)
		rc |= burst_double_teleport();
	if(argc > 1 && strcmp(argv[1], "recovery-teleport") == 0)
		rc |= burst_recovery_teleport();
	server_remote_destroy(&adm_owner);
	return rc;
}

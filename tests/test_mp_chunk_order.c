/* Packet51 / Packet52-53 ordering in the online client's chunk retention
 * (source/network/server_remote_chunks.c). Permanent regressions for the three
 * defects the 2026-09-10 multiplayer audit reproduced with the production
 * objects, plus the ordering cases the fix has to hold. Beta reference:
 * NetClientHandler.handleMapChunk -> World.setChunkData applies each packet's
 * explicit region IN ORDER; Packet52/53 likewise in order [740c583].
 *
 * The harness (init, helpers) is the existing S0 suite with its main renamed,
 * exactly like the audit's tmp/mp_audit_repro.c fixture. */
#define main existing_mp_suite_main
#include "test_mp_s0_env.c"
#undef main

static struct server_remote_impl* s_r;

/* One Packet51 for the column at block (x, 0..sy, 0) filled with `id`,
 * retained through the production path. sy < 128 = a partial sub-volume. */
static void retain_at(int x, int y, int sy, int id) {
	size_t cells = 16u * (size_t)sy * 16u, bytes = cells * 5u / 2u;
	unsigned char* raw = calloc(1, bytes);
	assert(raw);
	memset(raw, id, cells);
	uLongf n = compressBound(bytes);
	unsigned char* compressed = malloc(n);
	assert(compressed && compress(compressed, &n, raw, bytes) == Z_OK);
	free(raw);
	struct mp_event e = {.type = MP_EVENT_MAP_CHUNK};
	e.payload.map_chunk = (struct mp_map_chunk) {
		.x = x, .y = (int16_t)y, .z = 0, .sx = 16, .sy = (uint16_t)sy, .sz = 16,
		.compressed = compressed, .compressed_length = n,
		.inflated_length = bytes};
	assert(remote_retain_map_locked(s_r, &e));
}
static void retain(int x, int sy, int id) { retain_at(x, 0, sy, id); }
static void promote(void) {
	for(int i = 0; i < 40; i++)
		assert(remote_promotion_step_locked(s_r));
}
/* Emulate the pump: each edit is the next received packet (struct
 * remote_event::seq), stamped from the same counter as the retained bodies. */
static void edit(int x, int y, int z, int id) {
	s_r->pump_packet_seq = ++s_r->arrival_sequence;
	s_r->pump_packet_sub = 0;
	remote_apply_block_change(s_r, x, (uint8_t)y, z, (uint8_t)id, 0);
}
static int block_at(int x, int y, int z) {
	return world_get_block(&gstate.world, x, y, z).type;
}

/* Audit D1: a partial Packet51 must not discard the earlier full column. */
static void test_full_then_partial(void) {
	retain(0, 128, BLOCK_STONE);
	retain(0, 8, BLOCK_DIRT); /* y 0..7 only */
	promote();
	assert(world_find_chunk(&gstate.world, 0, 0, 0));
	assert(block_at(0, 4, 0) == BLOCK_DIRT && "the partial applies");
	assert(block_at(0, 12, 0) == BLOCK_STONE
		   && "the full column outside the partial survives");
	assert(block_at(0, 100, 0) == BLOCK_STONE);
	printf("PASS full then partial: both regions applied in order\n");
}

/* Audit D2: an edit older than a newer snapshot must not overwrite it. */
static void test_old_edit_then_new_snapshot(void) {
	retain(32, 128, BLOCK_STONE);
	edit(32, 10, 0, BLOCK_DIRT);
	retain(32, 128, BLOCK_STONE);
	promote();
	assert(block_at(32, 10, 0) == BLOCK_STONE
		   && "the newer snapshot post-dates the edit");
	printf("PASS old edit then new snapshot: snapshot wins\n");
}

/* Audit D3: 257 distinct deferred coordinates must not lose the first. */
static void test_deferred_overflow(void) {
	retain(64, 128, BLOCK_STONE);
	for(int i = 0; i < 257; i++)
		edit(64 + i / 128, i % 128, 0, BLOCK_DIRT);
	promote();
	assert(!s_r->stop_now && "257 distinct edits fit the 1024 log");
	assert(s_r->deferred_edits_dropped == 0 && "nothing may be dropped");
	for(int i = 0; i < 257; i++)
		assert(block_at(64 + i / 128, i % 128, 0) == BLOCK_DIRT
			   && "every one of the 257 edits applied (index 256 = (66,0,0))");
	printf("PASS deferred overflow: every edit applied, none dropped\n");
}

/* Ordering: an edit that arrives AFTER the snapshot, inside its volume, still
 * applies on top of it (the deferred path's whole purpose). */
static void test_new_edit_after_snapshot(void) {
	retain(96, 128, BLOCK_STONE);
	edit(96, 10, 0, BLOCK_DIRT);
	promote();
	assert(block_at(96, 10, 0) == BLOCK_DIRT);
	assert(block_at(96, 11, 0) == BLOCK_STONE);
	printf("PASS edit after snapshot: edit wins\n");
}

/* Ordering with a chain: full, edit inside the later partial's volume, then
 * the partial (newer than the edit) -> partial wins there; an edit outside
 * the partial's volume, older than the partial, still applies (nothing
 * newer covers it). */
static void test_edit_between_full_and_partial(void) {
	retain(128, 128, BLOCK_STONE);
	edit(128, 4, 0, BLOCK_DIRT);   /* inside */
	edit(128, 50, 0, BLOCK_DIRT);  /* outside */
	retain(128, 8, BLOCK_SAND); /* y 0..7, newer than both edits */
	promote();
	assert(block_at(128, 4, 0) == BLOCK_SAND && "partial post-dates the edit");
	assert(block_at(128, 50, 0) == BLOCK_DIRT && "uncovered edit survives");
	assert(block_at(128, 60, 0) == BLOCK_STONE);
	printf("PASS edit between full and partial: order preserved\n");
}

/* Two disjoint partials on a column the client already holds: both apply. */
static void test_resident_then_two_partials(void) {
	retain(160, 128, BLOCK_STONE);
	promote();
	assert(block_at(160, 5, 0) == BLOCK_STONE);
	retain_at(160, 0, 8, BLOCK_DIRT);   /* y 0..7 */
	retain_at(160, 64, 8, BLOCK_SAND);  /* y 64..71 */
	edit(160, 66, 0, BLOCK_DIRT); /* after sand */
	promote();
	assert(block_at(160, 5, 0) == BLOCK_DIRT);
	assert(block_at(160, 70, 0) == BLOCK_SAND);
	assert(block_at(160, 66, 0) == BLOCK_DIRT && "edit after the partial wins");
	assert(block_at(160, 100, 0) == BLOCK_STONE);
	printf("PASS resident column, two disjoint partials + later edit\n");
}

/* A newer FULL column supersedes an older queued partial entirely (coalesce). */
static void test_partial_then_full_replaces(void) {
	retain_at(192, 0, 8, BLOCK_DIRT);
	retain(192, 128, BLOCK_STONE);
	promote();
	assert(block_at(192, 4, 0) == BLOCK_STONE && "later full supersedes");
	printf("PASS partial then full: later full column wins\n");
}


/* A partial arriving while the full column is MID-DECODE: the in-flight decode
 * finishes and installs, then the partial applies on top (no discard). */
static void test_partial_during_promotion(void) {
	retain(224, 128, BLOCK_STONE);
	assert(remote_promotion_step_locked(s_r)); /* start: PROMOTING */
	assert(remote_promotion_step_locked(s_r)); /* a slice of the inflate */
	retain(224, 8, BLOCK_DIRT); /* parks behind the in-flight full */
	promote();
	assert(block_at(224, 4, 0) == BLOCK_DIRT);
	assert(block_at(224, 40, 0) == BLOCK_STONE
		   && "the in-flight full column was installed, not discarded");
	printf("PASS partial during promotion: full installed, partial on top\n");
}

/* Overlapping / nested / equal partials on one waiting column apply in
 * order; an equal-or-covering later body coalesces the older one. */
static void test_overlapping_partials(void) {
	retain(256, 128, BLOCK_STONE);
	retain_at(256, 0, 16, BLOCK_DIRT);   /* y 0..15 */
	retain_at(256, 8, 16, BLOCK_SAND);   /* y 8..23, overlaps */
	retain_at(256, 8, 4, BLOCK_GRAVEL);  /* y 8..11, nested inside sand */
	promote();
	assert(block_at(256, 2, 0) == BLOCK_DIRT);
	assert(block_at(256, 9, 0) == BLOCK_GRAVEL && "last writer of y 8..11");
	assert(block_at(256, 14, 0) == BLOCK_SAND && "sand outside the gravel");
	assert(block_at(256, 20, 0) == BLOCK_SAND);
	assert(block_at(256, 30, 0) == BLOCK_STONE);
	/* equal volume later: coalesces the earlier one, same result */
	retain_at(256, 0, 4, BLOCK_DIRT);
	retain_at(256, 0, 4, BLOCK_SAND);
	promote();
	assert(block_at(256, 1, 0) == BLOCK_SAND);
	printf("PASS overlapping/nested/equal partials: in order, coalesced\n");
}

/* IN-009b: set when a rewritten check is RED; main exits 1 after every other
 * check has run with its current behaviour. */
static int s_in009b_red;

/* Like retain_at, but a refusal is reported instead of asserted: the body is
 * freed (ownership stayed with the caller) and false is returned. */
static bool try_retain_at(int x, int y, int sy, int id) {
	size_t cells = 16u * (size_t)sy * 16u, bytes = cells * 5u / 2u;
	unsigned char* raw = calloc(1, bytes);
	assert(raw);
	memset(raw, id, cells);
	uLongf n = compressBound(bytes);
	unsigned char* compressed = malloc(n);
	assert(compressed && compress(compressed, &n, raw, bytes) == Z_OK);
	free(raw);
	struct mp_event e = {.type = MP_EVENT_MAP_CHUNK};
	e.payload.map_chunk = (struct mp_map_chunk) {
		.x = x, .y = (int16_t)y, .z = 0, .sx = 16, .sy = (uint16_t)sy, .sz = 16,
		.compressed = compressed, .compressed_length = n,
		.inflated_length = bytes};
	if(remote_retain_map_locked(s_r, &e))
		return true;
	free(e.payload.map_chunk.compressed);
	return false;
}

/* IN-009b (replaces the fifth-body refusal check): Beta applies EVERY
 * Packet51 region to a held chunk, in arrival order, with no per-column
 * limit -- client CFR NetClientHandler.handleMapChunk -> World.setChunkData
 * -> Chunk.setChunkData (tmp/jobs/loop/IN-009-astra-plan.md step 3), and the
 * server's PlayerInstance (server CFR PlayerInstance.java:90-153) sends any
 * number of bounding-box partials per column. A retained column given a head
 * plus seven non-covering partials must accept every body without a
 * reconnect, a chain rejection or a reason, and must promote with all eight
 * regions applied (lossless folding). */
static void test_chain_folds_losslessly(void) {
	static const struct {
		int y, sy, id;
	} partials[] = {
		{0, 2, BLOCK_DIRT},	   {10, 2, BLOCK_SAND},	 {20, 2, BLOCK_GRAVEL},
		{30, 2, BLOCK_DIRT},	   {40, 2, BLOCK_SAND},	 {50, 2, BLOCK_COBBLESTONE},
		{60, 2, BLOCK_GRAVEL}};
	const size_t count = sizeof(partials) / sizeof(partials[0]);
	uint32_t reconnects = s_r->forced_reconnects;
	uint32_t rejections = s_r->chain_rejections;
	retain(288, 128, BLOCK_STONE); /* head */
	size_t refused_at = 0;
	for(size_t i = 0; i < count; i++)
		if(!try_retain_at(288, partials[i].y, partials[i].sy, partials[i].id)
		   && !refused_at)
			refused_at = i + 2; /* body number, the head is body 1 */
	promote();
	promote(); /* room for a longer chain than the old four-body cap */
	bool applied = true;
	size_t first_missing = 0;
	for(size_t i = 0; i < count && applied; i++)
		if(block_at(288, partials[i].y + 1, 0) != partials[i].id) {
			applied = false;
			first_missing = i + 2;
		}
	bool clean = s_r->forced_reconnects == reconnects
		&& s_r->chain_rejections == rejections;
	bool head_kept = block_at(288, 5, 0) == BLOCK_STONE
		&& block_at(288, 100, 0) == BLOCK_STONE;
	if(refused_at || !applied || !clean || !head_kept) {
		printf("IN-009b RED: chain fold: head + %zu non-covering partials on one "
			   "retained column: body %zu refused, first unapplied body %zu, "
			   "forced_reconnects +%u, chain_rejections +%u, head outside the "
			   "partials %s, reason=\"%s\"\n",
			   count, refused_at, first_missing,
			   (unsigned)(s_r->forced_reconnects - reconnects),
			   (unsigned)(s_r->chain_rejections - rejections),
			   head_kept ? "kept" : "LOST", s_r->reason);
		s_in009b_red++;
		return;
	}
	printf("PASS chain fold: head + %zu partials, every body accepted and "
		   "applied in order, no reconnect\n", count);
}

/* GENUINE backing-capacity exhaustion still ends the session visibly (its
 * own connection state: the terminal kills it). The retained-byte cap is
 * the production lever server_remote_test_set_retained_byte_cap exists to
 * move; with nothing reclaimable the reclaim ladder must end the session
 * through remote_backing_capacity_fail_locked: refused, stop_now, mp_end =
 * SERVER_REMOTE_END_CAPACITY, and a nonempty reason. */
static void test_capacity_exhaustion_ends_session(void) {
	struct server_remote cap_owner = {0};
	assert(server_remote_init(&cap_owner));
	struct server_remote_impl* saved = s_r;
	s_r = cap_owner.impl;
	retain(1024, 128, BLOCK_STONE); /* head, fits */
	server_remote_test_set_retained_byte_cap(&cap_owner, s_r->retained_bytes);
	bool accepted = try_retain_at(1024, 0, 2, BLOCK_DIRT);
	assert(!accepted && "a body past the backing capacity is refused");
	assert(s_r->stop_now && "the session ends");
	assert(s_r->mp_end == SERVER_REMOTE_END_CAPACITY && "mp_end latched");
	assert(s_r->reason[0] && "with a nonempty reason");
	printf("PASS capacity exhaustion: refused, mp_end=%u, reason \"%s\"\n",
		   (unsigned)s_r->mp_end, s_r->reason);
	s_r = saved;
	server_remote_destroy(&cap_owner);
}

/* Unload with a chain waiting: every body and every parked edit for the
 * column goes (d92cbfe6 purge), and the reload starts clean. */
static void test_unload_drops_chain(void) {
	retain(320, 128, BLOCK_STONE);
	retain_at(320, 0, 4, BLOCK_DIRT);
	edit(320, 50, 0, BLOCK_DIRT);
	size_t bytes_before = s_r->retained_bytes;
	assert(bytes_before > 0);
	assert(remote_pre_chunk_locked(s_r, 20, 0, false)); /* unload chunk (20,0) */
	assert(s_r->retained_bytes < bytes_before && "chain bytes released");
	assert(s_r->deferred_edits == 0 && "parked edit purged with the column");
	promote();
	assert(!world_find_chunk(&gstate.world, 320, 0, 0) || block_at(320, 50, 0) != BLOCK_DIRT);
	printf("PASS unload: chain + parked edits released\n");
}



/* Astra mp2 P1: an edit received BEFORE the column's last load/unload cycle
 * (its receive stamp is older than the residency stamp) is from a dead
 * residency: dropped, never parked. A column with no entry at all: same. */
static void test_stale_residency_edit_dropped(void) {
	uint32_t dropped = s_r->deferred_edits_dropped;
	uint32_t parked = s_r->deferred_edits;
	/* no entry for column (30,0) at all */
	edit(480, 10, 0, BLOCK_DIRT);
	assert(s_r->deferred_edits == parked && s_r->deferred_edits_dropped == dropped + 1);
	/* announce + load the column (stamps the residency), then an edit whose
	 * stamp is OLDER than that load */
	assert(remote_pre_chunk_locked(s_r, 30, 0, true));
	uint64_t saved = s_r->arrival_sequence;
	s_r->pump_packet_seq = 1; /* received long before the load */
	s_r->pump_packet_sub = 0;
	remote_apply_block_change(s_r, 480, 11, 0, BLOCK_DIRT, 0);
	s_r->arrival_sequence = saved;
	assert(s_r->deferred_edits == parked && s_r->deferred_edits_dropped == dropped + 2);
	/* a genuinely newer edit for the announced column parks as before */
	edit(480, 12, 0, BLOCK_DIRT);
	assert(s_r->deferred_edits == parked + 1);
	printf("PASS stale-residency edit dropped, live one parked\n");
}

/* Beta can let a block change overtake the Packet50 that announces its
 * column (separate server queues). The edit is dropped (no storage yet); the
 * full that follows the load carries the server state including it. */
static void test_edit_overtakes_load_then_full(void) {
	uint32_t dropped = s_r->deferred_edits_dropped;
	uint32_t reconnects = s_r->forced_reconnects; /* the chain-cap test above counted one */
	edit(544, 10, 0, BLOCK_DIRT);              /* column (34,0): no entry yet */
	assert(s_r->deferred_edits_dropped == dropped + 1);
	assert(remote_pre_chunk_locked(s_r, 34, 0, true));
	retain(544, 128, BLOCK_STONE);             /* the server state, post-edit */
	promote();
	assert(block_at(544, 10, 0) == BLOCK_STONE && "the full column wins");
	assert(!s_r->stop_now && s_r->forced_reconnects == reconnects);
	printf("PASS edit overtaking its load: dropped, the full column installs\n");
}

/* Astra mp2 P2: an incoming edit a newer retained body already covers takes
 * no slot (superseded on arrival) -- so it can never be the edit that trips
 * exhaustion. */
static void test_incoming_edit_superseded_by_queued_body(void) {
	uint32_t superseded = s_r->deferred_edits_superseded;
	uint32_t parked = s_r->deferred_edits;
	assert(remote_pre_chunk_locked(s_r, 31, 0, true));
	uint64_t older_seq = ++s_r->arrival_sequence; /* the edit's packet */
	retain(496, 128, BLOCK_STONE);                 /* body received after it */
	s_r->pump_packet_seq = older_seq;              /* pump applies it late */
	s_r->pump_packet_sub = 0;
	remote_apply_block_change(s_r, 496, 5, 0, BLOCK_DIRT, 0);
	assert(s_r->deferred_edits == parked && "no slot taken");
	assert(s_r->deferred_edits_superseded == superseded + 1);
	promote();
	assert(block_at(496, 5, 0) == BLOCK_STONE && "the newer snapshot wins");
	printf("PASS incoming edit superseded by a newer body: no slot, snapshot wins\n");
}

/* Astra mp3: P2 at a SATURATED log. Fill every slot, then an incoming edit
 * inside a queued newer body takes no slot and does not terminate; one
 * outside any body is the honest termination. Runs on its own connection
 * state (the log is per connection), so it is placed after the gate test
 * and before the exhaustion test, sharing s_r. */
static void test_saturated_log_inside_vs_outside(void) {
	/* REL-MP-FIX2b: a full log of edits on 8 BODYLESS announced columns (40..47) no
	 * longer end the session -- the exhaustion rescue folds one column onto
	 * Beta's pre-chunk, losslessly. */
	for(int k = 0; k < 8; k++)
		assert(remote_pre_chunk_locked(s_r, 40 + k, 0, true));
	uint32_t parked0 = s_r->deferred_edits, rescues = s_r->deferred_log_rescues;
	uint32_t dropped = s_r->deferred_edits_dropped;
	/* REL-MP-JOINSPEED: the log is REMOTE_DEFERRED_EDIT_CAPACITY (16,384)
	 * now; 8 columns x 16 x 128 distinct cells (z 0) fill it exactly. */
	for(int i = 0; i < REMOTE_DEFERRED_EDIT_CAPACITY - (int)parked0 && !s_r->stop_now;
		i++)
		edit(640 + (i / 2048) * 16 + (i / 128) % 16, i % 128, 0, BLOCK_DIRT);
	assert(!s_r->stop_now && s_r->deferred_edits == REMOTE_DEFERRED_EDIT_CAPACITY
		   && "log full");
	edit(640, 0, 1, BLOCK_DIRT);
	assert(!s_r->stop_now && s_r->deferred_log_rescues == rescues + 1
		   && s_r->deferred_edits_dropped == dropped
		   && s_r->deferred_edits < REMOTE_DEFERRED_EDIT_CAPACITY
		   && "bodyless columns are rescued, nothing dropped");
	/* A saturation that truly cannot compact: every column in the table
	 * (40..47 and earlier cases' leftovers) unloaded and reloaded before the
	 * pump (Packet50 0 then 1). Their edits park for the reload, but the
	 * unload is still pending, so no compaction may fold them into a
	 * residency that is about to be torn down. */
	for(size_t c = 0; c < REMOTE_CHUNK_ENTRY_CAPACITY; c++) {
		if(s_r->chunks[c].state == REMOTE_CHUNK_UNUSED)
			continue;
		int32_t cx = s_r->chunks[c].chunk_x, cz = s_r->chunks[c].chunk_z;
		assert(remote_pre_chunk_locked(s_r, cx, cz, false));
		assert(remote_pre_chunk_locked(s_r, cx, cz, true));
	}
	parked0 = s_r->deferred_edits;
	for(int i = 0; i < REMOTE_DEFERRED_EDIT_CAPACITY - (int)parked0 && !s_r->stop_now;
		i++)
		edit(640 + (i / 2048) * 16 + (i / 128) % 16, i % 128, 2, BLOCK_DIRT);
	assert(!s_r->stop_now && s_r->deferred_edits == REMOTE_DEFERRED_EDIT_CAPACITY
		   && "log full again");
	/* a column with a full head and a queued partial B (y 8..15) */
	assert(remote_pre_chunk_locked(s_r, 48, 0, true));
	uint64_t e_seq = ++s_r->arrival_sequence;      /* E received first... */
	retain(768, 128, BLOCK_STONE);
	retain_at(768, 8, 8, BLOCK_SAND);              /* ...then head and B */
	uint32_t superseded = s_r->deferred_edits_superseded;
	s_r->pump_packet_seq = e_seq; s_r->pump_packet_sub = 0;
	remote_apply_block_change(s_r, 768, 10, 0, BLOCK_DIRT, 0); /* inside B */
	assert(!s_r->stop_now && "covered edit must not terminate");
	assert(s_r->deferred_edits_superseded == superseded + 1);
	assert(s_r->deferred_edits == REMOTE_DEFERRED_EDIT_CAPACITY && "no slot taken");
	/* the same, outside every body: nothing supersedes it -> termination */
	uint64_t seq2 = ++s_r->arrival_sequence;
	s_r->pump_packet_seq = seq2; s_r->pump_packet_sub = 0;
	remote_apply_block_change(s_r, 800, 10, 0, BLOCK_DIRT, 0); /* column 50 */
	/* column 50 has no entry: dropped (not exhaustion) -- announce one */
	assert(!s_r->stop_now);
	assert(remote_pre_chunk_locked(s_r, 50, 0, true));
	edit(800, 11, 0, BLOCK_DIRT);
	assert(s_r->stop_now && s_r->deferred_edit_log_exhausted == 1);
	assert(strstr(s_r->reason, "deferred edit log exhausted"));
	printf("PASS saturated log: 8 bodyless columns rescued losslessly; on an "
		   "uncompactable log a covered edit needs no slot, an uncovered one "
		   "terminates\n");
}

/* Publication gate: a finished decode is HELD while an event received before
 * the body is still queued; released once the queue moves past it. */
static void test_publication_gate_holds_for_older_event(void) {
	assert(remote_pre_chunk_locked(s_r, 32, 0, true));
	uint64_t older_seq = ++s_r->arrival_sequence; /* an edit still in the queue */
	retain(512, 128, BLOCK_STONE);
	/* emulate the queued older event (the pump has not reached it) */
	assert(s_r->event_count == 0);
	memset(&s_r->events[s_r->event_head], 0, sizeof(s_r->events[0]));
	s_r->events[s_r->event_head].kind = REMOTE_EVENT_PACKET;
	s_r->events[s_r->event_head].seq = older_seq;
	s_r->event_count = 1;
	uint32_t waits = s_r->promotion_publish_waits;
	promote();
	assert(!world_find_chunk(&gstate.world, 512, 0, 0) && "not published");
	assert(s_r->promotion_publish_waits > waits && "the gate held");
	s_r->event_count = 0; /* the pump consumed it */
	promote();
	assert(block_at(512, 3, 0) == BLOCK_STONE && "published once the queue passed");
	printf("PASS publication gate: held for an older queued event, then released\n");
}


/* ---- Real wire: receive thread vs pump (Astra mp2: required before merge) --
 * A loopback peer sends, in this order, for three columns:
 *   column 0: announce, full A (stone), Packet53 edit E at (5,10,5) dirt,
 *             partial B (y 8..15, sand)  -> E is INSIDE B and older: superseded
 *   column 1: announce, full A2, edit E2 at (21,10,5), partial B2 (y 64..71)
 *             -> E2 is OUTSIDE B2: applied
 *   column 2: announce, full A3, edit E3 at (37,10,5), then UNLOAD (2,0)
 *             -> the reader runs the unload before the pump reaches E3: dropped
 * The test does not pump until the peer has sent everything, so the stamps
 * are the PRODUCTION ones (read thread) and the pump applies every edit late. */
struct order_wire_fixture {
	SOCKET listener;
	bool ok;
	atomic_bool go;        /* test -> peer: login pumped, send burst 1 */
	atomic_bool sent_all;  /* peer -> test: burst 1 is on the wire */
	atomic_bool go2;       /* test -> peer: phase 1 pumped, send burst 2 */
	atomic_bool sent_all2; /* peer -> test: burst 2 is on the wire */
};

static bool wire_chunk(SOCKET peer, int32_t x, int16_t y, int32_t z,
					   uint8_t sx, uint8_t sy, uint8_t sz, uint8_t id) {
	size_t cells = (size_t)sx * sy * sz, nibble = cells / 2u;
	size_t total = cells + 3u * nibble;
	uint8_t* inflated = calloc(1, total);
	if(!inflated) return false;
	memset(inflated, id, cells);
	uLongf cap = compressBound((uLong)total);
	uint8_t* compressed = malloc(cap);
	if(!compressed || compress2(compressed, &cap, inflated, (uLong)total,
								Z_BEST_SPEED) != Z_OK) {
		free(inflated); free(compressed); return false;
	}
	free(inflated);
	size_t len = 18u + (size_t)cap;
	uint8_t* packet = malloc(len);
	if(!packet) { free(compressed); return false; }
	packet[0] = 0x33; put_i32(packet + 1, x); put_i16(packet + 5, y);
	put_i32(packet + 7, z); packet[11] = (uint8_t)(sx - 1);
	packet[12] = (uint8_t)(sy - 1); packet[13] = (uint8_t)(sz - 1);
	put_i32(packet + 14, (int32_t)cap);
	memcpy(packet + 18, compressed, cap);
	free(compressed);
	bool ok = socket_write_fragmented(peer, packet, len);
	free(packet);
	return ok;
}
static bool wire_edit(SOCKET peer, int32_t x, uint8_t y, int32_t z, uint8_t id) {
	uint8_t body[12]; size_t o = 0;
	body[o++] = 0x35; put_i32(body + o, x); o += 4; body[o++] = y;
	put_i32(body + o, z); o += 4; body[o++] = id; body[o++] = 0;
	return socket_write_fragmented(peer, body, sizeof(body));
}
static bool wire_pre(SOCKET peer, int32_t cx, int32_t cz, uint8_t mode) {
	uint8_t body[10]; body[0] = 0x32; put_i32(body + 1, cx); put_i32(body + 5, cz);
	body[9] = mode;
	return socket_write_fragmented(peer, body, sizeof(body));
}
static bool wire_wait(atomic_bool* flag) {
	for(int i = 0; i < 10000; i++) {
		if(atomic_load(flag)) return true;
		thread_msleep(1);
	}
	return false;
}

static void* order_wire_peer_entry(void* user) {
	struct order_wire_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET) return NULL;
	if(!peer_login(peer)) goto done;
	static const uint8_t login_response[] = {0x01,0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	static const uint8_t spawn[] = {0x06,0,0,0,0, 0,0,0,100, 0,0,0,0};
	uint8_t correction[42] = {0x0D};
	put_f64(correction + 1, 0.5); put_f64(correction + 9, 101.62);
	put_f64(correction + 17, 103.24); put_f64(correction + 25, 0.5);
	put_f32(correction + 33, 90.0f); put_f32(correction + 37, 0.0f);
	correction[41] = 0;
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response))
	   || !socket_write_fragmented(peer, spawn, sizeof(spawn))
	   || !wire_pre(peer, 0, 0, 1) || !wire_pre(peer, 1, 0, 1)
	   || !wire_pre(peer, 2, 0, 1)
	   || !socket_write_fragmented(peer, correction, sizeof(correction)))
		goto done;
	/* Burst 1 only after the test has pumped the login and HELD the pump. */
	if(!wire_wait(&fixture->go)) goto done;
	if(!wire_chunk(peer, 0, 0, 0, 16, 128, 16, BLOCK_STONE)) goto done;
	if(!wire_edit(peer, 5, 10, 5, BLOCK_DIRT)) goto done;
	if(!wire_chunk(peer, 0, 8, 0, 16, 8, 16, BLOCK_SAND)) goto done;
	if(!wire_chunk(peer, 16, 0, 0, 16, 128, 16, BLOCK_STONE)) goto done;
	if(!wire_edit(peer, 21, 10, 5, BLOCK_DIRT)) goto done;
	if(!wire_chunk(peer, 16, 64, 0, 16, 8, 16, BLOCK_SAND)) goto done;
	if(!wire_chunk(peer, 32, 0, 0, 16, 128, 16, BLOCK_STONE)) goto done;
	if(!wire_edit(peer, 37, 10, 5, BLOCK_DIRT)) goto done;
	if(!wire_pre(peer, 2, 0, 0)) goto done;
	atomic_store(&fixture->sent_all, true);
	/* Burst 2 (Astra mp3): column 1 unload -> load -> full -> partial B ->
	 * edit E AFTER B inside its volume, pump held throughout. */
	if(!wire_wait(&fixture->go2)) goto done;
	if(!wire_pre(peer, 1, 0, 0)) goto done;
	if(!wire_pre(peer, 1, 0, 1)) goto done;
	if(!wire_chunk(peer, 16, 0, 0, 16, 128, 16, BLOCK_STONE)) goto done;
	if(!wire_chunk(peer, 16, 8, 0, 16, 8, 16, BLOCK_SAND)) goto done;
	if(!wire_edit(peer, 21, 10, 5, BLOCK_DIRT)) goto done;
	atomic_store(&fixture->sent_all2, true);
	uint8_t drain;
	while(socket_read_exact(peer, &drain, 1)) {
	}
	fixture->ok = true;
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static size_t wire_entry_locked(struct server_remote_impl* r, int32_t cx,
								int32_t cz) {
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++)
		if(r->chunks[i].state != REMOTE_CHUNK_UNUSED && r->chunks[i].chunk_x == cx
		   && r->chunks[i].chunk_z == cz)
			return i;
	return REMOTE_CHUNK_ENTRY_CAPACITY;
}
static uint64_t wire_queued_edit_seq_locked(struct server_remote_impl* r,
											int32_t x) {
	for(size_t k = 0; k < r->event_count; k++) {
		const struct remote_event* ev
			= &r->events[(r->event_head + k) % REMOTE_QUEUE_CAPACITY];
		if(ev->kind == REMOTE_EVENT_PACKET
		   && ev->packet.type == MP_EVENT_BLOCK_CHANGE
		   && ev->packet.payload.block_change.x == x)
			return ev->seq;
	}
	return 0;
}
/* Bounded wait on a locked predicate: the reader has taken the whole burst. */
static bool wire_wait_locked(struct server_remote_impl* r,
							 bool (*pred)(struct server_remote_impl*)) {
	for(int i = 0; i < 10000; i++) {
		thread_mutex_lock(&r->lock);
		bool ok = pred(r);
		thread_mutex_unlock(&r->lock);
		if(ok) return true;
		thread_msleep(1);
	}
	return false;
}
static bool burst1_received(struct server_remote_impl* r) {
	/* the LAST packet of burst 1 is the unload of column 2 */
	size_t i2 = wire_entry_locked(r, 2, 0);
	return i2 != REMOTE_CHUNK_ENTRY_CAPACITY && r->chunks[i2].unload_pending
		&& wire_queued_edit_seq_locked(r, 37) != 0;
}
static bool burst2_received(struct server_remote_impl* r) {
	/* the LAST packet of burst 2 is the edit at x=21 */
	return wire_queued_edit_seq_locked(r, 21) != 0;
}

/* The whole login group is PUMPED, not merely received. burst_logged_in
 * returns once Packet1 is applied, but the peer writes Packet6, the three
 * Packet50s and the Packet13 byte at a time after it; any of them still in
 * the event queue carries a seq older than A, so the publication gate would
 * (correctly) hold A behind it and the "A installed" check below would fail
 * on scheduling luck. Packet13 is the peer's LAST login packet and the pump
 * counts it in corrections_received, so once it has been applied and the
 * queue is empty, nothing older than burst 1 can be waiting. */
static bool order_login_drained(const struct server_remote_status* status) {
	return burst_logged_in(status) && status->corrections_received >= 1
		&& status->queued_events == 0;
}

static void test_wire_order_receive_vs_pump(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct order_wire_fixture peer = {.listener = make_listener(&port)};
	atomic_init(&peer.go, false); atomic_init(&peer.sent_all, false);
	atomic_init(&peer.go2, false); atomic_init(&peer.sent_all2, false);
	struct thread peer_thread;
	assert(thread_create(&peer_thread, order_wire_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	struct server_remote_impl* r = remote.impl;
	assert(wait_for(&remote, order_login_drained, 5000)
		   && "login pumped, Packet13 applied, event queue empty");
	/* ---- phase 1: HOLD the pump; the READ thread takes the whole burst. */
	atomic_store(&peer.go, true);
	assert(wire_wait(&peer.sent_all));
	assert(wire_wait_locked(r, burst1_received) && "reader took burst 1");
	thread_mutex_lock(&r->lock);
	size_t i0 = wire_entry_locked(r, 0, 0);
	assert(i0 != REMOTE_CHUNK_ENTRY_CAPACITY);
	assert(r->chunks[i0].state == REMOTE_CHUNK_RETAINED
		   && r->chunks[i0].queued_count == 1 && "A head, B queued behind it");
	uint64_t e_seq = wire_queued_edit_seq_locked(r, 5);
	assert(e_seq && "E is still queued, un-pumped");
	assert(r->chunks[i0].arrival < (e_seq << 16)
		   && (e_seq << 16) < r->chunks[i0].queued_arrival[0]
		   && "wire order A < E < B survives the reader/pump split");
	/* PUBLICATION GATE with the REAL queued E: advance the production
	 * promotion under the lock. A (older than E) installs; B (newer than E)
	 * decodes and is HELD, because E still sits in the queue. */
	uint32_t waits = r->promotion_publish_waits;
	for(int i = 0; i < 60; i++)
		assert(remote_promotion_step_locked(r));
	assert(r->promotion.active && r->promotion.ready_to_publish
		   && r->promotion.entry == i0 && "B decoded but not published");
	assert(r->promotion_publish_waits > waits && "the gate held for E");
	thread_mutex_unlock(&r->lock);
	clin_update();
	assert(world_find_chunk(&gstate.world, 0, 0, 0) != NULL
		   && world_get_block(&gstate.world, 5, 12, 5).type == BLOCK_STONE
		   && "A installed, B still held");
	/* resume the real pump: E is superseded by B, then B publishes */
	bool done = false;
	for(int elapsed = 0; elapsed < 10000 && !done; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		done = world_find_chunk(&gstate.world, 16, 0, 0) != NULL
			&& world_get_block(&gstate.world, 21, 10, 5).type == BLOCK_DIRT
			&& world_get_block(&gstate.world, 20, 66, 0).type == BLOCK_SAND
			&& world_get_block(&gstate.world, 5, 12, 5).type == BLOCK_SAND;
		thread_msleep(1);
	}
	assert(done && "both columns published with their partials");
	assert(world_get_block(&gstate.world, 5, 10, 5).type == BLOCK_SAND
		   && "E (older) inside B: the snapshot wins");
	assert(world_get_block(&gstate.world, 0, 0, 0).type == BLOCK_STONE
		   && "A outside B survives");
	assert(world_get_block(&gstate.world, 21, 10, 5).type == BLOCK_DIRT
		   && "E2 outside B2 applied");
	assert(world_get_block(&gstate.world, 20, 100, 0).type == BLOCK_STONE);
	thread_mutex_lock(&r->lock);
	assert(r->deferred_edits_superseded >= 1 && "E superseded, no slot");
	assert(r->deferred_edits_applied >= 1 && "E2 applied from the log");
	assert(r->deferred_edits_dropped >= 1 && "E3 from the unloaded residency dropped");
	assert(r->deferred_edits == 0 && "nothing stale left parked");
	assert(!r->stop_now && r->forced_reconnects == 0);
	thread_mutex_unlock(&r->lock);
	/* ---- phase 2: unload -> load -> full -> partial B -> E AFTER B. */
	atomic_store(&peer.go2, true);
	assert(wire_wait(&peer.sent_all2));
	assert(wire_wait_locked(r, burst2_received) && "reader took burst 2");
	thread_mutex_lock(&r->lock);
	size_t i1 = wire_entry_locked(r, 1, 0);
	assert(i1 != REMOTE_CHUNK_ENTRY_CAPACITY);
	assert(r->chunks[i1].unload_pending && r->chunks[i1].reload_after_unload
		   && "reload preserved through the held pump");
	assert(r->chunks[i1].state == REMOTE_CHUNK_RETAINED
		   && r->chunks[i1].queued_count == 1 && "full head, B queued");
	uint64_t e4_seq = wire_queued_edit_seq_locked(r, 21);
	assert(e4_seq && (e4_seq << 16) > r->chunks[i1].queued_arrival[0]
		   && (e4_seq << 16) > r->chunks[i1].residency_seq
		   && "E4 is newer than the reload and than B");
	thread_mutex_unlock(&r->lock);
	done = false;
	for(int elapsed = 0; elapsed < 10000 && !done; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		done = world_get_block(&gstate.world, 21, 10, 5).type == BLOCK_DIRT
			&& world_get_block(&gstate.world, 20, 12, 5).type == BLOCK_SAND;
		thread_msleep(1);
	}
	assert(done && "E4 wins over B (newer), B elsewhere, after the reload");
	thread_mutex_lock(&r->lock);
	assert(r->deferred_edits == 0 && !r->stop_now && r->forced_reconnects == 0);
	i1 = wire_entry_locked(r, 1, 0);
	assert(i1 != REMOTE_CHUNK_ENTRY_CAPACITY
		   && r->chunks[i1].state == REMOTE_CHUNK_RESIDENT
		   && !r->chunks[i1].unload_pending && "reload completed cleanly");
	thread_mutex_unlock(&r->lock);
	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
	printf("PASS wire order: reader stamps + held pump = Beta order; gate held for a real queued edit; reload + edit-after-partial\n");
}

int main(void) {
	assert(svin_init() && clin_init());
	blocks_init();
	items_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	struct server_remote owner = {0};
	assert(server_remote_init(&owner));
	s_r = owner.impl;
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 42);

	test_full_then_partial();
	test_old_edit_then_new_snapshot();
	test_deferred_overflow();
	test_new_edit_after_snapshot();
	test_edit_between_full_and_partial();
	test_resident_then_two_partials();
	test_partial_then_full_replaces();
	test_partial_during_promotion();
	test_overlapping_partials();
	test_chain_folds_losslessly();
	test_unload_drops_chain();
	test_stale_residency_edit_dropped();
	test_incoming_edit_superseded_by_queued_body();
	test_publication_gate_holds_for_older_event();
	test_edit_overtakes_load_then_full();
	test_saturated_log_inside_vs_outside(); /* last: it terminates the connection */
	test_wire_order_receive_vs_pump(); /* its own connection */
	test_capacity_exhaustion_ends_session(); /* its own connection */

	server_remote_destroy(&owner);
	if(s_in009b_red) {
		printf("IN-009b RED: test_mp_chunk_order: %d check(s) RED; every other "
			   "check passed\n", s_in009b_red);
		return 1;
	}
	printf("PASS test_mp_chunk_order\n");
	return 0;
}

/* IN-008: replaying the recorded real-Beta walk stream must keep the remote
 * session alive.
 *
 * Cause under test (tmp/jobs/loop/IN-007-cause.md): a column outside the
 * admission radius is never promoted (remote_start_promotion_locked's radius
 * gate, source/network/server_remote_chunks.c:1236-1238), so every later
 * Packet51 for it is chained behind its retained head. Beta's server sends any
 * number of partial Packet51 bodies per column (PlayerInstance.java:90-153 in
 * the CFR b1.7.3 server decompile, one per tick with >= 10 distinct updates),
 * and the directional coalescing in remote_chunk_append_body_locked
 * (server_remote_chunks.c:290-316) merges none of the fixture's bodies. The
 * fifth body for column (12,-9) -- the Packet51 at stream offset 336,517,
 * x=200 y=10 z=-144 3x4x6 -- exceeds head + REMOTE_CHUNK_QUEUE_MAX (3), and
 * remote_retain_map_locked counts a forced reconnect (server_remote_chunks.c:781),
 * sets "remote Packet51 body chain exhausted" (:783) and the read loop ends the
 * session (server_remote_transport.c:1145). Beta has no chunk re-request, so
 * killing the session is never faithful.
 *
 * Fixture: tests/fixtures/mp_walk/stream.bin, the inbound byte stream of a real
 * Poseidon b1.7.3 session (view-distance 8) recorded by
 * tests/fixtures/mp_walk/run_capture.ps1 (see its README). It is replayed
 * byte-exact through a loopback peer into the PRODUCTION transport (read
 * thread, decoder, retain/chain path) and pump, with the Wii default
 * admission radius 4 (SERVER_REMOTE_ADMIT_RADIUS_DEFAULT on PLATFORM_WII,
 * source/network/server_remote.h:88), the same harness style as
 * tests/test_mp_chunk_order.c's wire test.
 *
 * Packet attribution: the peer sends the stream in batches that each end on a
 * Packet51 and waits until the client has received that batch's last byte
 * before sending the next. A Packet51 refusal stops the read thread before it
 * calls recv() again, so the client's final rx_bytes is exactly the end
 * offset of the refused packet; the packet index reported is the one whose
 * end offset equals (or, for any other kind of ending, last precedes) that
 * byte count.
 *
 * Pass: every byte delivered and pumped, forced_reconnects == 0, the session
 * still in PLAY, not dying, mp_end == 0 and no reason. Fail: prints "IN-008
 * RED: forced_reconnects=<n> reason=<text> at packet <i>" and exits 1.
 *
 * REL-MP (tests/fixtures/mp_walk_fresh, tmp/jobs/loop/REL-MP-DIAG/DIAG.md):
 * fresh Beta terrain floods Packet52/53 edits onto chained backed columns and
 * the deferred-edit log ends the session. The separate "REL-MP RED:" marker
 * is printed ONLY for that terminal state -- mp_end == 6 and the exact reason
 * "remote deferred edit log exhausted" -- so any other ending (setup error,
 * timeout, crash, a different reason) can never be read as that RED. */
#define main existing_mp_suite_main
#include "test_mp_s0_env.c"
#undef main

#define WALK_ADMIT_RADIUS 4
#define WALK_TIMEOUT_MS 120000

struct walk_packet {
	size_t start, end; /* [start, end) in stream.bin */
	enum mp_event_type type;
};

struct walk_stream {
	uint8_t* bytes;
	size_t length;
	struct walk_packet* packets;
	size_t count;
	size_t handshake_end; /* end of the server's Packet2 reply */
};

struct walk_peer {
	SOCKET listener;
	const struct walk_stream* stream;
	struct server_remote_impl* impl; /* rx_bytes is read lock-free (atomic) */
	atomic_bool impl_ready;
	atomic_bool stop;      /* test -> peer: finish */
	atomic_bool sent_all;  /* peer -> test: every byte is on the wire */
	atomic_bool aborted;   /* peer -> test: a send failed (client closed) */
	bool login_ok;
};

static bool walk_load(const char* path, struct walk_stream* s) {
	memset(s, 0, sizeof(*s));
	FILE* f = fopen(path, "rb");
	if(!f) {
		fprintf(stderr, "IN-008 SETUP: cannot open %s\n", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(size <= 0) {
		fclose(f);
		return false;
	}
	s->length = (size_t)size;
	s->bytes = malloc(s->length);
	bool ok = s->bytes && fread(s->bytes, 1, s->length, f) == s->length;
	fclose(f);
	if(!ok)
		return false;
	/* Split into packets with the production decoder (the client direction). */
	s->packets = calloc(s->length, sizeof(*s->packets));
	if(!s->packets)
		return false;
	struct mp_decoder decoder;
	mp_decoder_init(&decoder);
	size_t start = 0;
	for(size_t i = 0; i < s->length; i++) {
		struct mp_event event;
		enum mp_decode_result r = mp_decoder_feed(&decoder, s->bytes[i], &event);
		if(r == MP_DECODE_ERROR) {
			fprintf(stderr, "IN-008 SETUP: stream does not decode at byte %zu\n", i);
			mp_decoder_destroy(&decoder);
			return false;
		}
		if(r == MP_DECODE_EVENT) {
			s->packets[s->count++] = (struct walk_packet) {
				.start = start, .end = i + 1, .type = event.type};
			mp_event_destroy(&event);
			start = i + 1;
		}
	}
	mp_decoder_destroy(&decoder);
	if(start != s->length || s->count < 2) {
		fprintf(stderr, "IN-008 SETUP: stream ends mid-packet (%zu of %zu)\n",
				start, s->length);
		return false;
	}
	/* The stream opens with the server's offline handshake reply, which
	 * peer_login sends itself (byte-identical: 02 00 01 00 2D). */
	static const uint8_t offline_reply[] = {0x02, 0x00, 0x01, 0x00, '-'};
	if(s->packets[0].end != sizeof(offline_reply)
	   || memcmp(s->bytes, offline_reply, sizeof(offline_reply)) != 0) {
		fprintf(stderr, "IN-008 SETUP: stream does not start with the offline handshake reply\n");
		return false;
	}
	s->handshake_end = s->packets[0].end;
	return true;
}

static uint32_t walk_rx(struct server_remote_impl* impl) {
	return atomic_load_explicit(&impl->rx_bytes, memory_order_relaxed);
}

static bool walk_send_all(SOCKET peer, const uint8_t* data, size_t length) {
	while(length) {
		int chunk = length > 16384 ? 16384 : (int)length;
#ifdef MSG_NOSIGNAL
		int n = send(peer, (const char*)data, chunk, MSG_NOSIGNAL);
#else
		int n = send(peer, (const char*)data, chunk, 0);
#endif
		if(n <= 0)
			return false;
		data += n;
		length -= (size_t)n;
	}
	return true;
}

static void* walk_peer_entry(void* user) {
	struct walk_peer* p = user;
	const struct walk_stream* s = p->stream;
	SOCKET peer = accept(p->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		goto out;
	if(!peer_login(peer))
		goto done;
	p->login_ok = true;
	while(!atomic_load(&p->impl_ready))
		thread_msleep(1);
	size_t sent = s->handshake_end;
	for(size_t i = 1; i < s->count && !atomic_load(&p->stop); i++) {
		bool last = i + 1 == s->count;
		if(s->packets[i].type != MP_EVENT_MAP_CHUNK && !last)
			continue;
		/* Batch [sent, end of packet i): it ends on a Packet51 (or the end). */
		size_t end = s->packets[i].end;
		if(!walk_send_all(peer, s->bytes + sent, end - sent)) {
			atomic_store(&p->aborted, true);
			goto drain;
		}
		sent = end;
		/* Wait until the client has received the whole batch, so a refusal
		 * of this Packet51 leaves rx_bytes at exactly its end offset. */
		for(int spin = 0; walk_rx(p->impl) < end && !atomic_load(&p->stop); spin++) {
			if(spin > 64)
				thread_msleep(1);
		}
	}
	if(sent == s->length)
		atomic_store(&p->sent_all, true);
drain:
	/* Hold the connection open (a server that keeps the session) and drain
	 * the client's outbound bytes until the test is done or the client
	 * closes its end. */
	fault_peer_recv_timeout(peer, 50);
	while(!atomic_load(&p->stop)) {
		uint8_t buffer[512];
		int n = recv(peer, (char*)buffer, sizeof(buffer), 0);
		if(n == 0)
			break;
	}
done:
	closesocket(peer);
out:
	closesocket(p->listener);
	return NULL;
}

/* REL-MP-FIX3e (tmp/jobs/triage-1020/walk-diag.md section 1): the writer must
 * pop and send while impl->lock is held elsewhere (chunk compaction holds it
 * for whole inflate/deflate passes). Beta's NetworkManager guards its send
 * queue with its own sendQueueLock, never the world (NetworkManager.java:61,
 * writer thread 100-140). Producers also take impl->lock, so frames are
 * injected under it and the lock is then HELD while the peer must see every
 * frame, in order. Prints "REL-MP-FIX3e RED:" and fails otherwise. */
#define FIX3E_FRAMES 16
#define FIX3E_HOLD_MS 1500
struct fix3e_peer {
	SOCKET listener;
	atomic_bool stop;
	atomic_size_t length;
	uint8_t bytes[16384];
};

static void* fix3e_peer_entry(void* user) {
	struct fix3e_peer* p = user;
	SOCKET peer = accept(p->listener, NULL, NULL);
	static const uint8_t keep_alive[] = {0x00};
	static const uint8_t login_response[] = {0x01, 0, 0, 0, 0x2A, 0, 0, 0, 0,
		0, 0, 0, 0, 0x27, 0x0F, 0};
	if(peer != INVALID_SOCKET && peer_login(peer)
	   && walk_send_all(peer, keep_alive, sizeof keep_alive)
	   && walk_send_all(peer, login_response, sizeof login_response)) {
		fault_peer_recv_timeout(peer, 20);
		while(!atomic_load(&p->stop)) {
			size_t have = atomic_load(&p->length);
			if(have == sizeof p->bytes)
				break;
			int n = recv(peer, (char*)p->bytes + have,
						 (int)(sizeof p->bytes - have), 0);
			if(n == 0)
				break;
			if(n > 0)
				atomic_store(&p->length, have + (size_t)n);
		}
	}
	if(peer != INVALID_SOCKET)
		closesocket(peer);
	closesocket(p->listener);
	return NULL;
}

static bool fix3e_logged_in(const struct server_remote_status* status) {
	/* is_play's echo + post-login keep-alive are both out: none pending. */
	return status->state == REMOTE_PLAY && status->logged_in
		&& status->keep_alives_sent >= 2 && status->queued_frames == 0;
}

/* Delivered frames: how many of `frames` appear in the peer's bytes in FIFO
 * order (each one after the previous one's end). */
static int fix3e_delivered(struct fix3e_peer* p, const struct mp_frame* frames) {
	size_t have = atomic_load(&p->length), at = 0;
	int found = 0;
	while(found < FIX3E_FRAMES) {
		const struct mp_frame* f = &frames[found];
		bool hit = false;
		for(; !hit && at + f->length <= have; at++)
			hit = memcmp(p->bytes + at, f->data, f->length) == 0;
		if(!hit)
			break;
		at += f->length - 1;
		found++;
	}
	return found;
}

static bool test_fix3e_writer_isolated_from_impl_lock(void) {
	static struct fix3e_peer peer;
	struct server_remote remote = {0};
	assert(server_remote_init(&remote)); /* also starts the socket layer */
	uint16_t port;
	peer.listener = make_listener(&port);
	atomic_init(&peer.stop, false);
	atomic_init(&peer.length, 0);
	struct thread peer_thread;
	assert(thread_create(&peer_thread, fix3e_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	assert(wait_for(&remote, fix3e_logged_in, 5000));
	struct server_remote_status before, after;
	server_remote_get_status(&remote, &before);
	struct mp_frame frames[FIX3E_FRAMES], keep_alive;
	assert(mp_encode_keep_alive(&keep_alive));
	for(int i = 0; i < FIX3E_FRAMES; i++) {
		char text[32];
		snprintf(text, sizeof text, "fix3e-frame-%02d", i);
		assert(mp_encode_chat(&frames[i], text));
	}
	struct server_remote_impl* impl = remote.impl;
	thread_mutex_lock(&impl->lock); /* stands in for a compaction pass */
	for(int i = 0; i < FIX3E_FRAMES + 2; i++) {
		const struct mp_frame* f = i < FIX3E_FRAMES ? &frames[i] : &keep_alive;
		bool echo = i == 3;
#ifdef SERVER_REMOTE_FRAME_LOCK
		assert(queue_frame_locked(impl, f, echo));
#else /* pre-FIX3e layout: the ring lived under impl->lock */
		size_t index = (impl->frame_head + impl->frame_count)
			% REMOTE_QUEUE_CAPACITY;
		impl->frames[index] = (struct remote_frame) {
			.generation = impl->generation, .frame = *f,
			.correction_echo = echo};
		impl->frame_count++;
		thread_cond_signal(&impl->write_signal);
#endif
	}
	ptime_t t0 = time_get();
	int delivered = 0;
	while(time_diff_ms(t0, time_get()) < FIX3E_HOLD_MS
		  && (delivered = fix3e_delivered(&peer, frames)) < FIX3E_FRAMES)
		thread_msleep(1);
	thread_mutex_unlock(&impl->lock);
	for(int i = 0; i < 2000; i++) { /* counters land after the last send */
		server_remote_get_status(&remote, &after);
		if(after.keep_alives_sent >= before.keep_alives_sent + 2)
			break;
		thread_msleep(1);
	}
	bool ok = delivered == FIX3E_FRAMES && after.state == REMOTE_PLAY
		&& after.mp_end == 0 && !after.dying
		&& after.keep_alives_sent == before.keep_alives_sent + 2
		&& after.corrections_echoed == before.corrections_echoed + 1;
	if(!ok)
		printf("REL-MP-FIX3e RED: writer delivered %d of %d frames in FIFO "
			   "order while impl->lock was held %d ms; keep_alives_sent +%u "
			   "corrections_echoed +%u state=%d mp_end=%u\n",
			   delivered, FIX3E_FRAMES, FIX3E_HOLD_MS,
			   after.keep_alives_sent - before.keep_alives_sent,
			   after.corrections_echoed - before.corrections_echoed,
			   (int)after.state, after.mp_end);
	atomic_store(&peer.stop, true);
	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	server_remote_destroy(&remote);
	if(ok)
		printf("PASS REL-MP-FIX3e: %d frames sent in FIFO order with "
			   "impl->lock held\n", FIX3E_FRAMES);
	return ok;
}

/* REL-MP-FIX4b (close-diag.md run 2): same-dim respawn into an absent column.
 * Like vanilla (handleFlying streams, NetworkManager.java:47 30 s SO_TIMEOUT,
 * scaled here) the peer sends the column only after FIX4B_MIN_MOVES movement
 * packets and kicks on a silent gap. Keep-alive wiring mirrors main.c. */
bool clin_remote_hold_keep_alive_due(ptime_t now);
#define FIX4B_SO_TIMEOUT_MS 1500
#define FIX4B_WINDOW_MS 2500
#define FIX4B_MIN_MOVES 10
struct fix4b_peer {
	SOCKET listener;
	atomic_bool world_ready, done, ended;
	uint32_t ka, moves, at_b, max_gap_ms;
	bool sent_column, timed_out; int bad_id;
};

static uint8_t* fix4b_column(int32_t cx, size_t* len) {
	size_t cells = 16u * 128u * 16u, raw = cells + 3u * (cells / 2u);
	uLongf cap = compressBound((uLong)raw);
	uint8_t *in = calloc(1, raw), *out = malloc(18u + cap);
	if(in) in[0] = BLOCK_STONE;
	if(!in || !out
	   || compress2(out + 18, &cap, in, (uLong)raw, Z_BEST_SPEED) != Z_OK) {
		free(in); free(out); return NULL;
	}
	free(in); out[0] = 0x33; put_i32(out + 1, cx * 16); put_i16(out + 5, 0);
	put_i32(out + 7, 0); out[11] = 15; out[12] = 127; out[13] = 15;
	put_i32(out + 14, (int32_t)cap); *len = 18u + cap; return out;
}

/* Server Packet13: x, y = feet+1.62, stance = feet, z (teleportTo). */
static void fix4b_pos(uint8_t* p, double x) {
	p[0] = 0x0D; put_f64(p + 1, x); put_f64(p + 9, 101.62);
	put_f64(p + 17, 100.0); put_f64(p + 25, 0.5); put_f32(p + 33, 90.0f);
	put_f32(p + 37, 0.0f); p[41] = 1;
}

/* 1 = frame read, 0 = none within wait_ms, -1 = closed, -2 = unexpected id. */
static int fix4b_frame(SOCKET s, unsigned wait_ms, uint8_t* id, uint8_t* body) {
	fd_set set; FD_ZERO(&set); FD_SET(s, &set);
	struct timeval tv = {0, (long)wait_ms * 1000};
	int r = select((int)s + 1, &set, NULL, NULL, &tv);
	if(r <= 0) return r < 0 ? -1 : 0;
	if(!socket_read_exact(s, id, 1)) return -1;
	size_t n;
	switch(*id) {
		case 0x00: n = 0; break;  case 0x0A: n = 1; break;
		case 0x0B: n = 33; break; case 0x0C: n = 9; break;
		case 0x0D: n = 41; break; case 0x10: n = 2; break;
		case 0x12: case 0x13: n = 5; break;
		default: return -2;
	}
	return n == 0 || socket_read_exact(s, body, n) ? 1 : -1;
}

static void* fix4b_peer_entry(void* user) {
	struct fix4b_peer* p = user;
	SOCKET s = accept(p->listener, NULL, NULL);
	size_t c0n = 0, cbn = 0;
	uint8_t *c0 = fix4b_column(0, &c0n), *cb = fix4b_column(12, &cbn);
	static const uint8_t login[] = {0x01,0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0,
		0x06,0,0,0,0, 0,0,0,100, 0,0,0,0, 0x08,0,20, 0x32,0,0,0,0, 0,0,0,0, 1};
	static const uint8_t respawn[] = {0x09,0}, ab[] = {0x32,0,0,0,12, 0,0,0,0, 1};
	uint8_t pos0[42], posb[42], id = 0, body[64] = {0}; int r = 0;
	fix4b_pos(pos0, 0.5); fix4b_pos(posb, 200.5);
	if(s == INVALID_SOCKET || !c0 || !cb || !peer_login(s)
	   || !walk_send_all(s, login, sizeof login)
	   || !walk_send_all(s, pos0, sizeof pos0) || !walk_send_all(s, c0, c0n))
		goto done;
	while(!atomic_load(&p->world_ready) && !atomic_load(&p->done))
		if((r = fix4b_frame(s, 20, &id, body)) < 0) goto done;
	if(!walk_send_all(s, respawn, sizeof respawn) || !walk_send_all(s, posb, sizeof posb))
		goto done;
	ptime_t t0 = time_get(), last = t0;
	while(!atomic_load(&p->done) && (r = fix4b_frame(s, 20, &id, body)) >= 0) {
		ptime_t now = time_get();
		uint32_t gap = (uint32_t)time_diff_ms(last, now);
		if(gap > p->max_gap_ms) p->max_gap_ms = gap;
		if(r == 0) {
			if((p->timed_out = gap > FIX4B_SO_TIMEOUT_MS)) break;
			continue;
		}
		last = now;
		uint8_t x[8]; double bx;
		for(int i = 0; i < 8; i++) x[i] = body[7 - i];
		memcpy(&bx, x, 8);
		p->ka += id == 0x00;
		p->moves += id >= 0x0A && id <= 0x0D;
		p->at_b += (id == 0x0B || id == 0x0D) && bx == 200.5;
		if(!p->sent_column && p->moves >= FIX4B_MIN_MOVES
		   && time_diff_ms(t0, now) >= FIX4B_WINDOW_MS
		   && !(p->sent_column = walk_send_all(s, ab, sizeof ab)
				&& walk_send_all(s, cb, cbn)))
			break;
	}
done:
	if(r == -2) p->bad_id = id;
	free(c0); free(cb);
	if(s != INVALID_SOCKET) closesocket(s);
	closesocket(p->listener);
	atomic_store(&p->ended, true);
	return NULL;
}

static bool test_fix4b_respawn_hold_keeps_talking(void) {
	static struct fix4b_peer peer = {.bad_id = -1};
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	peer.listener = make_listener(&port);
	atomic_init(&peer.world_ready, false); atomic_init(&peer.done, false);
	atomic_init(&peer.ended, false);
	struct thread peer_thread;
	assert(thread_create(&peer_thread, fix4b_peer_entry, &peer, 64));
	gstate.remote_session = true;
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	bool held = false, released = false;
	struct server_remote_status st;
	ptime_t t0 = time_get();
	while(!released && !atomic_load(&peer.ended)
		  && time_diff_ms(t0, time_get()) < 10000) {
		server_remote_pump(&remote);
		server_remote_get_status(&remote, &st);
		if(st.state == REMOTE_PLAY && !st.dying
		   && clin_remote_hold_keep_alive_due(time_get()))
			(void)server_remote_send_keep_alive(&remote);
		clin_update();
		/* corrections_received: world_loaded is this session's now. */
		if(st.corrections_received >= 1 && gstate.world_loaded
		   && !gstate.remote_terrain_hold)
			atomic_store(&peer.world_ready, true);
		if(atomic_load(&peer.world_ready)) {
			held |= gstate.remote_terrain_hold;
			released = held && !gstate.remote_terrain_hold;
		}
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &st);
	atomic_store(&peer.done, true);
	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	server_remote_destroy(&remote);
	gstate.remote_session = false;
	bool ok = held && released && peer.sent_column && !peer.timed_out
		&& peer.ka >= 2 && peer.moves >= FIX4B_MIN_MOVES && peer.at_b >= 1
		&& peer.max_gap_ms < FIX4B_SO_TIMEOUT_MS && peer.bad_id < 0;
	printf("%s REL-MP-FIX4b: held=%d released=%d keep_alives=%u moves=%u "
		   "echo_at_dest=%u max_gap_ms=%u timed_out=%d column_sent=%d "
		   "bad_id=%d state=%d mp_end=%u\n", ok ? "PASS" : "RED", held,
		   released, peer.ka, peer.moves, peer.at_b, peer.max_gap_ms,
		   peer.timed_out, peer.sent_column, peer.bad_id, (int)st.state, st.mp_end);
	return ok;
}

/* ---- REL-MP-JOINSPEED: lossless-replay oracle -------------------------- */
/* An independent model of what a Beta 1.7.3 client holds after the SAME
 * stream, built straight from the decoded packets with none of the remote
 * module's machinery (no log, no chain, no compaction, no stamps):
 *   Packet50 load   -> an empty pre-chunk: air, metadata 0
 *                      [ChunkProviderClient.java:39-41 prepareChunk];
 *   Packet50 unload -> the column is gone;
 *   Packet51        -> its region overwrites ids + metadata, in arrival order
 *                      [NetClientHandler.java:294-297 -> World.setChunkData ->
 *                      Chunk.setChunkData:548-591];
 *   Packet52/53     -> that cell's id + metadata [NetClientHandler.java:275-301];
 *   Packet60        -> each listed cell becomes air (the same clear the remote
 *                      path applies, server_remote_protocol.c).
 * Light is NOT modelled: the client relights the cells it changes, so light
 * is a derived value, not wire state. What every server write must leave
 * behind -- exactly once, in order, nothing lost or resurrected -- is the
 * final id + metadata of every cell, and that is what the equivalence
 * compares, for every loaded column, at the end of the replay. */
#define WALK_CELLS (16u * 128u * 16u)
#define WALK_NIB (WALK_CELLS / 2u)
#define WALK_RAW (WALK_CELLS + 3u * WALK_NIB)
struct walk_oracle_column {
	int32_t cx, cz;
	bool live;
	bool any_body; /* a Packet51 arrived during this residency */
	uint8_t* raw;  /* ids then metadata nibbles (wire layout) */
};
struct walk_oracle {
	struct walk_oracle_column* cols;
	size_t count, capacity;
	size_t edits, bodies, loads, unloads, reload_without_unload;
};

static struct walk_oracle_column* walk_oracle_find(struct walk_oracle* o,
												   int32_t cx, int32_t cz) {
	for(size_t i = 0; i < o->count; i++)
		if(o->cols[i].live && o->cols[i].cx == cx && o->cols[i].cz == cz)
			return &o->cols[i];
	return NULL;
}

static void walk_oracle_set(struct walk_oracle* o, int32_t x, int32_t y,
							int32_t z, uint8_t id, uint8_t meta) {
	if(y < 0 || y > 127)
		return;
	struct walk_oracle_column* c
		= walk_oracle_find(o, (int32_t)floor(x / 16.0), (int32_t)floor(z / 16.0));
	if(!c)
		return; /* Beta's client writes into its empty placeholder chunk */
	size_t idx = ((size_t)(x & 15) * 16u + (size_t)(z & 15)) * 128u + (size_t)y;
	unsigned shift = ((unsigned)y & 1u) * 4u;
	uint8_t* m = c->raw + WALK_CELLS + (idx >> 1);
	c->raw[idx] = id;
	*m = (uint8_t)((*m & ~(0x0Fu << shift)) | ((unsigned)(meta & 15u) << shift));
	o->edits++;
}

static bool walk_oracle_build(const struct walk_stream* s, struct walk_oracle* o) {
	memset(o, 0, sizeof(*o));
	uint8_t* body = malloc(WALK_RAW);
	if(!body)
		return false;
	struct mp_decoder decoder;
	mp_decoder_init(&decoder);
	int32_t dimension = 0;
	bool ok = true;
	for(size_t i = 0; i < s->length && ok; i++) {
		struct mp_event e;
		enum mp_decode_result r = mp_decoder_feed(&decoder, s->bytes[i], &e);
		if(r != MP_DECODE_EVENT)
			continue;
		switch(e.type) {
			case MP_EVENT_LOGIN: dimension = e.payload.login.dimension; break;
			case MP_EVENT_RESPAWN:
				if(e.payload.respawn_dimension != dimension) {
					dimension = e.payload.respawn_dimension;
					for(size_t k = 0; k < o->count; k++)
						o->cols[k].live = false;
				}
				break;
			case MP_EVENT_PRE_CHUNK: {
				int32_t cx = e.payload.pre_chunk.chunk_x;
				int32_t cz = e.payload.pre_chunk.chunk_z;
				struct walk_oracle_column* c = walk_oracle_find(o, cx, cz);
				if(!e.payload.pre_chunk.mode) {
					if(c)
						c->live = false;
					o->unloads++;
					break;
				}
				o->loads++;
				if(c) {
					o->reload_without_unload++;
					c->live = false;
				}
				if(o->count == o->capacity) {
					size_t cap = o->capacity ? o->capacity * 2 : 256;
					void* grown = realloc(o->cols, cap * sizeof(*o->cols));
					if(!grown) {
						ok = false;
						break;
					}
					o->cols = grown;
					o->capacity = cap;
				}
				c = &o->cols[o->count++];
				c->cx = cx;
				c->cz = cz;
				c->live = true;
				c->any_body = false;
				c->raw = calloc(1, WALK_CELLS + WALK_NIB); /* air, meta 0 */
				ok = c->raw != NULL;
			} break;
			case MP_EVENT_MAP_CHUNK: {
				const struct mp_map_chunk* w = &e.payload.map_chunk;
				int32_t cx = (int32_t)floor(w->x / 16.0);
				int32_t cz = (int32_t)floor(w->z / 16.0);
				struct walk_oracle_column* c = walk_oracle_find(o, cx, cz);
				uLongf n = WALK_RAW;
				if(!c || w->inflated_length > WALK_RAW
				   || uncompress(body, &n, w->compressed,
								 (uLong)w->compressed_length) != Z_OK
				   || n != w->inflated_length) {
					/* CaveWii drops a body for a column that was never
					 * announced only if the stream does; none of the
					 * recorded streams does, so treat it as a setup fault. */
					fprintf(stderr, "JOINSPEED SETUP: body at %zu not modelled\n", i);
					ok = false;
					break;
				}
				int32_t x0 = w->x - cx * 16, z0 = w->z - cz * 16, y0 = w->y;
				size_t o_ = 0;
				for(int32_t x = x0; x < x0 + w->sx; x++)
					for(int32_t z = z0; z < z0 + w->sz; z++) {
						size_t idx = ((size_t)x << 11) | ((size_t)z << 7) | (size_t)y0;
						memcpy(c->raw + idx, body + o_, w->sy);
						o_ += w->sy;
					}
				for(int32_t x = x0; x < x0 + w->sx; x++)
					for(int32_t z = z0; z < z0 + w->sz; z++) {
						size_t idx = ((size_t)x << 11) | ((size_t)z << 7) | (size_t)y0;
						memcpy(c->raw + WALK_CELLS + (idx >> 1), body + o_,
							   w->sy / 2u);
						o_ += w->sy / 2u;
					}
				c->any_body = true;
				o->bodies++;
			} break;
			case MP_EVENT_BLOCK_CHANGE:
				walk_oracle_set(o, e.payload.block_change.x,
								e.payload.block_change.y, e.payload.block_change.z,
								e.payload.block_change.block_type,
								e.payload.block_change.metadata);
				break;
			case MP_EVENT_MULTI_BLOCK_CHANGE: {
				const struct mp_multi_block_change* m = &e.payload.multi_block_change;
				for(uint16_t k = 0; k < m->count; k++)
					walk_oracle_set(o, m->chunk_x * 16 + ((m->coordinate[k] >> 12) & 15),
									m->coordinate[k] & 255,
									m->chunk_z * 16 + ((m->coordinate[k] >> 8) & 15),
									m->type[k], m->metadata[k]);
			} break;
			case MP_EVENT_EXPLOSION: {
				int32_t bx = java_double_to_int(e.payload.explosion.x);
				int32_t by = java_double_to_int(e.payload.explosion.y);
				int32_t bz = java_double_to_int(e.payload.explosion.z);
				for(uint16_t k = 0; k < e.payload.explosion.count; k++)
					walk_oracle_set(o, bx + e.payload.explosion.blocks[k].dx,
									by + e.payload.explosion.blocks[k].dy,
									bz + e.payload.explosion.blocks[k].dz, 0, 0);
			} break;
			default: break;
		}
		mp_event_destroy(&e);
	}
	mp_decoder_destroy(&decoder);
	free(body);
	return ok;
}

static void walk_oracle_free(struct walk_oracle* o) {
	for(size_t i = 0; i < o->count; i++)
		free(o->cols[i].raw);
	free(o->cols);
	memset(o, 0, sizeof(*o));
}

/* ---- the chain-integrity check (review r1, Sol) ------------------------ */
/* Independently of every production walk: each column's chain must reach
 * exactly its owned pool entries, once each, in strictly ascending arrival,
 * ending at edit_tail, with edit_count nodes; no owned entry may be
 * unreachable; the per-column counts must sum to deferred_edits; and a
 * SETTLED installed column (RESIDENT, no head, no chain) must own no edit at
 * all -- an entry still parked there is an edit the drain skipped. Returns
 * the number of violations; prints the first few. impl->lock held. */
static size_t walk_chain_check(const struct server_remote_impl* impl) {
	static uint8_t seen[REMOTE_DEFERRED_EDIT_CAPACITY];
	memset(seen, 0, sizeof(seen));
	size_t bad = 0, linked = 0;
#define WALK_CHAIN_BAD(...) \
	do { \
		if(bad++ < 6) \
			printf("  JOINSPEED CHAIN " __VA_ARGS__); \
	} while(0)
	for(size_t k = 0; k < REMOTE_CHUNK_ENTRY_CAPACITY; k++) {
		const struct remote_chunk* c = &impl->chunks[k];
		if(c->state == REMOTE_CHUNK_UNUSED) {
			if(c->edit_head || c->edit_tail || c->edit_count)
				WALK_CHAIN_BAD("slot %zu: unused entry holds a chain\n", k);
			continue;
		}
		size_t n = 0;
		uint16_t last = 0;
		uint64_t prev_arrival = 0;
		for(uint16_t ref = c->edit_head; ref; n++) {
			if(!impl->deferred_edit_log || ref > REMOTE_DEFERRED_EDIT_CAPACITY
			   || n >= REMOTE_DEFERRED_EDIT_CAPACITY) {
				WALK_CHAIN_BAD("slot %zu: bad index %u / cycle\n", k, (unsigned)ref);
				break;
			}
			const struct remote_deferred_edit* e = &impl->deferred_edit_log[ref - 1u];
			if(e->owner != k + 1u)
				WALK_CHAIN_BAD("slot %zu: node %u owned by %u\n", k, (unsigned)ref,
							   (unsigned)e->owner);
			if(seen[ref - 1u]++)
				WALK_CHAIN_BAD("slot %zu: node %u reached twice\n", k, (unsigned)ref);
			if(n && e->arrival <= prev_arrival)
				WALK_CHAIN_BAD("slot %zu: arrival order broken at %u\n", k,
							   (unsigned)ref);
			prev_arrival = e->arrival;
			last = ref;
			ref = e->next;
		}
		if(last != c->edit_tail || n != c->edit_count)
			WALK_CHAIN_BAD("slot %zu: tail %u/%u count %zu/%u\n", k, (unsigned)last,
						   (unsigned)c->edit_tail, n, (unsigned)c->edit_count);
		if(c->installed && c->state == REMOTE_CHUNK_RESIDENT && n)
			WALK_CHAIN_BAD("slot %zu (%ld,%ld): settled installed column still "
						   "parks %zu edits (drain skipped them)\n",
						   k, (long)c->chunk_x, (long)c->chunk_z, n);
		linked += n;
	}
	for(size_t i = 0; impl->deferred_edit_log && i < REMOTE_DEFERRED_EDIT_CAPACITY;
		i++)
		if(remote_deferred_edit_used(&impl->deferred_edit_log[i]) && !seen[i])
			WALK_CHAIN_BAD("pool entry %zu (owner %u) unreachable from any chain\n",
						   i + 1, (unsigned)impl->deferred_edit_log[i].owner);
	if(linked != impl->deferred_edits)
		WALK_CHAIN_BAD("chains hold %zu edits, deferred_edits says %u\n", linked,
					   impl->deferred_edits);
#undef WALK_CHAIN_BAD
	return bad;
}

/* A NOT-installed column's logical content: its head, queued bodies and
 * parked edits applied to the pre-chunk in stamp (= wire) order -- a body
 * overwrites its region, an edit its cell, so an edit older than a covering
 * body loses to it exactly as Beta's in-order application does. The edits
 * come from the column's chain, whose integrity walk_chain_check verifies
 * separately. Never used for an installed column (review r1): those are
 * compared as the client holds them. */
struct walk_item {
	uint64_t stamp;
	const struct mp_map_chunk* body; /* NULL: edit */
	const struct remote_deferred_edit* edit;
};
static int walk_item_cmp(const void* a, const void* b) {
	const struct walk_item* x = a;
	const struct walk_item* y = b;
	return x->stamp < y->stamp ? -1 : x->stamp > y->stamp;
}
static bool walk_apply_pending(const struct server_remote_impl* impl,
							   const struct remote_chunk* chunk, uint8_t* model) {
	static struct walk_item items[REMOTE_DEFERRED_EDIT_CAPACITY + 8];
	static uint8_t body[WALK_RAW];
	size_t n = 0;
	if(chunk->wire.compressed)
		items[n++] = (struct walk_item) {chunk->arrival, &chunk->wire, NULL};
	for(uint8_t q = 0; q < chunk->queued_count; q++)
		items[n++] = (struct walk_item) {chunk->queued_arrival[q],
			&chunk->queued[q], NULL};
	for(uint16_t ref = chunk->edit_head;
		ref && n < sizeof(items) / sizeof(items[0]);) {
		const struct remote_deferred_edit* e = &impl->deferred_edit_log[ref - 1u];
		items[n++] = (struct walk_item) {e->arrival, NULL, e};
		ref = e->next;
	}
	qsort(items, n, sizeof(items[0]), walk_item_cmp);
	for(size_t i = 0; i < n; i++) {
		if(items[i].edit) {
			const struct remote_deferred_edit* e = items[i].edit;
			size_t idx = e->cell; /* ((x & 15) * 16 + (z & 15)) * 128 + y */
			unsigned shift = ((unsigned)remote_deferred_edit_y(e) & 1u) * 4u;
			model[idx] = e->type;
			model[WALK_CELLS + (idx >> 1)] = (uint8_t)(
				(model[WALK_CELLS + (idx >> 1)] & ~(0x0Fu << shift))
				| ((unsigned)(e->metadata & 15u) << shift));
			continue;
		}
		const struct mp_map_chunk* w = items[i].body;
		uLongf len = WALK_RAW;
		if(w->inflated_length > WALK_RAW
		   || uncompress(body, &len, w->compressed, (uLong)w->compressed_length)
			   != Z_OK
		   || len != w->inflated_length)
			return false;
		int32_t x0 = w->x - chunk->chunk_x * 16, z0 = w->z - chunk->chunk_z * 16;
		size_t o = 0;
		for(int32_t x = x0; x < x0 + w->sx; x++)
			for(int32_t z = z0; z < z0 + w->sz; z++) {
				size_t idx = ((size_t)x << 11) | ((size_t)z << 7) | (size_t)w->y;
				memcpy(model + idx, body + o, w->sy);
				o += w->sy;
			}
		for(int32_t x = x0; x < x0 + w->sx; x++)
			for(int32_t z = z0; z < z0 + w->sz; z++) {
				size_t idx = ((size_t)x << 11) | ((size_t)z << 7) | (size_t)w->y;
				memcpy(model + WALK_CELLS + (idx >> 1), body + o, w->sy / 2u);
				o += w->sy / 2u;
			}
	}
	return true;
}

/* Drive every column that holds a body to installation (the radius opened to
 * the platform maximum, so the admission gate is not what is being measured)
 * until the remote state is quiet. */
static void walk_settle(struct server_remote* remote) {
	struct server_remote_impl* impl = remote->impl;
	server_remote_set_admit_radius(remote, SERVER_REMOTE_ADMIT_RADIUS_MAX);
	int quiet = 0;
	for(int i = 0; i < 20000 && quiet < 200; i++) {
		server_remote_pump(remote);
		clin_update();
		thread_mutex_lock(&impl->lock);
		bool busy = impl->promotion.active || impl->event_count;
		for(size_t k = 0; !busy && k < REMOTE_CHUNK_ENTRY_CAPACITY; k++)
			busy = impl->chunks[k].state == REMOTE_CHUNK_RETAINED
				|| impl->chunks[k].state == REMOTE_CHUNK_PROMOTING;
		thread_mutex_unlock(&impl->lock);
		quiet = busy ? 0 : quiet + 1;
	}
}

/* Compare each loaded column's final id + metadata with the oracle:
 *  - installed: must be SETTLED (RESIDENT, no pending head, chain or parked
 *    edit) and is compared exactly as the client voxels hold it -- the
 *    checker applies nothing on top (review r1);
 *  - not installed: head + chain + parked edits on the pre-chunk, in stamp
 *    order (walk_apply_pending).
 * Plus walk_chain_check over the whole pool. Returns mismatching columns +
 * chain violations (0 = lossless and consistent). impl->lock held. */
static size_t walk_compare(const struct server_remote_impl* impl,
						   const struct walk_oracle* o, size_t* out_checked,
						   size_t* out_installed, size_t* out_parked_only) {
	uint8_t* model = malloc(WALK_CELLS + WALK_NIB);
	assert(model);
	size_t bad = walk_chain_check(impl), checked = 0, installed = 0,
		   parked_only = 0, shown = 0;
	for(size_t ci = 0; ci < o->count; ci++) {
		const struct walk_oracle_column* c = &o->cols[ci];
		if(!c->live)
			continue;
		checked++;
		size_t index = REMOTE_CHUNK_ENTRY_CAPACITY;
		for(size_t k = 0; k < REMOTE_CHUNK_ENTRY_CAPACITY; k++)
			if(impl->chunks[k].state != REMOTE_CHUNK_UNUSED
			   && impl->chunks[k].chunk_x == c->cx
			   && impl->chunks[k].chunk_z == c->cz)
				index = k;
		const struct remote_chunk* chunk
			= index == REMOTE_CHUNK_ENTRY_CAPACITY ? NULL : &impl->chunks[index];
		memset(model, 0, WALK_CELLS + WALK_NIB);
		bool have = false;
		const char* why = "no entry";
		if(chunk && chunk->installed) {
			installed++;
			if(chunk->state != REMOTE_CHUNK_RESIDENT || chunk->wire.compressed
			   || chunk->queued_count || chunk->edit_count) {
				why = "installed but not settled";
			} else {
				have = true;
				for(unsigned cy = 0; cy < 8; cy++) {
					struct chunk* section = world_find_chunk(&gstate.world,
						c->cx * 16, (w_coord_t)(cy * 16), c->cz * 16);
					if(!section)
						continue; /* never allocated: compares as air, meta 0 */
					for(unsigned x = 0; x < 16; x++)
						for(unsigned z = 0; z < 16; z++)
							for(unsigned y = 0; y < 16; y++) {
								struct block_data b = chunk_get_block(section,
									(c_coord_t)x, (c_coord_t)y, (c_coord_t)z);
								size_t idx
									= ((size_t)x * 16u + z) * 128u + cy * 16u + y;
								model[idx] = b.type;
								model[WALK_CELLS + (idx >> 1)] |= (uint8_t)(
									(b.metadata & 15u) << ((y & 1u) * 4u));
							}
				}
			}
		} else if(chunk) {
			have = true;
			if(!chunk->wire.compressed && !chunk->queued_count)
				parked_only++;
			if(!walk_apply_pending(impl, chunk, model)) {
				have = false;
				why = "a pending body does not decode";
			}
		}
		bool same = have && memcmp(model, c->raw, WALK_CELLS + WALK_NIB) == 0;
		if(!same) {
			if(shown++ < 8) {
				size_t first = 0;
				while(have && first < WALK_CELLS + WALK_NIB
					  && model[first] == c->raw[first])
					first++;
				printf("  JOINSPEED MISMATCH column (%ld,%ld): %s; installed=%d "
					   "head=%d queued=%u edits=%u any_body=%d first_diff=%zu\n",
					   (long)c->cx, (long)c->cz, have ? "content differs" : why,
					   chunk ? (int)chunk->installed : -1,
					   chunk ? chunk->wire.compressed != NULL : -1,
					   chunk ? chunk->queued_count : 0,
					   chunk ? chunk->edit_count : 0, (int)c->any_body, first);
			}
			bad++;
		}
	}
	free(model);
	*out_checked = checked;
	*out_installed = installed;
	*out_parked_only = parked_only;
	return bad;
}

static size_t walk_equivalence(struct server_remote* remote,
							   const struct walk_oracle* o, size_t* out_checked,
							   size_t* out_installed, size_t* out_parked_only) {
	struct server_remote_impl* impl = remote->impl;
	walk_settle(remote);
	thread_mutex_lock(&impl->lock);
	size_t bad = walk_compare(impl, o, out_checked, out_installed, out_parked_only);
	thread_mutex_unlock(&impl->lock);
	return bad;
}

/* NEGATIVE CONTROLS (review r1): the check must FAIL on each injected fault,
 * and pass again once it is undone. Run on the settled state after a passing
 * equivalence. Returns the number of controls the check did NOT catch;
 * *out_ran counts the controls this stream could stage. */
static size_t walk_negative_controls(struct server_remote* remote,
									 const struct walk_oracle* o, size_t* out_ran) {
	struct server_remote_impl* impl = remote->impl;
	size_t missed = 0, ran = 0, a, b, d;
	thread_mutex_lock(&impl->lock);
	size_t backed = REMOTE_CHUNK_ENTRY_CAPACITY, settled = REMOTE_CHUNK_ENTRY_CAPACITY;
	for(size_t k = 0; k < REMOTE_CHUNK_ENTRY_CAPACITY; k++) {
		const struct remote_chunk* c = &impl->chunks[k];
		if(backed == REMOTE_CHUNK_ENTRY_CAPACITY && !c->installed
		   && c->state != REMOTE_CHUNK_UNUSED && c->edit_count >= 2)
			backed = k;
		if(settled == REMOTE_CHUNK_ENTRY_CAPACITY && c->installed
		   && c->state == REMOTE_CHUNK_RESIDENT && !c->edit_count
		   && world_find_chunk(&gstate.world, c->chunk_x * 16, 64, c->chunk_z * 16))
			settled = k;
	}
	/* (1) BROKEN LINK: the head skips its successor, which stays owned but
	 * unreachable -- an edit the production walks would never see. */
	if(backed != REMOTE_CHUNK_ENTRY_CAPACITY) {
		ran++;
		struct remote_chunk* c = &impl->chunks[backed];
		struct remote_deferred_edit* head = &impl->deferred_edit_log[c->edit_head - 1u];
		uint16_t skipped = head->next;
		struct remote_deferred_edit* lost = &impl->deferred_edit_log[skipped - 1u];
		uint16_t saved_tail = c->edit_tail;
		head->next = lost->next;
		if(c->edit_tail == skipped)
			c->edit_tail = c->edit_head;
		c->edit_count--;
		size_t caught = walk_compare(impl, o, &a, &b, &d);
		printf("  negative (1) broken chain link on slot %zu: %zu violations "
			   "reported\n", backed, caught);
		missed += caught == 0;
		head->next = skipped;
		c->edit_tail = saved_tail;
		c->edit_count++;
	}
	/* (2) SKIPPED DRAIN: a parked entry moved onto a settled installed column
	 * (relinked into its chain, owned by it) -- an edit the drain left behind.
	 * The old checker would have APPLIED it; the check must reject it. */
	if(backed != REMOTE_CHUNK_ENTRY_CAPACITY && settled != REMOTE_CHUNK_ENTRY_CAPACITY) {
		ran++;
		struct remote_chunk* from = &impl->chunks[backed];
		struct remote_chunk* to = &impl->chunks[settled];
		uint16_t ref = from->edit_head;
		struct remote_deferred_edit* e = &impl->deferred_edit_log[ref - 1u];
		from->edit_head = e->next;
		from->edit_count--;
		uint16_t saved_owner = e->owner, saved_next = e->next;
		e->owner = (uint16_t)(settled + 1u);
		e->next = 0;
		to->edit_head = to->edit_tail = ref;
		to->edit_count = 1;
		size_t caught = walk_compare(impl, o, &a, &b, &d);
		printf("  negative (2) drain-skipped entry parked on settled slot %zu: "
			   "%zu violations reported\n", settled, caught);
		missed += caught == 0;
		to->edit_head = to->edit_tail = 0;
		to->edit_count = 0;
		e->owner = saved_owner;
		e->next = saved_next;
		from->edit_head = ref;
		from->edit_count++;
	}
	/* (3) WRONG VOXEL: one installed cell differs from what the server sent
	 * (an edit applied wrong, or not at all). */
	if(settled != REMOTE_CHUNK_ENTRY_CAPACITY) {
		ran++;
		const struct remote_chunk* c = &impl->chunks[settled];
		struct chunk* section
			= world_find_chunk(&gstate.world, c->chunk_x * 16, 64, c->chunk_z * 16);
		struct block_data orig = chunk_get_block(section, 3, 5, 7);
		struct block_data wrong = orig;
		wrong.type = orig.type == BLOCK_GLASS ? BLOCK_STONE : BLOCK_GLASS;
		chunk_set_block(section, 3, 5, 7, wrong);
		size_t caught = walk_compare(impl, o, &a, &b, &d);
		printf("  negative (3) one wrong voxel on settled slot %zu: %zu "
			   "violations reported\n", settled, caught);
		missed += caught == 0;
		chunk_set_block(section, 3, 5, 7, orig);
	}
	size_t after = walk_compare(impl, o, &a, &b, &d);
	thread_mutex_unlock(&impl->lock);
	if(after) {
		printf("  negative controls: state not restored (%zu)\n", after);
		missed++;
	}
	*out_ran = ran;
	return missed;
}

/* Index of the last packet fully inside the first `rx` stream bytes. */
static size_t walk_packet_at(const struct walk_stream* s, uint32_t rx) {
	size_t index = 0;
	for(size_t i = 0; i < s->count; i++)
		if(s->packets[i].end <= rx)
			index = i;
	return index;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if(argc < 2) {
		fprintf(stderr, "IN-008 SETUP: usage: test_mp_walk_session <stream.bin>\n");
		return 2;
	}
	struct walk_stream s;
	if(!walk_load(argv[1], &s))
		return 2;
	size_t map_chunks = 0;
	for(size_t i = 0; i < s.count; i++)
		map_chunks += s.packets[i].type == MP_EVENT_MAP_CHUNK;
	printf("stream: %zu bytes, %zu packets, %zu Packet51\n", s.length, s.count,
		   map_chunks);
	struct walk_oracle oracle;
	if(!walk_oracle_build(&s, &oracle))
		return 2;
	printf("oracle: %zu loads, %zu unloads, %zu bodies, %zu cell writes, "
		   "%zu reloads without unload\n",
		   oracle.loads, oracle.unloads, oracle.bodies, oracle.edits,
		   oracle.reload_without_unload);

	assert(svin_init() && clin_init());
	blocks_init();
	items_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	if(!test_fix3e_writer_isolated_from_impl_lock())
		return 1;

	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct walk_peer peer = {.listener = make_listener(&port), .stream = &s};
	atomic_init(&peer.impl_ready, false);
	atomic_init(&peer.stop, false);
	atomic_init(&peer.sent_all, false);
	atomic_init(&peer.aborted, false);
	struct thread peer_thread;
	assert(thread_create(&peer_thread, walk_peer_entry, &peer, 64));
	if(!server_remote_start(&remote, "127.0.0.1", port, "Cave")) {
		fprintf(stderr, "IN-008 SETUP: server_remote_start failed\n");
		return 2;
	}
	/* After start: start's per-connection reset sets the radius to the
	 * platform max (server_remote.h:580-585). */
	server_remote_set_admit_radius(&remote, WALK_ADMIT_RADIUS);
	peer.impl = remote.impl;
	atomic_store(&peer.impl_ready, true);

	struct server_remote_status status;
	bool logged_in = false, ended = false, finished = false, stalled = false;
	int settle = 0;
	ptime_t t0 = time_get();
	while(!finished) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY && status.logged_in)
			logged_in = true;
		/* REL-MP-FIX5: one slow post-login frame (real Wii hw1021c worst
		 * main-thread gaps 557-984 ms) while the stream keeps arriving. A
		 * full event ring must back-pressure the reader, never end the
		 * session (Beta NetworkManager.java:24,137,187-208). */
		if(logged_in && !stalled) {
			thread_msleep(900);
			stalled = true;
		}
		if(status.forced_reconnects != 0 || status.dying || status.mp_end != 0
		   || (logged_in && status.state != REMOTE_PLAY)
		   || atomic_load(&peer.aborted)) {
			ended = true;
			break;
		}
		/* Every byte received: pump on so the queue and promotion drain. */
		if(atomic_load(&peer.sent_all) && status.rx_bytes >= s.length
		   && ++settle >= 500)
			finished = true;
		if(time_diff_ms(t0, time_get()) > WALK_TIMEOUT_MS) {
			fprintf(stderr,
					"IN-008 SETUP: timeout after %d ms (rx=%u of %zu, state=%d)\n",
					WALK_TIMEOUT_MS, status.rx_bytes, s.length, (int)status.state);
			atomic_store(&peer.stop, true);
			server_remote_cancel(&remote);
			thread_join(&peer_thread);
			server_remote_destroy(&remote);
			return 2;
		}
		thread_msleep(1);
	}
	if(!logged_in && !ended) {
		fprintf(stderr, "IN-008 SETUP: never reached PLAY\n");
		return 2;
	}
	/* Give the pump a moment to publish the terminal reason, then read it. */
	for(int i = 0; ended && i < 200; i++) {
		server_remote_pump(&remote);
		clin_update();
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	char reason[256];
	struct server_remote_impl* impl = remote.impl;
	thread_mutex_lock(&impl->lock);
	snprintf(reason, sizeof reason, "%s",
			 status.reason[0] ? status.reason : impl->reason);
	uint32_t chain_rejections = impl->chain_rejections;
	thread_mutex_unlock(&impl->lock);
	uint32_t rx = status.rx_bytes;
	bool alive = !ended && status.forced_reconnects == 0
		&& status.state == REMOTE_PLAY && !status.dying && status.mp_end == 0
		&& reason[0] == '\0' && rx >= s.length && gstate.world_loaded
		&& status.event_backpressure_waits > 0;
	/* REL-MP-JOINSPEED: the replay's own cost, read BEFORE the equivalence
	 * phase opens the radius (that phase is measurement, not the session). */
	unsigned replay_ms = (unsigned)time_diff_ms(t0, time_get());
	thread_mutex_lock(&impl->lock);
	uint32_t compactions = impl->compactions,
			 compacted_edits = impl->compacted_edits,
			 rescues = impl->deferred_log_rescues,
			 superseded = impl->deferred_edits_superseded,
			 applied = impl->deferred_edits_applied,
			 dropped = impl->deferred_edits_dropped,
			 parked_now = impl->deferred_edits,
			 parked_peak = impl->deferred_edits_peak,
			 lock_us_max = impl->compaction_lock_us_max,
			 unlocked = impl->compaction_unlocked,
			 aborts = impl->compaction_aborts,
			 chain_folds = impl->fold.folds;
	uint64_t lock_us_total = impl->compaction_lock_us_total;
	thread_mutex_unlock(&impl->lock);
	printf("JOINSPEED replay: %u ms; compactions=%u (rescues %u, unlocked %u, "
		   "aborts %u) folding %u edits; impl->lock held by compaction %llu us "
		   "total, %u us max; parked edits peak=%u superseded=%u applied=%u "
		   "dropped=%u still parked=%u; IN-009 chain folds %u\n",
		   replay_ms, compactions, rescues, unlocked, aborts, compacted_edits,
		   (unsigned long long)lock_us_total, lock_us_max, parked_peak,
		   superseded, applied, dropped, parked_now, chain_folds);
	/* REL-MP-JOINSPEED bounds (tmp/jobs/join-regress-1022): the exhaustion
	 * rescue -- one compaction per incoming edit -- never runs on a recorded
	 * session; compaction is O(columns), not O(edits); and the time it holds
	 * impl->lock is bounded (generous PC ceilings: a whole-column locked
	 * inflate+deflate is ~1.5 ms here, the old storm spent seconds). */
	bool join_bounds = rescues == 0 && compactions <= oracle.loads
		&& lock_us_total <= 1000000u && lock_us_max <= 100000u;
	printf("%s JOINSPEED bounds: rescues %u == 0, compactions %u <= %zu "
		   "announced columns, lock total %llu <= 1000000 us, max %u <= "
		   "100000 us\n",
		   join_bounds ? "PASS" : "RED", rescues, compactions, oracle.loads,
		   (unsigned long long)lock_us_total, lock_us_max);
	size_t equiv_bad = 0, equiv_checked = 0, equiv_installed = 0,
		   equiv_parked = 0;
	if(alive) {
		equiv_bad = walk_equivalence(&remote, &oracle, &equiv_checked,
									 &equiv_installed, &equiv_parked);
		printf("%s JOINSPEED equivalence: %zu loaded columns (%zu installed, "
			   "%zu backed of which %zu never got a body) compared cell for "
			   "cell (id + metadata) with the independent Beta model, %zu "
			   "mismatch\n",
			   equiv_bad ? "RED" : "PASS", equiv_checked, equiv_installed,
			   equiv_checked - equiv_installed, equiv_parked, equiv_bad);
		/* Review r1: the check must catch injected faults. A stream with no
		 * backed column holding >= 2 edits cannot stage (1)/(2); the vanilla
		 * join stream stages all three (ctest test_mp_walk_session_vanilla_join). */
		if(!equiv_bad) {
			size_t ran = 0;
			size_t missed = walk_negative_controls(&remote, &oracle, &ran);
			printf("%s JOINSPEED negative controls: %zu staged, %zu not caught\n",
				   missed ? "RED" : "PASS", ran, missed);
			if(missed)
				equiv_bad = missed;
		}
	}
	walk_oracle_free(&oracle);

	atomic_store(&peer.stop, true);
	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	server_remote_destroy(&remote);

	if(!alive) {
		size_t at = walk_packet_at(&s, rx);
		const struct walk_packet* p = &s.packets[at];
		printf("IN-008 RED: forced_reconnects=%u reason=%s at packet %zu\n",
			   status.forced_reconnects, reason[0] ? reason : "(none)", at);
		printf("  packet %zu: offset %zu..%zu type=%d%s; rx_bytes=%u of %zu; "
			   "chain_rejections=%u; state=%d mp_end=%u world_loaded=%d "
			   "bp_waits=%u\n",
			   at, p->start, p->end, (int)p->type,
			   p->type == MP_EVENT_MAP_CHUNK ? " (Packet51)" : "", rx, s.length,
			   chain_rejections, (int)status.state, status.mp_end,
			   (int)gstate.world_loaded, status.event_backpressure_waits);
		if(status.mp_end == SERVER_REMOTE_END_PROTOCOL
		   && strcmp(reason, "remote deferred edit log exhausted") == 0)
			printf("REL-MP RED: mp_end=%u reason=%s state=%d dying=%d "
				   "forced_reconnects=%u rx_bytes=%u of %zu at packet %zu\n",
				   status.mp_end, reason, (int)status.state, (int)status.dying,
				   status.forced_reconnects, rx, s.length, at);
		free(s.packets);
		free(s.bytes);
		return 1;
	}
	if(equiv_bad || !join_bounds) {
		free(s.packets);
		free(s.bytes);
		return 1;
	}
	if(!test_fix4b_respawn_hold_keeps_talking()) {
		free(s.packets);
		free(s.bytes);
		return 1;
	}
	printf("PASS test_mp_walk_session: %zu bytes / %zu packets replayed at "
		   "admit radius %d, forced_reconnects=0, mp_end=0, no reason, "
		   "session still in PLAY after a 900ms pump stall (bp_waits=%u)\n",
		   s.length, s.count, WALK_ADMIT_RADIUS,
		   status.event_backpressure_waits);
	free(s.packets);
	free(s.bytes);
	return 0;
}

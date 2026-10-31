/* IN-009e: the remote session's terminal latch (mp_end / mp_end_phase /
 * mp_end_rx / reason) records the ORIGINAL failure, classified where it
 * happens, not a consequence of the teardown it triggers.
 *
 * Case: a socket WRITE failure in PLAY. The client's own write half is shut
 * down (net_socket_shutdown_write on the live socket), then the peer sends a
 * Beta keep-alive, which the client must echo [740c583 Packet0KeepAlive]. The
 * echo's send() fails; the read half and the peer stay open, so the only
 * thing that can end this session is that write failure. Before IN-009e the
 * write thread published a bare error and shut the socket down, and the
 * reader's EOF from that shutdown (mp_end 2/3) or the pump's side-slot latch
 * (mp_end 6) became the recorded cause.
 *
 * The harness (socket fixture, init) is the existing S0 suite with its main
 * renamed, the same pattern as test_mp_chunk_order. */
#define main existing_mp_suite_main
#include "test_mp_s0_env.c"
#undef main

struct end_peer_fixture {
	SOCKET listener;
	atomic_bool write_half_closed; /* peer saw the client's FIN */
	atomic_bool release;           /* test finished asserting */
	bool quiet; /* case 2: send nothing after the FIN */
	uint8_t bad_id; /* case 2: unsupported packet id sent on send_bad */
	atomic_bool send_bad;
	bool ok;
};

/* Bytes the peer sends before the client's write failure, i.e. the exact
 * rx_bytes the latch must record: offline handshake reply (5) + keep-alive
 * (1) + login response (16) + the keep-alive that provokes the echo (1). */
#define END_PEER_RX_AT_FAILURE 23u

static void* end_peer_entry(void* user) {
	struct end_peer_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	static const uint8_t expected_handshake[]
		= {0x02, 0x00, 0x04, 0x00, 'C', 0x00, 'a', 0x00, 'v', 0x00, 'e'};
	static const uint8_t offline_reply[] = {0x02, 0x00, 0x01, 0x00, '-'};
	static const uint8_t expected_login[] = {0x01, 0x00, 0x00, 0x00, 0x0E,
		0x00, 0x04, 0x00, 'C', 0x00, 'a', 0x00, 'v', 0x00, 'e',
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	static const uint8_t keep_alive[] = {0x00};
	static const uint8_t login_response[] = {0x01, 0x00, 0x00, 0x00, 0x2A,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x0F,
		0x00};
	uint8_t input[64];
	if(!socket_read_exact(peer, input, sizeof(expected_handshake))
	   || memcmp(input, expected_handshake, sizeof(expected_handshake)) != 0
	   || !socket_write_fragmented(peer, offline_reply, sizeof(offline_reply))
	   || !socket_read_exact(peer, input, sizeof(expected_login))
	   || memcmp(input, expected_login, sizeof(expected_login)) != 0
	   || !socket_write_fragmented(peer, keep_alive, sizeof(keep_alive))
	   || !socket_read_exact(peer, input, 1) || input[0] != 0x00
	   || !socket_write_fragmented(peer, login_response,
								   sizeof(login_response)))
		goto done;
	/* Case 2: once the test has seen PLAY, feed the LIVE reader an
	 * unsupported packet id so the production read loop itself calls
	 * remote_read_decode_error and exits. */
	if(fixture->bad_id) {
		while(!atomic_load(&fixture->send_bad))
			thread_msleep(1);
		if(!socket_write_fragmented(peer, &fixture->bad_id, 1))
			goto done;
	}
	/* Drain the client's PLAY frames until its write half closes (recv 0):
	 * that FIN is the test's net_socket_shutdown_write, never a teardown. */
	for(;;) {
		uint8_t byte;
		int count = recv(peer, (char*)&byte, 1, 0);
		if(count == 0)
			break;
		if(count < 0)
			goto done;
	}
	atomic_store(&fixture->write_half_closed, true);
	/* Provoke a client send on the shut write half. */
	if(!fixture->quiet
	   && !socket_write_fragmented(peer, keep_alive, sizeof(keep_alive)))
		goto done;
	fixture->ok = true;
	/* Hold the connection open: the session must end on the write failure
	 * alone, not on anything this peer does. */
	while(!atomic_load(&fixture->release))
		thread_msleep(1);
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static bool end_latched(const struct server_remote_status* status) {
	return status->mp_end != 0 && status->write_thread_exited;
}

static bool reader_joined(const struct server_remote_status* status) {
	return status->read_thread_exited;
}

static void test_write_failure_reported_as_itself(void) {
	struct server_remote remote;
	memset(&remote, 0, sizeof(remote));
	assert(server_remote_init(&remote));
	uint16_t port;
	struct end_peer_fixture peer = {.listener = make_listener(&port)};
	atomic_init(&peer.write_half_closed, false);
	atomic_init(&peer.release, false);
	struct thread peer_thread;
	assert(thread_create(&peer_thread, end_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	assert(wait_for(&remote, is_play, 10000));

	struct server_remote_impl* impl = remote.impl;
	thread_mutex_lock(&impl->lock);
	net_socket_t live = impl->socket;
	uint32_t generation = impl->generation;
	thread_mutex_unlock(&impl->lock);
	assert(live != NET_SOCKET_INVALID);
	net_socket_shutdown_write(live);

	bool ended = wait_for(&remote, end_latched, 10000);
	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	/* Now let the consequence arrive: the peer closes, the reader (still
	 * blocked in recv -- a local shutdown does not wake it on Windows) sees
	 * EOF. Its own first-wins latch must leave the recorded cause alone. */
	atomic_store(&peer.release, true);
	thread_join(&peer_thread);
	assert(peer.ok && atomic_load(&peer.write_half_closed));
	assert(ended);
	assert(wait_for(&remote, reader_joined, 10000));
	struct server_remote_status after;
	server_remote_get_status(&remote, &after);
	assert(after.mp_end == status.mp_end
		   && after.mp_end_phase == status.mp_end_phase
		   && after.mp_end_rx == status.mp_end_rx
		   && strcmp(after.reason, status.reason) == 0);

	char want_reason[128];
	snprintf(want_reason, sizeof want_reason,
			 "socket write failed (-1), play, rx=%u, gen=%u",
			 END_PEER_RX_AT_FAILURE, generation);
	bool exact = status.mp_end == SERVER_REMOTE_END_SEND
		&& status.mp_end_phase == (uint32_t)REMOTE_PLAY
		&& status.mp_end_rx == END_PEER_RX_AT_FAILURE
		&& strcmp(status.reason, want_reason) == 0;
	if(!exact) {
		printf("IN-009e RED: write failure reported as mp_end=%u phase=%u "
			   "rx=%u reason=\"%s\"; want mp_end=%u phase=%u rx=%u "
			   "reason=\"%s\"\n",
			   status.mp_end, status.mp_end_phase, status.mp_end_rx,
			   status.reason, SERVER_REMOTE_END_SEND, (unsigned)REMOTE_PLAY,
			   END_PEER_RX_AT_FAILURE, want_reason);
		assert(exact);
	}
	printf("PASS write failure latched as itself: mp_end=%u phase=%u rx=%u "
		   "reason=\"%s\"\n", status.mp_end, status.mp_end_phase,
		   status.mp_end_rx, status.reason);

	server_remote_cancel(&remote);
	server_remote_get_status(&remote, &status);
	/* Cancel/teardown is consequence: it must not rewrite the latch. */
	assert(status.mp_end == SERVER_REMOTE_END_SEND
		   && status.mp_end_rx == END_PEER_RX_AT_FAILURE);
	server_remote_destroy(&remote);
}

/* Sol IN-009e review P2: an EARLIER terminal error keeps the whole record.
 * The live reader's remote_read_decode_error publishes "bad or unsupported P0 packet" into the
 * error side-slot without setting mp_end or stop_now; before the pump has
 * consumed it, an outstanding send fails. The write failure is then the
 * consequence: code, phase, rx and reason must all describe the decoder
 * error, never mp_end 7 paired with the decoder's text. */
#define END_PEER_RX_AT_PLAY 22u /* handshake reply + keep-alive + login */

static void test_earlier_error_keeps_the_record(void) {
	struct server_remote remote;
	memset(&remote, 0, sizeof(remote));
	assert(server_remote_init(&remote));
	uint16_t port;
	struct end_peer_fixture peer = {.listener = make_listener(&port),
									.quiet = true,
									.bad_id = 0x7E};
	atomic_init(&peer.write_half_closed, false);
	atomic_init(&peer.release, false);
	atomic_init(&peer.send_bad, false);
	struct thread peer_thread;
	assert(thread_create(&peer_thread, end_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	assert(wait_for(&remote, is_play, 10000));

	struct server_remote_impl* impl = remote.impl;
	thread_mutex_lock(&impl->lock);
	net_socket_t live = impl->socket;
	uint32_t rx_at_play
		= atomic_load_explicit(&impl->rx_bytes, memory_order_relaxed);
	thread_mutex_unlock(&impl->lock);
	assert(live != NET_SOCKET_INVALID && rx_at_play == END_PEER_RX_AT_PLAY);

	/* 1. The earlier error, through the LIVE reader: the peer sends an
	 * unsupported packet id, the production read loop calls
	 * remote_read_decode_error and exits. Wait (bounded, no pump) for that
	 * exit so no reader can wake on the later shutdown and latch 2/3. */
	atomic_store(&peer.send_bad, true);
	bool reader_out = false;
	bool error_published = false;
	for(int i = 0; i < 10000 && !reader_out; i++) {
		thread_mutex_lock(&impl->lock);
		reader_out = impl->read_thread_exited;
		error_published = impl->error_pending;
		thread_mutex_unlock(&impl->lock);
		if(!reader_out)
			thread_msleep(1);
	}
	assert(reader_out && error_published);
	uint32_t rx_at_error
		= atomic_load_explicit(&impl->rx_bytes, memory_order_relaxed);
	assert(rx_at_error == END_PEER_RX_AT_PLAY + 1u);
	/* 2. A genuine send failure before any pump runs. */
	net_socket_shutdown_write(live);
	struct mp_frame frame;
	assert(mp_encode_keep_alive(&frame));
	assert(queue_frame(impl, &frame));
	bool writer_out = false;
	for(int i = 0; i < 10000 && !writer_out; i++) {
		thread_mutex_lock(&impl->lock);
		writer_out = impl->write_thread_exited;
		thread_mutex_unlock(&impl->lock);
		if(!writer_out)
			thread_msleep(1);
	}
	assert(writer_out);
	thread_mutex_lock(&impl->lock);
	uint32_t end_before_pump = impl->mp_end;
	thread_mutex_unlock(&impl->lock);

	/* 3. The pump consumes the side-slot. */
	bool ended = wait_for(&remote, end_latched, 10000);
	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	atomic_store(&peer.release, true);
	thread_join(&peer_thread);
	assert(peer.ok && ended);

	char want_reason[128];
	snprintf(want_reason, sizeof want_reason,
			 "bad or unsupported P0 packet id=0x7E phase=0");
	bool exact = end_before_pump == 0
		&& status.mp_end == SERVER_REMOTE_END_PROTOCOL
		&& status.mp_end_phase == (uint32_t)REMOTE_PLAY
		&& status.mp_end_rx == rx_at_error
		&& strcmp(status.reason, want_reason) == 0;
	if(!exact) {
		printf("IN-009e RED: earlier decoder error lost its record to a later "
			   "write failure: write thread latched mp_end=%u; recorded "
			   "mp_end=%u phase=%u rx=%u reason=\"%s\"; want mp_end=%u "
			   "phase=%u rx=%u reason=\"%s\"\n",
			   end_before_pump, status.mp_end, status.mp_end_phase,
			   status.mp_end_rx, status.reason, SERVER_REMOTE_END_PROTOCOL,
			   (unsigned)REMOTE_PLAY, rx_at_error, want_reason);
		assert(exact);
	}
	printf("PASS earlier decoder error keeps the record: mp_end=%u phase=%u "
		   "rx=%u reason=\"%s\"\n", status.mp_end, status.mp_end_phase,
		   status.mp_end_rx, status.reason);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(svin_init() && clin_init());
	blocks_init();
	items_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	test_write_failure_reported_as_itself();
	test_earlier_error_keeps_the_record();
	puts("test_mp_end_reason: PASS");
	return 0;
}

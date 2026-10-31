/* First-failure and 64-byte liveness regression tests; included after socket double. */
static void test_sender_failure_trace(void) {
	hw_telemetry_test_trace_reset();
	hw_telemetry_note_frame(1234);
	struct hw_telemetry_heartbeat hb = {.tick = 8};
	hw_telemetry_emit_heartbeat(&hb);
	const int results[] = {3, 2, -EAGAIN};
	struct socket_send_double sock = {.results=results, .result_count=3, .hidden_on_error=1};
	struct hw_telemetry_send_fail_state st = {0};
	const char line[] = "{\"event\":\"heartbeat\",\"tick\":8}";
	bool close_socket;
	assert(!hw_telemetry_test_send_line_with_state(line, socket_send, &sock, &st, &close_socket));
	const struct cw_tel_trace* t = hw_telemetry_test_trace();
	assert(t->reason == CW_TEL_SEND && t->error == -EAGAIN && t->result == -EAGAIN);
	assert(t->frame == 1234 && t->tick == 8);
	assert(t->queued == 5 && t->length == strlen(line));
	assert(strcmp(t->kind, "heartbeat") == 0);
	assert(t->last[0] == 3 && t->last[1] == 2 && t->last[2] == -EAGAIN);
	// 2026-09-07: one transient records its evidence and abandons the line,
	// but no longer quiesces the session (see hw_telemetry_send_fail_classify).
	assert(!st.quiesced && st.fail_count == 1 && !close_socket && st.resync);
	assert(sock.calls == 3); /* negative never retried, including hidden prefix */
	assert(sock.used == 6 && !memcmp(sock.wire, line, sock.used));
	// The NEXT line resyncs the framing with a leading LF -- the abandoned
	// 6-byte prefix becomes one malformed NDJSON line, nothing is duplicated,
	// and the stream keeps flowing.
	assert(hw_telemetry_test_send_line_with_state(line, socket_send, &sock, &st, &close_socket));
	assert(sock.calls == 6 && !st.resync && st.fail_count == 1);
	assert(sock.wire[6] == '\n');
	assert(!memcmp(sock.wire + 7, line, strlen(line)));
	assert(sock.used == 7 + strlen(line) + 1);
	struct cw_tel_trace before = *t;
	struct partial_send_capture zero = {0};
	assert(!hw_telemetry_test_send_line_with_state("later", zero_send_bytes, &zero, NULL, NULL));
	assert(memcmp(&before, t, sizeof(before)) == 0); /* first failure survives */
	char rendered[512];
	assert(cw_tel_trace_format(rendered, sizeof(rendered), t, "trace-test") < sizeof(rendered));
	assert(strstr(rendered, "partial_unknown=1"));
	// Zero also discards SDK-internal progress. Fail once with unknown prefix.
	hw_telemetry_test_trace_reset();
	const int zero_result[] = {0};
	sock = (struct socket_send_double){.results=zero_result,
		.result_count=1, .hidden_on_error=2};
	st = (struct hw_telemetry_send_fail_state){0};
	assert(!hw_telemetry_test_send_line_with_state(line, socket_send, &sock, &st, NULL));
	assert(!hw_telemetry_test_send_line_with_state(line, socket_send, &sock, &st, NULL));
	assert(sock.calls == 1 && sock.used == 2 && !memcmp(sock.wire, line, 2));
	t = hw_telemetry_test_trace();
	assert(t->reason == CW_TEL_SEND && t->result == 0 && t->error == -EIO);
	cw_tel_trace_format(rendered, sizeof(rendered), t, "zero-test");
	assert(strstr(rendered, "partial_unknown=1"));

	char alive[64]; cw_tel_alive_format(alive, UINT32_MAX, UINT32_MAX);
	assert(alive[0] == '\n' && alive[63] == '\n');
	assert(!memchr(alive, 0, sizeof(alive)));
	assert(memcmp(alive+1, "{\"event\":\"alive\"", 16) == 0);
	puts("PASS first-failure trace: EAGAIN, exact known prefix, last 3 results; alive 64 bytes");
}


/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include "platform/service_observer.h"
#include "platform/input_owner.h"
#include "parson/parson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static uint64_t now = 100;
static unsigned clock_reads, rows;
static bool print_rows, refuse, mid_tick, skip_states, freeze_on_clock;
static bool retry_exact;
static char refused_row[2304];
static char tape[262144];
static size_t used;
static uint64_t clock_us(void) {
	clock_reads++;
	if(freeze_on_clock) {
		freeze_on_clock = false;
		service_observer_request(true);
	}
	return now;
}
static bool emit(const char* line) {
	assert(strlen(line) <= 2303);
	if(refuse) {
		if(retry_exact) {
			if(refused_row[0]) assert(!strcmp(refused_row, line));
			else snprintf(refused_row, sizeof refused_row, "%s", line);
		}
		rows++; return false;
	}
	if(retry_exact && refused_row[0]) {
		assert(!strcmp(refused_row, line)); refused_row[0] = 0;
	}
	if(print_rows) printf("%.*s,\"run_id\":\"so-test\"}\n", (int)strlen(line)-1, line);
	if(used + strlen(line) + 2 < sizeof tape) {
		memcpy(tape + used, line, strlen(line)); used += strlen(line); tape[used++] = '\n'; tape[used] = 0;
	}
	rows++;
	return !refuse;
}
static void reset(void) {
	used = rows = clock_reads = 0; tape[0] = refused_row[0] = 0;
	now = 100; refuse = retry_exact = false;
	service_observer_configure(true, "abc_06.27", 17);
}
static void scenario(void) {
	reset();
	service_observer_frame(SO_STARTUP);
	service_observer_poll(true); now += 50; service_observer_poll(false);
	service_observer_receipt(0, true, true);
	now += 1000; service_observer_consume(0, 0, 1);
	service_observer_submit(101);
	now += 1000; service_observer_display(101);
	service_observer_display(101); /* Does not duplicate first presentation. */
	service_observer_receipt(1, false, false);
	service_observer_consume(1, 1, 2); /* Missing/disconnected is no age. */
	service_observer_audio(SO_AUDIO_RESET, false, 0);
	service_observer_audio(SO_AUDIO_PRIME, true, 200000);
	service_observer_tick(true);
	uint64_t begin = now;
	service_observer_phase(4, now);
	service_observer_checkpoint(7, 9, 81, 0, UINT32_MAX);
	service_observer_checkpoint(7, 9, 81, 1, 0);
	service_observer_boost(0, (uint32_t)now);
	now += 65000;
	service_observer_boost(2, (uint32_t)now);
	service_observer_wait(0, begin, now);
	service_observer_components(4000, 60000);
	service_observer_checkpoint(7, 9, 81, 2, 1);
	service_observer_tick(false);
	now = 75100; service_observer_frame(SO_GAMEPLAY);
	service_observer_poll(true); now += 50; service_observer_poll(false);
	service_observer_audio(SO_AUDIO_SERVICE, true, 50000);
	now += 60000; service_observer_audio(SO_AUDIO_UNDERRUN, true, 0);
	service_observer_audio(SO_AUDIO_TRACK_STOP, false, 0);
	if(!skip_states) {
		now += 1000; service_observer_frame(SO_LOADING);
		now += 1000; service_observer_frame(SO_MENU);
	}
	now += 1000; service_observer_frame(SO_SHUTDOWN);
	service_observer_audio(SO_AUDIO_SHUTDOWN, false, 0);
	service_observer_audio(SO_AUDIO_RESET, false, 0);
	service_observer_audio(SO_AUDIO_PRIME, true, 70000);
	service_observer_audio(SO_AUDIO_STOP, false, 0);
	now += 1000000;
	if(mid_tick) { service_observer_tick(true); now += 25; }
	service_observer_flush(true, emit);
}
/* Fake clock only: positive coverage schema, never a hardware workload claim. */
static void exposure_rows(void) {
	reset(); service_observer_frame(SO_STARTUP);
	service_observer_audio(SO_AUDIO_PRIME, true, 500000000);
	now += 100; service_observer_frame(SO_GAMEPLAY);
	for(unsigned k = 0; k < 8; k++) {
		now += 45000000; service_observer_frame(SO_GAMEPLAY);
		service_observer_poll(true); now += 50; service_observer_poll(false);
		service_observer_receipt(0, true, true); service_observer_consume(0, 0, 1|2|4|8);
		service_observer_submit(101); now += 100; service_observer_display(101);
		service_observer_audio(SO_AUDIO_SERVICE, true, 500000000);
		service_observer_tick(true);
		service_observer_checkpoint(1, k+1, (k+1)*600, 0, UINT32_MAX);
		service_observer_checkpoint(1, k+1, (k+1)*600, 1, 0);
		service_observer_boost(0, (uint32_t)now);
		now += 200001; service_observer_boost(2, (uint32_t)now);
		service_observer_components(1000, 1000);
		service_observer_checkpoint(1, k+1, (k+1)*600, 2, 1);
		service_observer_tick(false);
		service_observer_request(false); while(service_observer_pump(emit)) { }
	}
	now += 1000000; service_observer_audio(SO_AUDIO_TRACK_STOP, false, 0);
	service_observer_frame(SO_SHUTDOWN); service_observer_flush(true, emit);
}
static void clock_and_final_edges(void) {
	reset(); now = UINT64_C(0xffffffff) - 50;
	service_observer_frame(SO_GAMEPLAY); service_observer_tick(true);
	service_observer_boost(0, (uint32_t)now);
	now += 200; service_observer_boost(2, (uint32_t)now);
	service_observer_components(100, 100); service_observer_tick(false);
	service_observer_request(false); assert(service_observer_pump(emit));
	/* Final arrives mid-batch: old batch completes, then a distinct final tail. */
	service_observer_request(true);
	for(unsigned k = 0; k < 84 && service_observer_pump(emit); k++) { }
	assert(!service_observer_hold_transport());
	assert(strstr(tape, "[38,1,200,0,200,200,149,1,"));
	assert(!strstr(tape, "\"loss\":[1,"));
	reset(); service_observer_frame(SO_GAMEPLAY);
	now += UINT64_C(0x100000000); service_observer_frame(SO_MENU);
	service_observer_flush(true, emit);
	assert(strstr(tape, "\"loss\":[0,0,0,1]")); /* Never silently narrow >32-bit spans. */
}
static void reset(void);
static uint32_t metric_count(unsigned id);
static void transport_retention(void) {
	reset(); service_observer_frame(SO_GAMEPLAY);
	service_observer_tick(true); now += 20; service_observer_tick(false);
	refuse = retry_exact = true;
	assert(!service_observer_pump(emit));
	assert(!used && service_observer_hold_transport());
	service_observer_request(false);
	now += 2000000; service_observer_request(false);
	for(unsigned k = 0; k < 20; k++) assert(!service_observer_pump(emit));
	/* A late client must get the original edge, exactly once and as seq 1. */
	now += 500000; refuse = false;
	while(service_observer_pump(emit)) { }
	service_observer_flush(true, emit);
	assert(strstr(tape, "\"seq\":1,"));
	assert(strstr(tape, "\"loss\":[0,0,0,0]"));
	assert(metric_count(SO_TICK_US) == 1);
	assert(!service_observer_hold_transport());
	puts("PASS late client/refusal retains exact sequence, context and metric until delivery");
}
static void final_refusal(void) {
	reset(); service_observer_frame(SO_GAMEPLAY);
	refuse = true; service_observer_request(true);
	assert(!service_observer_pump(emit));
	assert(service_observer_hold_transport());
	now += 500000; refuse = false;
	while(service_observer_pump(emit)) { }
	assert(strstr(tape, "\"seq\":1,") && !service_observer_hold_transport());
	reset(); service_observer_frame(SO_GAMEPLAY);
	refuse = true; service_observer_request(true);
	assert(!service_observer_pump(emit) && service_observer_hold_transport());
	now += 1000001;
	assert(!service_observer_pump(emit) && !service_observer_hold_transport());
	assert(service_observer_transport_failed());
	assert(!used); /* Timeout cannot invent a final row. */
	puts("PASS final refusal recovers; no-client final timeout bounded without false final");
}
static bool ambiguous_emit(const char* row) {
	(void)row; rows++; service_observer_transport_abort(); return false;
}
static void terminal_transport(void) {
	reset(); service_observer_frame(SO_GAMEPLAY); service_observer_request(true);
	assert(!service_observer_pump(ambiguous_emit));
	assert(service_observer_transport_failed() && !service_observer_hold_transport());
	unsigned attempted = rows;
	for(unsigned k = 0; k < 100; k++) assert(!service_observer_pump(emit));
	assert(rows == attempted && !used);
	puts("PASS ambiguous TCP failure is terminal; no retry, invented final or waiting");
}
static void* teardown_server(void* unused) {
	(void)unused; service_observer_tick(true); now += 10;
	service_observer_tick(false); return NULL;
}
static void shutdown_boundary(void) {
	reset(); service_observer_frame(SO_GAMEPLAY);
	pthread_t server; assert(!pthread_create(&server, NULL, teardown_server, NULL));
	assert(!pthread_join(server, NULL));
	service_observer_state(SO_SHUTDOWN);
	for(unsigned i = 0; i < 4; i++) service_observer_wait(0, now, now+10);
	service_observer_flush(true, emit);
	assert(strstr(tape, "\"loss\":[0,0,0,0]"));
	assert(strstr(tape, "\"teardown_unobserved\":4"));
	puts("PASS shutdown MAIN waits explicitly unobserved; completed SERVER tick retained");
	reset(); service_observer_frame(SO_GAMEPLAY);
	assert(!pthread_create(&server, NULL, teardown_server, NULL));
	assert(!pthread_join(server, NULL)); service_observer_state(SO_SHUTDOWN);
	service_observer_wait(3, now, now+10); service_observer_phase(SO_PHASES, now);
	service_observer_flush(true, emit);
	assert(strstr(tape, "\"loss\":[2,0,0,0]"));
	assert(strstr(tape, "\"teardown_unobserved\":0"));
	puts("PASS invalid shutdown phase/wait kinds remain real missing observations");
}
static void configuration_identity(void) {
	JSON_Value* a = json_parse_string("{\"debug\":{\"service_observer_int\":1},\"music\":true}");
	JSON_Value* b = json_parse_string("{\"debug\":{\"service_observer_int\":1},\"music\":false}");
	assert(a && b);
	uint32_t h = service_observer_config_hash(a);
	assert(h && h == service_observer_config_hash(a) && h != service_observer_config_hash(b));
	json_value_free(a); json_value_free(b);
	char deep[40]; memset(deep, '[', 18); deep[18] = '0'; memset(deep+19, ']', 18); deep[37] = 0;
	a = json_parse_string(deep); assert(a && !service_observer_config_hash(a)); json_value_free(a);
	reset(); service_observer_configure(true, "identity-unavailable", 0);
	service_observer_flush(true, emit); assert(strstr(tape, "\"loss\":[1,0,0,0]"));
}
static uint32_t metric_count(unsigned id) {
	uint32_t count = 0;
	for(char* line = tape; *line;) {
		char* end = strchr(line, '\n'); assert(end); *end = 0;
		JSON_Value* v = json_parse_string(line); assert(v);
		JSON_Array* a = json_object_get_array(json_value_get_object(v), "metrics");
		for(size_t k = 0; k < json_array_get_count(a); k++) {
			JSON_Array* m = json_array_get_array(a, k);
			if(json_array_get_number(m, 0) == id) count = (uint32_t)json_array_get_number(m, 1);
		}
		json_value_free(v); *end = '\n'; line = end + 1;
	}
	return count;
}
static void consume_bound(void) {
	reset(); service_observer_frame(SO_GAMEPLAY); service_observer_receipt(0, true, true);
	for(unsigned k = 0; k < 1000; k++) service_observer_consume(0, 0, 1u << (k % 4));
	service_observer_submit(101); now += 100; service_observer_display(101);
	service_observer_flush(true, emit);
	assert(metric_count(SO_CONSUME_AGE) == 1 && metric_count(SO_REUSE_AGE) == 0);
	assert(strstr(tape, ",0,0,15,2,")); /* all actions retained on first presentation */
	service_observer_configure(true, "reuse", 17); used = rows = 0;
	service_observer_frame(SO_GAMEPLAY); service_observer_receipt(0, true, true);
	service_observer_consume(0, 0, 1); now += 100; service_observer_frame(SO_GAMEPLAY);
	service_observer_consume(0, 0, 1); service_observer_flush(true, emit);
	assert(metric_count(SO_CONSUME_AGE) == 1 && metric_count(SO_REUSE_AGE) == 1);
	puts("PASS bounded owner/device/frame consumption and real next-frame reuse");
}
static void multi_present(void) {
	reset(); service_observer_frame(SO_GAMEPLAY);
	service_observer_receipt(0, true, true); service_observer_receipt(4, true, true);
	service_observer_receipt(1, true, true);
	service_observer_consume(0, 0, 1); service_observer_consume(0, 4, 2);
	service_observer_consume(1, 1, 4);
	service_observer_submit(101); now += 100; service_observer_display(101);
	service_observer_display(101); service_observer_flush(true, emit);
	assert(metric_count(SO_PRESENT_AGE) == 3);
	puts("PASS all devices and split owners reach first framebuffer presentation exactly once");
}
static void foreign_spans(void) {
	reset(); service_observer_frame(SO_GAMEPLAY);
	service_observer_phase(4, now); service_observer_wait(0, now, now+10);
	service_observer_flush(true, emit);
	assert(strstr(tape, "\"loss\":[2,0,0,0]"));
	puts("PASS unsupported phase/wait writers leave explicit missing evidence");
}
static void final_mid_tick(void) {
	reset(); now += 10; service_observer_frame(SO_GAMEPLAY);
	service_observer_tick(true); now += 100;
	service_observer_components(40, 60); service_observer_tick(false);
	uint64_t boundary = now;
	now += 50; service_observer_tick(true); now += 25;
	service_observer_request(true);
	service_observer_tick(false); /* Frozen partial tick must not become a sample. */
	while(service_observer_pump(emit)) { }
	assert(!service_observer_hold_transport());
	assert(metric_count(SO_TICK_US) == 1);
	assert(strstr(tape, "\"loss\":[0,0,0,0]"));
	char end[80]; snprintf(end, sizeof end, "\"tick_end_us\":%llu", (unsigned long long)boundary);
	assert(strstr(tape, end));
	assert(strstr(tape, "\"completed_ticks\":1"));
	/* Deterministically interleave MAIN freeze after tick(false)'s initial
	 * enabled check but before its completed-boundary publication. */
	reset(); service_observer_tick(true); now += 10;
	freeze_on_clock = true; service_observer_tick(false);
	while(service_observer_pump(emit)) { }
	assert(metric_count(SO_TICK_US) == 0);
	assert(strstr(tape, "\"tick_end_us\":0,\"completed_ticks\":0"));
	assert(strstr(tape, "\"loss\":[0,0,0,0]"));
	reset(); service_observer_tick(true); service_observer_tick(true);
	now += 20; service_observer_tick(false); service_observer_flush(true, emit);
	assert(strstr(tape, "\"loss\":[1,0,0,0]"));
	puts("PASS final mid-tick excludes partial tick, retains completed boundary and genuine gaps");
}
static void off(void) {
	service_observer_configure(false, NULL, 0); unsigned before = clock_reads;
	SERVICE_OBSERVER(service_observer_frame(SO_GAMEPLAY));
	service_observer_tick(true); service_observer_poll(true);
	service_observer_receipt(0, true, true); service_observer_consume(0, 0, 0);
	service_observer_boost(2, 1); service_observer_flush(true, emit);
	SERVICE_OBSERVER(service_observer_submit_safe(++clock_reads, 1));
	assert(clock_reads == before && !rows);
}
static void scanned_frame_loss(void) {
	reset(); service_observer_frame(SO_GAMEPLAY); service_observer_receipt(0, true, true);
	service_observer_consume(0, 0, 1); service_observer_submit_safe(101, 0);
	now += 10; service_observer_frame(SO_GAMEPLAY); service_observer_receipt(0, true, true);
	service_observer_consume(0, 0, 2); service_observer_submit_safe(101, 101);
	now += 10; service_observer_display(101); service_observer_flush(true, emit);
	assert(strstr(tape, "\"loss\":[1,0,0,0]"));
	assert(metric_count(SO_PRESENT_AGE) == 1);
	assert(strstr(tape, ",1,0,0,1,2,")); /* first input survives refused overwrite */
	puts("PASS scanned framebuffer records loss and preserves VI snapshot without abort");
}
static void loss(void) {
	reset(); service_observer_tick(true);
	for(unsigned k = 0; k < 100; k++) service_observer_checkpoint(1, k, 1, 0, UINT32_MAX);
	now += 1000000; service_observer_flush(false, emit);
	assert(strstr(tape, "\"loss\":[0,68,0,0]")); /* 32 server slots, no overwrite */
	service_observer_tick(false);
	unsigned first = rows; service_observer_flush(false, emit); assert(rows == first + 1);
	/* The new worst tick edge drains without another aggregate request. */
	first = rows;
	refuse = true; now += 1000000; service_observer_flush(false, emit);
	unsigned rejected = rows - first; assert(rejected > 0);
	refuse = false; now += 1000000; service_observer_flush(true, emit);
	assert(strstr(tape, "\"loss\":[0,68,0,0]")); /* Deferred packet delivered, true overflow retained. */
	reset(); service_observer_consume(4, 10, 0);
	service_observer_phase(100, now); /* Before owner registration: explicit missing */
	now += 1000000; service_observer_flush(true, emit);
	assert(strstr(tape, "\"loss\":[2,0,0,0]"));
}
static void cost(void) {
	service_observer_test_clock(NULL);
	const unsigned n = 100000, hooks = 17;
	const char* names[] = {"frame", "poll_enter", "poll_exit", "receipt", "consume", "audio",
		"tick_begin", "components", "phase", "wait", "boost_begin", "boost_end", "cap",
		"checkpoint", "submit", "display", "tick_end"};
	for(unsigned on = 0; on < 2; on++) {
		uint64_t total[17] = {0}; uint32_t maximum[17] = {0};
		service_observer_configure(on, "bench", 1);
		for(unsigned k = 0; k < n; k++) for(unsigned h = 0; h < hooks; h++) {
			uint64_t start = service_observer_now();
			SERVICE_OBSERVER(switch(h) {
				case 0: service_observer_frame(SO_GAMEPLAY); break;
				case 1: service_observer_poll(true); break;
				case 2: service_observer_poll(false); break;
				case 3: service_observer_receipt(0, true, true); break;
				case 4: service_observer_consume(0, 0, 1); break;
				case 5: service_observer_audio(SO_AUDIO_SERVICE, true, 100000); break;
				case 6: service_observer_tick(true); break;
				case 7: service_observer_components(1, 2); break;
				case 8: service_observer_phase(4, service_observer_now()); break;
				case 9: service_observer_wait(0, start, service_observer_now()); break;
				case 10: service_observer_boost(0, (uint32_t)service_observer_now()); break;
				case 11: service_observer_boost(1, (uint32_t)service_observer_now()); break;
				case 12: service_observer_boost(2, (uint32_t)service_observer_now()); break;
				case 13: service_observer_checkpoint(1, k, k, 2, 0); break;
				case 14: service_observer_submit(101); break;
				case 15: service_observer_display(101); break;
				case 16: service_observer_tick(false); break;
			});
			uint32_t duration = (uint32_t)(service_observer_now() - start);
			total[h] += duration; if(duration > maximum[h]) maximum[h] = duration;
		}
		for(unsigned h = 0; h < hooks; h++) {
			printf("PC hook %s observer=%s n=%u mean_us=%.4f max_observed_us=%u (includes timer/dispatch; unsubtracted)\n",
				names[h], on ? "ON" : "OFF", n, (double)total[h]/n, maximum[h]);
			assert((double)total[h]/n <= 10.0);
		}
	}
	printf("observer total preallocated storage=%zu bytes; Wii hook/aggregate cost unmeasured\n", service_observer_storage_bytes());
}
static void continuous_context(void) {
	reset(); service_observer_frame(SO_GAMEPLAY);
	for(unsigned k = 0; k < 100; k++) {
		service_observer_tick(true);
		service_observer_boost(0, (uint32_t)now);
		now += 100; service_observer_boost(1, (uint32_t)now);
		service_observer_components(10, 20); service_observer_tick(false);
		/* Existing sender services its bounded pump between ticks; no new
		 * aggregate request is due yet. No controller is actually assigned. */
		struct input_device device = {0};
		service_observer_device(0, 1, &device);
		for(unsigned p = 0; p < 20 && service_observer_pump(emit); p++) { }
		now += 10000;
	}
	service_observer_flush(true, emit);
	unsigned boosts = 0;
	for(char* line = tape; *line;) {
		char* end = strchr(line, '\n'); assert(end); *end = 0;
		JSON_Value* value = json_parse_string(line); assert(value);
		JSON_Object* row = json_value_get_object(value);
		JSON_Array* loss = json_object_get_array(row, "loss");
		for(unsigned k = 0; k < 4; k++) assert(json_array_get_number(loss, k) == 0);
		JSON_Array* context = json_object_get_array(row, "context");
		for(size_t k = 0; k < json_array_get_count(context); k++)
			boosts += json_array_get_number(json_array_get_array(context, k), 0) == SO_BOOST_EDGE;
		json_value_free(value); *end = '\n'; line = end + 1;
	}
	assert(boosts == 200 && metric_count(SO_TICK_US) == 100);
	assert(strstr(tape, "\"input_unassigned\":100"));
	assert(!service_observer_hold_transport());
	puts("PASS bounded continuous boost context drain, unassigned input census, final tail releases completion fence");
}
static void deferred_metric(void) {
	reset(); service_observer_frame(SO_GAMEPLAY);
	service_observer_tick(true); now += 20;
	service_observer_components(10, 20); service_observer_tick(false);
	/* The only double is the odd publication window, not a metric/sample:
	 * real producer cumulative values are kept intact while sender defers. */
	service_observer_test_metric_busy(SO_TICK_US, true);
	service_observer_request(true);
	while(service_observer_pump(emit)) { }
	assert(service_observer_hold_transport());
	assert(metric_count(SO_TICK_US) == 0);
	service_observer_test_metric_busy(SO_TICK_US, false);
	while(service_observer_pump(emit)) { }
	assert(!service_observer_hold_transport());
	assert(metric_count(SO_TICK_US) == 1);
	assert(!strstr(tape, "\"loss\":[1,"));
	puts("PASS real cumulative metric deferred across injected odd serial; pending final cannot certify before complete publication");
}
int main(int argc, char** argv) {
	service_observer_test_clock(clock_us);
	assert(service_observer_storage_bytes() <= 16384);
	if(argc > 1 && !strcmp(argv[1], "cost")) { cost(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "transport-retention")) { transport_retention(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "final-refusal")) { final_refusal(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "shutdown-boundary")) { shutdown_boundary(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "terminal-transport")) { terminal_transport(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "--x-red")) { continuous_context(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "deferred-metric")) { deferred_metric(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "rows-x")) { print_rows = true; continuous_context(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "rows")) { print_rows = true; scenario(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "rows-exposure")) { print_rows = true; exposure_rows(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "rows-mid-tick")) { print_rows = mid_tick = true; scenario(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "rows-no-menu")) { print_rows = skip_states = true; scenario(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "consume-bound")) { consume_bound(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "multi-present")) { multi_present(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "foreign-spans")) { foreign_spans(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "final-mid-tick")) { final_mid_tick(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "scanned-frame")) { scanned_frame_loss(); return 0; }
	off();
	transport_retention(); final_refusal(); shutdown_boundary(); terminal_transport();
	assert(service_observer_bin(999) == 0 && service_observer_bin(1000) == 1);
	assert(service_observer_bin(49999) == 4 && service_observer_bin(50000) == 5);
	assert(service_observer_bin(UINT32_MAX) == 7);
	scenario(); assert(strstr(tape, "\"presentation\":\"render_submit_proxy\""));
	loss(); clock_and_final_edges(); configuration_identity(); puts("PASS observer fake-clock bins, exact maximum tick/input IDs, first display, OFF no clock, ring overflow, refusal, pending final flush, 32-bit IRQ clock wrap/long-span overflow, config identity/depth-unavailable");
}

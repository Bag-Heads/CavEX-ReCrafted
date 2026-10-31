/* Copyright (c) 2026 CaveWii_v2 contributors */
#include "split_offer_piston_probe.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "server_local.h"
#include "server_local_state.h"
#include "server_world.h"
#include "../block/blocks_data.h"
#include "../entity/combat.h"
#include "../game/difficulty.h"
#include "../platform/time.h"

/* One server producer, one main consumer. Release/acquire publishes POD;
 * overflow latches a terminal failure, never overwrites or blocks. */
static bool armed, hold;
static uint32_t rd, wr, overflow, command;
static struct sop_record fifo[SOP_FIFO_CAP];
static struct {
	uint32_t tick, gc, hc, damage, power_tick;
	bool offered, powered, extended, admitted, playing;
	bool play_failed;
	ptime_t play_start;
	int32_t piston[3];
} S;

bool sop_probe_armed(void) {
	return __atomic_load_n(&armed, __ATOMIC_RELAXED);
}
void sop_probe_disarm(void) {
	__atomic_store_n(&armed, false, __ATOMIC_RELEASE);
}
void sop_probe_reset(void) {
	__atomic_store_n(&armed, false, __ATOMIC_RELEASE);
	__atomic_store_n(&hold, false, __ATOMIC_RELEASE);
	__atomic_store_n(&command, 0, __ATOMIC_RELEASE);
	/* At boot / tests after the owner has stopped, like vsp_reset. */
	rd = wr = overflow = 0;
	memset(&S, 0, sizeof(S));
}
void sop_probe_arm(void) {
	sop_probe_reset();
	__atomic_store_n(&hold, true, __ATOMIC_RELEASE);
	__atomic_store_n(&armed, true, __ATOMIC_RELEASE);
}
bool sop_probe_hold_offer(void) {
	return sop_probe_armed() && __atomic_load_n(&hold, __ATOMIC_ACQUIRE);
}
bool sop_probe_command(enum sop_command cmd) {
	uint32_t empty = 0;
	return sop_probe_armed() && __atomic_compare_exchange_n(&command, &empty,
		(uint32_t)cmd, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}
enum sop_command sop_probe_take_command(void) {
	if(!sop_probe_armed()) return 0;
	enum sop_command cmd = (enum sop_command)__atomic_exchange_n(&command, 0, __ATOMIC_ACQ_REL);
	if(cmd == SOP_CMD_RELEASE) __atomic_store_n(&hold, false, __ATOMIC_RELEASE);
	return cmd;
}
uint32_t sop_probe_overflows(void) {
	return __atomic_load_n(&overflow, __ATOMIC_ACQUIRE);
}
unsigned sop_probe_pending(void) {
	return __atomic_load_n(&wr, __ATOMIC_ACQUIRE)
		- __atomic_load_n(&rd, __ATOMIC_ACQUIRE);
}
bool sop_probe_pop(struct sop_record* out) {
	uint32_t r = __atomic_load_n(&rd, __ATOMIC_RELAXED);
	if(r == __atomic_load_n(&wr, __ATOMIC_ACQUIRE)) return false;
	*out = fifo[r % SOP_FIFO_CAP];
	__atomic_store_n(&rd, r + 1, __ATOMIC_RELEASE);
	return true;
}
void sop_probe_snapshot(struct server_local* s, enum sop_event ev,
	bool ok, const char* reason) {
	if(!sop_probe_armed() || !s) return;
	uint32_t w = __atomic_load_n(&wr, __ATOMIC_RELAXED);
	if(w - __atomic_load_n(&rd, __ATOMIC_ACQUIRE) >= SOP_FIFO_CAP) {
		__atomic_add_fetch(&overflow, 1, __ATOMIC_RELEASE);
		return;
	}
	struct server_player* g = s->guest_actor;
	const struct server_world* world = server_local_world_of(s, g);
	struct sop_record r = {.event = ev, .tick = S.tick, .ok = ok,
		.guest_corrections = S.gc, .host_corrections = S.hc,
		.guest_damage = S.damage, .host_token = s->player.correction_seq,
		.guest_token = g ? g->correction_seq : 0,
		.generation = s->guest_generation, .bound = g != NULL,
		.confirmed = s->guest_confirmed, .gameplay = s->guest_gameplay,
		.finished_loading = g && g->finished_loading,
		.health = g ? g->health : 0, .host_health = s->player.health,
		.difficulty = server_local_difficulty(s),
		.world_slot = world ? world->world_index : 0,
		.world_generation = world ? world->generation : 0,
		.dimension = world ? world->dimension : 0,
		.superflat = world && world->superflat,
		.host = {s->player.x, s->player.y, s->player.z},
		.guest = {g ? g->x : 0, g ? g->y : 0, g ? g->z : 0},
		.extended = S.extended};
	if(g && ev == SOP_FIXTURE) {
		r.piston[0] = (int32_t)floor(g->x) - 1;
		r.piston[1] = (int32_t)floor(g->y - PLAYER_EYE_HEIGHT + 0.01);
		r.piston[2] = (int32_t)floor(g->z);
	}
	if(ev == SOP_FIXTURE) memcpy(S.piston, r.piston, sizeof(S.piston));
	if(S.playing) r.play_ms = (uint32_t)time_diff_ms(S.play_start, time_get());
	snprintf(r.reason, sizeof(r.reason), "%s", reason ? reason : "");
	fifo[w % SOP_FIFO_CAP] = r;
	__atomic_store_n(&wr, w + 1, __ATOMIC_RELEASE);
}
void sop_probe_offer(struct server_local* s) {
	if(sop_probe_armed() && !S.offered) {
		S.offered = true;
		sop_probe_snapshot(s, SOP_OFFERED, true, "accept_delayed_fixture");
	}
}
void sop_probe_correction(bool guest) {
	if(!sop_probe_armed()) return;
	if(guest) S.gc++; else S.hc++;
}
void sop_probe_damage(struct server_local* s, struct server_player* p,
	int old_health, int new_health) {
	if(sop_probe_armed() && p == s->guest_actor && new_health < old_health)
		S.damage += (uint32_t)(old_health - new_health);
}
void sop_probe_powered(void) {
	S.powered = true;
	S.power_tick = S.tick;
}
void sop_probe_play_begin(void) {
	S.playing = true;
	S.play_start = time_get();
}
void sop_probe_server_end(struct server_local* s) {
	if(!sop_probe_armed()) return;
	S.tick++;
	if(S.playing && !S.play_failed && (!s->guest_actor || !s->guest_gameplay
	   || !s->guest_actor->finished_loading || s->guest_actor->health <= 0)) {
		S.play_failed = true;
		sop_probe_snapshot(s, SOP_ERROR, false, "play_admission_lost");
	}
	/* Five real updates include both extension halves and the settle pass.
 * No admission is released until this record is received and graded. */
	if(S.powered && !S.extended && S.tick - S.power_tick >= 5) {
		struct block_data base = {0}, head = {0};
		struct server_world* world = server_local_world_of(s, s->guest_actor);
		bool settled = world
			&& server_world_get_block(world, S.piston[0], S.piston[1], S.piston[2], &base)
			&& server_world_get_block(world, S.piston[0]+1, S.piston[1], S.piston[2], &head)
			&& base.type == BLOCK_PISTON_BASE && (base.metadata & 8)
			&& head.type == BLOCK_PISTON_HEAD;
		S.extended = settled;
		/* A missing or deferred extension cannot certify the predicate. */
		S.powered = false;
		sop_probe_snapshot(s, SOP_EXTENDED, settled,
			settled ? "full_extension_and_settle" : "extension_incomplete");
	}
	if(S.offered && !S.admitted && s->guest_gameplay) {
		S.admitted = true;
		sop_probe_snapshot(s, SOP_ADMITTED, true, "production_gameplay_ready");
	}
}
size_t sop_probe_format(const struct sop_record* r, uint32_t seq,
	char* line, size_t cap) {
	static const char* names[] = {"offered", "fixture", "before", "extended",
		"released", "admitted", "play_begin", "play_end", "error"};
	if(!r || !seq || (unsigned)r->event >= sizeof(names)/sizeof(names[0]))
		return 0;
	for(unsigned i = 0; i < 3; i++)
		if(!isfinite(r->host[i]) || !isfinite(r->guest[i])) return 0;
	int n = snprintf(line, cap,
		"{\"schema\":1,\"profile\":\"%s\",\"event\":\"split_offer_piston.%s\","
		"\"seq\":%u,\"origin\":\"server\",\"tick\":%u,\"generation\":%llu,"
		"\"host\":[%.9g,%.9g,%.9g],\"guest\":[%.9g,%.9g,%.9g],"
		"\"guest_corrections\":%u,\"host_corrections\":%u,\"guest_damage\":%u,"
		"\"host_token\":%u,\"guest_token\":%u,\"health\":%d,\"host_health\":%d,"
		"\"bound\":%s,\"confirmed\":%s,\"gameplay\":%s,\"finished_loading\":%s,"
		"\"extended\":%s,\"play_ms\":%u,\"ok\":%s,\"reason\":\"%s\","
		"\"difficulty\":%d,\"superflat\":%s,\"world_slot\":%u,"
		"\"world_generation\":%u,\"dimension\":%d}",
		SOP_PROFILE, names[r->event], seq, r->tick,
		(unsigned long long)r->generation, r->host[0], r->host[1], r->host[2],
		r->guest[0], r->guest[1], r->guest[2], r->guest_corrections,
		r->host_corrections, r->guest_damage, r->host_token, r->guest_token,
		r->health, r->host_health, r->bound ? "true" : "false",
		r->confirmed ? "true" : "false", r->gameplay ? "true" : "false",
		r->finished_loading ? "true" : "false", r->extended ? "true" : "false",
		r->play_ms, r->ok ? "true" : "false", r->reason, r->difficulty,
		r->superflat ? "true" : "false", r->world_slot, r->world_generation,
		r->dimension);
	return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

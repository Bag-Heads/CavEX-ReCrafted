/* Copyright (c) 2026 CaveWii_v2 contributors */
#ifndef NETWORK_SPLIT_OFFER_PISTON_PROBE_H
#define NETWORK_SPLIT_OFFER_PISTON_PROBE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct server_local;
struct server_player;
#define SOP_PROFILE "split_offer_piston_2m"
#define SOP_RIG_WORLD "cw_split_offer_piston_rig"
#define SOP_LINE_CAP 1536u
#define SOP_FIFO_CAP 16u
enum sop_event { SOP_OFFERED, SOP_FIXTURE, SOP_BEFORE, SOP_EXTENDED,
	SOP_RELEASED, SOP_ADMITTED, SOP_PLAY_BEGIN, SOP_PLAY_END, SOP_ERROR };
enum sop_command { SOP_CMD_BUILD = 1, SOP_CMD_POWER, SOP_CMD_RELEASE,
	SOP_CMD_PLAY_BEGIN, SOP_CMD_PLAY_END };
/* POD snapshots, authored only on the server thread. No borrowed pointers. */
struct sop_record {
	enum sop_event event;
	uint32_t tick, guest_corrections, host_corrections, guest_damage;
	uint32_t host_token, guest_token, play_ms;
	uint64_t generation;
	double host[3], guest[3];
	int health, host_health;
	int difficulty, dimension;
	uint32_t world_slot, world_generation;
	int32_t piston[3];
	bool bound, confirmed, gameplay, finished_loading, extended, ok;
	bool superflat;
	char reason[48];
};
/* Inert when disarmed: no RNG, clock, record or simulation write. */
void sop_probe_arm(void);
void sop_probe_reset(void);
void sop_probe_disarm(void);
bool sop_probe_armed(void);
bool sop_probe_hold_offer(void);
bool sop_probe_command(enum sop_command command);
enum sop_command sop_probe_take_command(void);
bool sop_probe_pop(struct sop_record* out);
uint32_t sop_probe_overflows(void);
unsigned sop_probe_pending(void);
size_t sop_probe_format(const struct sop_record* r, uint32_t seq,
	char* line, size_t cap);
void sop_probe_snapshot(struct server_local* s, enum sop_event event,
	bool ok, const char* reason);
void sop_probe_offer(struct server_local* s);
void sop_probe_correction(bool guest);
void sop_probe_damage(struct server_local* s, struct server_player* p,
	int old_health, int new_health);
void sop_probe_powered(void);
void sop_probe_play_begin(void);
void sop_probe_server_end(struct server_local* s);
void server_local_split_offer_piston_tick(struct server_local* s);
#endif

/* Copyright (c) 2026 CaveWii_v2 contributors */
#ifndef COLUMN_OBSERVER_H
#define COLUMN_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct chunk;
struct client_rpc;
struct server_world;
struct server_chunk;
struct world;
struct displaylist;

/* Runtime-only passive seams. Each producer owns its NULL-initialized hook;
 * focused builds need no observer implementation. Arm before starting threads,
 * disarm only after joining them. No hook changes an engine-owned byte. */
extern void (*clin_column_observe_rpc)(const struct client_rpc*);
extern void (*clin_column_observe_before_rpc)(const struct client_rpc*);
extern void (*clin_column_observe_publish)(const struct client_rpc*);
extern void (*clin_column_observe_slot)(int, const void*);
extern void (*server_column_observe_tick)(struct server_world*);
extern void (*server_column_observe_update)(struct server_world*);
extern void (*server_column_observe_lifetime)(struct server_world*, bool);
extern void (*server_column_observe_chunk)(struct server_world*, int32_t,
	int32_t, const struct server_chunk*, bool);
extern void (*chunk_column_observe_lifetime)(struct chunk*, bool);
extern void (*mesher_column_observe_complete)(
	const struct chunk*, uint32_t, const struct displaylist*, const bool*,
	const uint8_t*, bool);

/* prefix names census.gz, server.gz, client.gz and render.gz. A census-only
 * run opens only census.gz. Golden verification consumes the frozen stream
 * and rejects both unequal bytes and trailing/unconsumed records. */
bool column_observer_start(const char* prefix, unsigned traces, bool compare);
bool column_observer_stop(void);
bool column_observer_ok(void);
void column_observer_client(struct world* w, int event);
void column_observer_window(bool measuring);

#endif

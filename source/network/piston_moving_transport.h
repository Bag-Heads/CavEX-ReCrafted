/*
	Copyright (c) 2026 CaveWii_v2 contributors

	Compact transport and client-side ownership for Beta TileEntityPiston state.
	The server and client share one process today, but progress is still encoded as
	raw bytes so both IEEE-754 bit patterns survive without an alignment-dependent
	packed float load on Broadway.
*/

#ifndef PISTON_MOVING_TRANSPORT_H
#define PISTON_MOVING_TRANSPORT_H

#include "../block/blocks_data.h"
#include "../block/piston_moving.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 3 scalar bytes + two unaligned-safe float bit patterns = 11 bytes exactly. */
struct piston_moving_wire_state {
	uint8_t stored_id;
	uint8_t stored_meta;
	/* facing in bits 0..2; PISTON_MOVING_FLAG_* in bits 3..4. */
	uint8_t facing_flags;
	uint8_t current_progress_bits[sizeof(float)];
	uint8_t last_progress_bits[sizeof(float)];
};

/* Chunk-local x/z nibbles + y + wire state = 13 bytes per moving block. */
struct piston_moving_chunk_record {
	uint8_t xz;
	uint8_t y;
	struct piston_moving_wire_state state;
};

typedef char piston_moving_float_size_guard[(sizeof(float) == 4) ? 1 : -1];
typedef char piston_moving_wire_size_guard[
	(sizeof(struct piston_moving_wire_state) == 11) ? 1 : -1];
typedef char piston_moving_record_size_guard[
	(sizeof(struct piston_moving_chunk_record) == 13) ? 1 : -1];

bool piston_moving_wire_encode(struct piston_moving_wire_state* out,
							   const struct piston_moving_state* state);
bool piston_moving_wire_decode(const struct piston_moving_wire_state* wire,
							   struct piston_moving_state* out);
/* record encode/decode take block-coordinate chunk origins (multiples of 16),
 * unlike the snapshot/store chunk-index APIs below. */
bool piston_moving_record_encode(struct piston_moving_chunk_record* out,
								 w_coord_t chunk_origin_x,
								 w_coord_t chunk_origin_z,
								 w_coord_t x, w_coord_t y, w_coord_t z,
								 const struct piston_moving_state* state);
bool piston_moving_record_decode(const struct piston_moving_chunk_record* record,
								 w_coord_t chunk_origin_x,
								 w_coord_t chunk_origin_z,
								 w_coord_t* x, w_coord_t* y, w_coord_t* z,
								 struct piston_moving_state* state);

struct server_local;
/*
 * Reusable backing owned by one pooled chunk RPC slot. `records` is the
 * published compact payload; `orders` is server-only sorting scratch which
 * keeps TileEntity loaded-list order without a per-stream malloc/free cycle.
 * Both arrays grow together and are retained at their high-water mark.
 */
struct piston_moving_chunk_snapshot {
	struct piston_moving_chunk_record* records;
	uint64_t* orders;
	size_t count;
	size_t capacity;
};

void piston_moving_chunk_snapshot_destroy(
	struct piston_moving_chunk_snapshot* snapshot);

/* Gather one server chunk in TileEntity loaded-list insertion order. */
bool piston_moving_server_snapshot(
	struct server_local* server, w_coord_t chunk_index_x,
	w_coord_t chunk_index_z, struct piston_moving_chunk_snapshot* snapshot);
/* The same for a chunk of world `w` (the A2-T landing stream sends the
 * destination world's columns; only that world's moving entries belong to
 * them). piston_moving_server_snapshot is this with worlds[0]. */
struct server_world;
bool piston_moving_server_snapshot_w(
	struct server_local* server, struct server_world* w,
	w_coord_t chunk_index_x, w_coord_t chunk_index_z,
	struct piston_moving_chunk_snapshot* snapshot);

struct piston_moving_client_entry {
	w_coord_t x, y, z;
	struct piston_moving_state state;
};

void client_piston_moving_clear(void);
/* Advance the render/collision copy once per active 20 Hz client tick. A
 * completed entry remains owned until the server's ordinary settling block
 * overwrite removes it, preventing an orphan id-36 interval under RPC lag. */
void client_piston_moving_tick(void);
bool client_piston_moving_upsert(w_coord_t x, w_coord_t y, w_coord_t z,
								const struct piston_moving_state* state);
bool client_piston_moving_remove(w_coord_t x, w_coord_t y, w_coord_t z);
void client_piston_moving_remove_chunk(w_coord_t chunk_index_x,
								   w_coord_t chunk_index_z);
bool client_piston_moving_replace_chunk(
	w_coord_t chunk_index_x, w_coord_t chunk_index_z,
	const struct piston_moving_chunk_record* records, size_t count);
bool client_piston_moving_query(w_coord_t x, w_coord_t y, w_coord_t z,
							   struct piston_moving_state* out);
size_t client_piston_moving_count(void);

typedef void (*client_piston_moving_visit_fn)(
	const struct piston_moving_client_entry* entry, void* user);
void client_piston_moving_visit(client_piston_moving_visit_fn visit, void* user);

/* Split-dims B2: client moving-piston STORES, one per client world payload.
 * Every client_piston_moving_* call above operates on the SELECTED store;
 * the primary is static and selected by default, so code that never selects
 * another store is unchanged. Owner thread only. destroy refuses the primary
 * and the selected store. */
struct piston_moving_store;
struct piston_moving_store* client_piston_moving_store_primary(void);
struct piston_moving_store* client_piston_moving_store_create(void);
void client_piston_moving_store_destroy(struct piston_moving_store* store);
struct piston_moving_store* client_piston_moving_store_select(
	struct piston_moving_store* store);
void client_piston_moving_store_clear(struct piston_moving_store* store);
size_t client_piston_moving_store_count(const struct piston_moving_store* store);

#endif

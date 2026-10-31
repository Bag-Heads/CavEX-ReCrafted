/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef COMPLEX_BLOCK_ARCHIVE_H
#define COMPLEX_BLOCK_ARCHIVE_H

#include <m-lib/m-string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../cNBT/nbt.h"
#include "../cglm/cglm.h"

#include "../item/inventory.h"
#include "../item/item_types.h"
#include "../world.h"
#include "server_local.h"

// File-scope forward decl (interface split, build_architecture step 7 slice
// 4): without this, the first use of `struct complex_block_pos` below --
// inside a parameter list, with no prior file-scope declaration -- creates a
// tag SCOPED ONLY TO THAT PROTOTYPE (C99 6.2.1p7), not visible elsewhere in
// the TU. Any includer that later brings in the complete struct from
// server_local_state.h then has two incompatible same-named tags in play and
// gets "conflicting types" at the definition/call site. Declaring it here, at
// file scope, makes every prototype below (and every includer) share the one
// tag that server_local_state.h's definition completes.
struct complex_block_pos;

// M5: the flat chest/sign tables are per-DIMENSION (chests.dat/signs.dat for
// the overworld -- unchanged, save-compatible -- and chests_dim-1.dat/
// signs_dim-1.dat for the nether). Callers pass the dimension of the WORLD
// whose table is being read/written (s->world.dimension), never the player's.
/* A missing primary+backup is a valid empty legacy archive. A structurally
 * invalid known-version primary may use a validated _old. A newer-version
 * primary fails closed and both files are preserved for the newer binary. */
bool chest_archive_read(struct complex_block_pos* pos, struct item_data* items, string_t path,
						enum world_dim dim);
bool chest_archive_write(struct complex_block_pos* pos, struct item_data* items, string_t path,
						 enum world_dim dim);
bool sign_archive_read(struct complex_block_pos* pos, char* texts, string_t path,
					   enum world_dim dim);
bool sign_archive_write(struct complex_block_pos* pos, char* texts, string_t path,
					    enum world_dim dim);

/* A1b (split-dimensions D9/D10): world-selected legacy flat archives. `st` is
 * a keyed production view (server_world_tile_store(w)); `dim` is that world's
 * persistent archive identity.
 * - write_w writes ONLY `st`'s world rows: another world's occupied rows go to
 *   disk as empty rows (single-world output is byte-identical to the plain
 *   writer).
 * - read_w replaces the shared table from the file and stamps every restored
 *   row with `st`'s owner. The file is a full-table image, so it REFUSES
 *   (returns false, mutates nothing) while another world owns any row; a
 *   per-world merge reload belongs to A2's storage path. */
struct te_store;
bool chest_archive_write_w(const struct te_store* st, string_t path,
						   enum world_dim dim);
bool sign_archive_write_w(const struct te_store* st, string_t path,
						  enum world_dim dim);
/* A2-S: decode BOTH flat archives of `dim` under `path` into a fresh
 * tile_restore_plan (scratch, never the shared table): missing files yield an
 * empty image (STORAGE_OK), unsupported/corrupt/unpromotable inputs refuse
 * (STORAGE_CORRUPT), a scratch allocation failure refuses (STORAGE_CAPACITY).
 * Nothing is mutated; the caller preflights + commits (tile_entities_store.h)
 * or aborts. */
struct tile_restore_plan;
enum storage_rc complex_archive_prepare_w(const struct te_store* st,
										  string_t path, enum world_dim dim,
										  struct tile_restore_plan* plan);
/* The A1b wrappers, now prepare -> preflight -> commit of the ONE kind
 * (foreign rows of the other world are kept, no longer a refusal). World
 * reload uses complex_archive_prepare_w for both kinds together instead. */
bool chest_archive_read_w(const struct te_store* st, string_t path,
						  enum world_dim dim);
bool sign_archive_read_w(const struct te_store* st, string_t path,
						 enum world_dim dim);

#ifdef CAVEWII_COMPLEX_BLOCK_ARCHIVE_TEST
/* Fail the next archive write after `successful_writes` calls have passed.
 * A value of zero fails the next chest/sign write; one permits the chest and
 * fails the following sign write. */
void complex_block_archive_test_fail_write_after(unsigned successful_writes);
/* Fail this many consecutive `_new` -> primary promotion attempts. */
void complex_block_archive_test_fail_promotions(unsigned count);
/* Fail one committed-path sync after this many successful sync calls. */
void complex_block_archive_test_fail_sync_after(unsigned successful_syncs);
#endif
#endif

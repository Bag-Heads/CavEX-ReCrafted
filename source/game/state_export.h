/*
	Copyright (c) 2026 ByteBit/xtreme8000

	This file is part of CavEX / CaveWii_v2.

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

// CaveWii_v2 game-state export: the per-tick CANONICAL state contract shared by
// the external Python scorer (now) and the future live in-engine bridge (later).
// One compact JSON line describes everything a text-driven driver needs to make
// a cheap decision without reading pixels.
//
// Adapted from amahpour-CavEX source/game/state_export.{h,c}. That upstream split
// the serializer into a PURE writer + a PC-only live collector
// (state_export.h:29-33). We keep ONLY the pure half here: the engine-free
// `state_export_write`, which renders a `struct game_export_state` snapshot into a
// caller buffer with no globals, no I/O, no gstate reads -- so the scorer can link
// it and unit-test the exact byte format the bridge will later emit. The live
// collector (filling the snapshot from gstate) is deliberately NOT in this file --
// that is the bridge's job, wired into the C build separately.
//
// Schema differences from amahpour (CaveWii's structs differ -- see entity.h):
//   * orient is emitted in DEGREES, not radians (this struct stores the degree
//     values the bridge will convert; the writer never converts).
//   * flying / creative / riding are DROPPED: CaveWii's `struct
//     entity_local_player` (entity.h:93-96) has neither flight, a creative flag,
//     nor a boat-riding id -- emitting them would be a lie.
//   * `look` is richer than amahpour's `aim`: CaveWii's `camera_ray_result`
//     (camera.h:39-45) carries the hit side and a separate entity-hit channel, so
//     the contract exposes side / block_id / entity_hit / entity_id.
//   * hotbar is FLATTENED to top-level slot/item/count fields (not a nested
//     object), and in_water / oxygen / health are added.
//   * tick is u64 (amahpour used int).

#ifndef STATE_EXPORT_H
#define STATE_EXPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Side length of the square local heightmap window centred on the player. Must
// be odd so the player sits in the middle cell. 5x5 = 25 ints, plenty for a
// cheap "walk toward higher ground" decision and keeps the JSON line small.
// (amahpour state_export.h:44, STATE_EXPORT_HEIGHT_WINDOW.)
#define STATE_EXPORT_HEIGHT_WINDOW 5
#define STATE_EXPORT_HEIGHT_CELLS                                               \
	(STATE_EXPORT_HEIGHT_WINDOW * STATE_EXPORT_HEIGHT_WINDOW)

// Current schema revision. Bump whenever a field is added/removed/retyped or its
// meaning changes, so the scorer can reject a mismatched line instead of silently
// misreading it. The writer always emits this first.
#define STATE_EXPORT_SCHEMA_VERSION 2

// The aimed `look` block: a flat, engine-free copy of the engine's last raycast
// (the bridge fills it from camera_ray_result; the pure writer only reads it).
struct game_export_look {
	bool hit;			 // true when the crosshair ray hit a block
	int x, y, z;		 // aimed block coords (valid iff hit)
	int side;			 // hit face: enum side (TOP0 BOTTOM1 LEFT2 RIGHT3 FRONT4
						 // BACK5); -1 when !hit
	int block_id;		 // aimed block type id (valid iff hit; -1 when !hit)
	bool entity_hit;	 // true when the ray hit an entity (mob/item) instead of
						 // / before a block
	int entity_id;		 // hit entity id (valid iff entity_hit; -1 otherwise)
};

// A flat, engine-free snapshot of everything the exporter serializes. The bridge
// collector fills this from gstate; the PURE writer only ever reads it. (Mirrors
// amahpour's `struct game_export_state`, state_export.h:48-77, minus the dropped
// fields and plus CaveWii's look/in_water/oxygen/health.)
struct game_export_state {
	int schema_version;	 // == STATE_EXPORT_SCHEMA_VERSION; the writer emits this
						 // verbatim (the bridge should set it from the macro).

	uint64_t tick;		 // monotonic 20 Hz agent/world tick

	const char* screen;	 // current screen name, e.g. "ingame" / "inventory";
						 // the writer substitutes "unknown" when NULL.

	bool has_player;	 // false on menus / before the world loads. When false the
						 // writer omits every player-dependent field below.

	float px, py, pz;	 // player position, world coords (pos[3] = x,y,z)
	float yaw, pitch;	 // orientation in DEGREES (orient[2] = yaw,pitch)
	bool on_ground;

	int hotbar_slot;	 // selected hotbar index 0..8, or -1 if unknown
	int hotbar_item_id;	 // item id in the selected slot, or -1 if empty
	int hotbar_count;	 // stack size in the selected slot, 0 if empty

	struct game_export_look look;

	// Surface height per cell of the window, row-major, north-west origin; the
	// centre cell ([n/2][n/2]) is the player's column. Stored RELATIVE to the
	// player's surface y (the bridge subtracts; the writer emits the ints as-is).
	bool has_heightmap;	 // true when the height window was filled
	int heightmap[STATE_EXPORT_HEIGHT_CELLS];

	bool in_water;		 // player's eyes/body in a fluid cell
	int oxygen;			 // remaining air/oxygen (Beta bubble HUD units)
	int health;			 // player health (Beta half-hearts: 0..20)
	bool wet;			 // authoritative local-host Entity.isWet result
	int fire;			 // authoritative local-host fire counter
	bool rained_on;	 // authoritative rain-exposure decision at floored feet
	int height;			 // server heightmap value for the floored feet column
	bool biome_rains;
	int fx, fy, fz;	 // floored server coordinates used by the wet query
};

// Render `st` as a single-line JSON object into `buf` (NUL-terminated).
//
// Pure: reads only `st`, writes only `buf`. Returns the number of characters that
// WOULD be written (excluding the NUL), like snprintf -- so a caller can detect
// truncation (return value >= bufsz). Always NUL-terminates when bufsz > 0.
// Floating-point fields are emitted with fixed precision so the line is stable and
// compact. (amahpour state_export.h:79-87 / state_export.c:52-101.)
//
// ---------------------------------------------------------------------------
// JSON FIELD CONTRACT (what the Python scorer must parse)
// ---------------------------------------------------------------------------
// Exactly ONE line, a single flat JSON object, keys in this fixed order. Numbers
// only, no NaN/Inf. Floats: pos = 2 decimals, orient = 2 decimals. Booleans are
// bare `true`/`false`. `screen` is always a string.
//
// Always present (even on menus):
//   "schema_version": int   -- == STATE_EXPORT_SCHEMA_VERSION
//   "tick":           int   -- u64, unsigned (PRIu64)
//   "screen":         str   -- e.g. "ingame", "inventory", "unknown"
//   "player":         bool  -- has_player; gates every field below
//
// Present ONLY when "player" is true (omitted entirely otherwise):
//   "pos":         [float,float,float]  -- x, y, z  (2 dp)
//   "orient":      [float,float]        -- yaw, pitch in DEGREES (2 dp)
//   "on_ground":   bool
//   "hotbar_slot": int                  -- 0..8, or -1
//   "hotbar_item": int                  -- item id, or -1 if empty
//   "hotbar_count":int                  -- stack size, 0 if empty
//   "look": object                      -- never null; always present:
//       "hit":       bool
//       "x","y","z": int                -- aimed block coords (or -1 when !hit)
//       "side":      int                -- enum side 0..5 (or -1 when !hit)
//       "block_id":  int                -- block type (or -1 when !hit)
//       "entity_hit":bool
//       "entity_id": int                -- (or -1 when !entity_hit)
//   "heightmap":   [int x25]            -- ONLY when has_heightmap, else omitted;
//                                          row-major NW-origin, y RELATIVE to player
//   "in_water":    bool
//   "oxygen":      int
//   "health":      int
//   "wet":         bool
//   "fire":        int
//   "rained_on":   bool
//   "height":      int
//   "biome_rains": bool
//   "fx","fy","fz": int
//
// A concrete in-world line (has_player=true, looking at grass id 2 from above,
// no entity, heightmap filled):
//   {"schema_version":1,"tick":12345,"screen":"ingame","player":true,
//    "pos":[10.50,64.00,-7.25],"orient":[180.00,0.00],"on_ground":true,
//    "hotbar_slot":0,"hotbar_item":1,"hotbar_count":64,
//    "look":{"hit":true,"x":10,"y":63,"z":-7,"side":0,"block_id":2,
//            "entity_hit":false,"entity_id":-1},
//    "heightmap":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
//    "in_water":false,"oxygen":300,"health":20,"wet":false,"fire":0,
//    "rained_on":false,"height":64,"biome_rains":true,"fx":10,"fy":64,"fz":-8}
//   (emitted as ONE line with no spaces/newlines; wrapped here for readability).
//
// A menu line (has_player=false): every player field is absent:
//   {"schema_version":1,"tick":42,"screen":"inventory","player":false}
int state_export_write(const struct game_export_state* st, char* buf, size_t n);

#endif

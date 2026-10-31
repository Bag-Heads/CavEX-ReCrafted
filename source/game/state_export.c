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

// PURE state-export serializer. Engine-free: this file reads only the snapshot
// struct and writes only the caller's buffer -- no globals, no gstate, no I/O.
// The live collector that fills the snapshot from the running game is the future
// bridge's job and lives elsewhere. Ported from amahpour-CavEX
// source/game/state_export.c (the put() cursor helper :31-50 + the pure writer
// :52-101), adapted to CaveWii's schema (see state_export.h).

#include <inttypes.h> // PRIu64 -- emit the u64 tick portably
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "state_export.h"

// Hand-rolled JSON so this stays free of any heap/library dependency and can be
// linked into the pure unit-test / scorer harness. We accumulate with vsnprintf
// into a moving cursor and track the would-be length so truncation is detectable
// exactly like snprintf (return >= bufsz). `off` is the running would-be length;
// we only ever write while off < bufsz.
// (amahpour state_export.c:31-50, verbatim apart from naming.)
static void put(char* buf, size_t bufsz, int* off, const char* fmt, ...)
	__attribute__((format(printf, 4, 5)));

static void put(char* buf, size_t bufsz, int* off, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);

	// Single clamp: how much of the buffer is already used (never past bufsz).
	// With no real buffer we still let vsnprintf compute the would-be length,
	// but must pass (NULL, 0) -- the only NULL form vsnprintf accepts -- so a
	// NULL/oversized-size combination can never deref through NULL.
	size_t used = (*off >= 0 && (size_t)*off < bufsz) ? (size_t)*off : bufsz;
	char* dst = buf ? buf + used : NULL;
	size_t cap = dst ? bufsz - used : 0;
	int n = vsnprintf(dst, cap, fmt, ap);
	va_end(ap);

	if(n > 0)
		*off += n;
}

// (amahpour state_export.c:52-101, adapted to CaveWii's schema. Signature is
// struct-first per the CaveWii contract.)
int state_export_write(const struct game_export_state* st, char* buf, size_t n) {
	if(buf && n > 0)
		buf[0] = '\0';
	if(!st)
		return 0;

	int off = 0;

	put(buf, n, &off, "{\"schema_version\":%d", st->schema_version);
	put(buf, n, &off, ",\"tick\":%" PRIu64, st->tick);

	const char* screen = st->screen ? st->screen : "unknown";
	put(buf, n, &off, ",\"screen\":\"%s\"", screen);

	put(buf, n, &off, ",\"player\":%s", st->has_player ? "true" : "false");

	if(st->has_player) {
		put(buf, n, &off, ",\"pos\":[%.2f,%.2f,%.2f]", (double)st->px,
			(double)st->py, (double)st->pz);
		put(buf, n, &off, ",\"orient\":[%.2f,%.2f]", (double)st->yaw,
			(double)st->pitch);
		put(buf, n, &off, ",\"on_ground\":%s",
			st->on_ground ? "true" : "false");

		put(buf, n, &off, ",\"hotbar_slot\":%d", st->hotbar_slot);
		put(buf, n, &off, ",\"hotbar_item\":%d", st->hotbar_item_id);
		put(buf, n, &off, ",\"hotbar_count\":%d", st->hotbar_count);

		// look is ALWAYS an object (never null): the scorer can read .hit
		// without branching on presence. Coords/side/block fall back to -1 when
		// there is no block hit so a numeric parse never fails.
		const struct game_export_look* lk = &st->look;
		put(buf, n, &off, ",\"look\":{\"hit\":%s", lk->hit ? "true" : "false");
		put(buf, n, &off, ",\"x\":%d,\"y\":%d,\"z\":%d",
			lk->hit ? lk->x : -1, lk->hit ? lk->y : -1, lk->hit ? lk->z : -1);
		put(buf, n, &off, ",\"side\":%d", lk->hit ? lk->side : -1);
		put(buf, n, &off, ",\"block_id\":%d", lk->hit ? lk->block_id : -1);
		put(buf, n, &off, ",\"entity_hit\":%s",
			lk->entity_hit ? "true" : "false");
		put(buf, n, &off, ",\"entity_id\":%d",
			lk->entity_hit ? lk->entity_id : -1);
		put(buf, n, &off, "}");

		if(st->has_heightmap) {
			put(buf, n, &off, ",\"heightmap\":[");
			for(int k = 0; k < STATE_EXPORT_HEIGHT_CELLS; k++)
				put(buf, n, &off, "%s%d", k ? "," : "", st->heightmap[k]);
			put(buf, n, &off, "]");
		}

		put(buf, n, &off, ",\"in_water\":%s",
			st->in_water ? "true" : "false");
		put(buf, n, &off, ",\"oxygen\":%d", st->oxygen);
		put(buf, n, &off, ",\"health\":%d", st->health);
		put(buf, n, &off, ",\"wet\":%s", st->wet ? "true" : "false");
		put(buf, n, &off, ",\"fire\":%d", st->fire);
		put(buf, n, &off, ",\"rained_on\":%s",
			st->rained_on ? "true" : "false");
		put(buf, n, &off, ",\"height\":%d", st->height);
		put(buf, n, &off, ",\"biome_rains\":%s",
			st->biome_rains ? "true" : "false");
		put(buf, n, &off, ",\"fx\":%d,\"fy\":%d,\"fz\":%d",
			st->fx, st->fy, st->fz);
	}

	put(buf, n, &off, "}");
	return off;
}

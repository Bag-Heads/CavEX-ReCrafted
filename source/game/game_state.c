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

#include "game_state.h"
#include "../platform/cw_canary.h"

/* S5.3 hunt PROBE (temporary, ROUND 6): leading/trailing guard canaries
 * around gstate itself -- the ONE structure every suspect stage in the
 * round-5 frame remainder touches (tick loop, render pipeline, MP dispatch).
 * Adjacency is a same-TU, declaration-order best-effort (no linker script
 * guarantee), which is why BOTH sides are guarded rather than relying on one;
 * a leading-only or trailing-only clobber still narrows which neighbour an
 * overrun spilled from. See cw_canary.h for the scan-on-clobber-only design
 * (zero I/O on the clean path). */
#ifdef PLATFORM_WII
struct cw_canary gstate_canary_lead;
#endif
struct game_state gstate;
#ifdef PLATFORM_WII
struct cw_canary gstate_canary_trail;
#endif

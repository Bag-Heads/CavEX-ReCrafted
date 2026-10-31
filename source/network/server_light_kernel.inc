/* G9 R6 rank 1: pinned descriptors, direct plane offsets. Included only by
 * server_world.c, after resolver/store definitions. General lighting.c and
 * tests/support/g9_frozen_flood.h retain independent baseline algorithms. */
_Static_assert(CHUNK_SIZE == 16 && WORLD_HEIGHT == 128
	&& SIDE_TOP == 0 && SIDE_BOTTOM == 1 && SIDE_LEFT == 2 && SIDE_RIGHT == 3
	&& SIDE_FRONT == 4 && SIDE_BACK == 5, "direct plane stride/side layout");
/* g9-trial-1009: the chunk of a cell is floor(x / 16); an arithmetic right
 * shift (GCC defines >> on negative signed values as arithmetic) gives it
 * without WCOORD_CHUNK_OFFSET's sign branch and division. Same value for
 * every int32 (test_g9_trial_kernel_equiv checks both forms). */
_Static_assert((-1 >> 1) == -1 && (-17 >> 4) == -2, "arithmetic signed >>");
struct server_light_entry { w_coord_t x, y, z; bool forced; };
/* proto-light-1008 P2 (T1 candidate): util.c's nibble_read is out of line
 * from this TU; the same expression, inlined (identical value). */
static inline uint8_t server_light_nib(const uint8_t* base, unsigned idx) {
 return (base[idx / 2] >> ((idx % 2) * 4)) & 0xF;
}
static inline int8_t server_light_max(int8_t a, int8_t b) { return a > b ? a : b; }
/* g9-trial-1009 (T1): a neighbour value v (0..15) as a one-hot bit. The OR
 * of the bits of a set of values has its highest set bit at their maximum
 * (31 - clz), so the max over six neighbours is six shifts, ORs and one
 * count-leading-zeros: no compare/select chain (Broadway has no isel). */
#define SERVER_LIGHT_BIT(v) (UINT32_C(1) << ((v) & 0xFu))
/* g9-trial-1009 (T1): nibble (b >> s) & 15 of a byte b, s in {0, 4}, as a
 * rotate-and-mask (one rlwnm on Broadway instead of shift + mask): the bits
 * the rotation brings round land at 28..31 and are masked away. */
static inline unsigned server_light_rnib(uint32_t b, unsigned s) {
 return ((b >> s) | (b << ((32u - s) & 31u))) & 0xFu;
}
/* The flood's per-id block_light entry: bits 0-3 luminance (0 for a NULL
 * blocks[] entry), bits 8-12 k = 31 - max(opacity, 1) (opacity 1 for NULL),
 * bit 15 set when the neighbours are read (NULL or can_see_through). With a
 * one-hot neighbour mask m (max value v = 31 - clz(m)): v - opacity is
 * k - clz(m). */
#define SERVER_LIGHT_READS 0x8000u
static const int8_t SERVER_LIGHT_SIDE[SIDE_MAX][3] = {
 [SIDE_TOP]={0,1,0}, [SIDE_BOTTOM]={0,-1,0}, [SIDE_LEFT]={-1,0,0},
 [SIDE_RIGHT]={1,0,0}, [SIDE_FRONT]={0,0,-1}, [SIDE_BACK]={0,0,1}
};
struct server_light_pop {
 struct server_world* world;
 struct server_chunk* center;
 struct server_chunk* horizontal[4];
 /* g9-trial-1009: `center` while it may be reused by the next pop (never in
  * census mode; NULL after every yield and failed resolve) */
 struct server_chunk* memo;
 unsigned resolved, index, lx, lz, y;
 w_coord_t cx, cz;
 bool census;
};
/* proto-light-1008 P2: a pop in the SAME chunk as the previous pop reuses
 * that pop's center pointer and its already-resolved horizontal seams. The
 * dictionary does not change inside a flood slice (the server thread owns
 * it; the memo is dropped at every 256-pop yield and per flood), so the
 * pointers are exactly what a fresh resolve would return. Census mode keeps
 * the original per-pop resolve so its hit/miss counts are unchanged.
 * g9-trial-1009: server_light_flood performs this decode inline (same
 * order: y range, then the centre resolve, then the local coordinates). */

/* The memoised resolve of horizontal seam side `side` (LEFT..BACK): the one
 * resolve call server_light_pop_neighbour always made for a seam side, with
 * the same arguments, census flag and memo bit. */
static inline struct server_chunk* server_light_pop_seam(
 struct server_light_pop* p, enum side side) {
 const unsigned k = side - SIDE_LEFT;
 if(!(p->resolved & (1u << k))) {
  p->horizontal[k] = server_world_resolve_chunk_counted(p->world,
	  p->cx + SERVER_LIGHT_SIDE[side][0],
	  p->cz + SERVER_LIGHT_SIDE[side][2], p->census);
  p->resolved |= 1u << k;
 }
 return p->horizontal[k];
}
static inline bool server_light_pop_neighbour(struct server_light_pop* p,
 enum side side, uint8_t* sky, uint8_t* torch) {
 struct server_chunk* sc = p->center;
 unsigned idx = p->index;
 bool seam = false;
 switch(side) {
 case SIDE_TOP: if(p->y == WORLD_HEIGHT-1) return false; idx++; break;
 case SIDE_BOTTOM: if(!p->y) return false; idx--; break;
 case SIDE_LEFT: seam = !p->lx; idx = seam ? idx + 15*2048 : idx - 2048; break;
 case SIDE_RIGHT: seam = p->lx == 15; idx = seam ? idx - 15*2048 : idx + 2048; break;
 case SIDE_FRONT: seam = !p->lz; idx = seam ? idx + 15*128 : idx - 128; break;
 case SIDE_BACK: seam = p->lz == 15; idx = seam ? idx - 15*128 : idx + 128; break;
 default: return false;
 }
 if(seam) {
  sc = server_light_pop_seam(p, side);
  if(!sc) return false;
 }
 if(sky) *sky = server_light_nib(sc->lighting_sky, idx);
 if(torch) *torch = server_light_nib(sc->lighting_torch, idx);
 return true;
}
/* g9-trial-1009 (T1): the pop already read both of the cell's nibbles
 * (old_light) and nothing wrote the cell since, so the two equality tests
 * server_chunk_store_light would re-read are passed in (same values). */
static inline void server_light_pop_set(struct server_chunk* c, unsigned index,
 w_coord_t x, w_coord_t y, w_coord_t z, uint8_t light, uint8_t old_light,
 struct thread_mutex** held) {
 server_chunk_store_light_known(c, index, light & 15, light >> 4,
	 ((light ^ old_light) & 0x0F) != 0, ((light ^ old_light) & 0xF0) != 0, held);
 server_world_chunk_touch(c);
#ifdef CAVEWII_SERVER_LOCAL_TEST
 if(server_world_test_light_write_hook)
  server_world_test_light_write_hook(x, y, z, light);
#else
 (void)x; (void)y; (void)z;
#endif
}
static size_t server_light_flood(struct stack* queue, bool ignore_sky_light,
 size_t max_pops, size_t ceiling, enum lighting_drain_result* status,
 struct lighting_drain_stats* st, struct server_world* w) {
	size_t pops = 0;
	const bool census = g9_probe_enabled();
	/* g9-compute-1009: configured at boot, before the server thread starts
	 * (thread_g9_lease_configure); selects the stripe-hold mode below. */
	const bool lease_on = thread_g9_lease_enabled();
	const bool capture = g9_capture_enabled(); /* configured quiescent at boot */
	if(status)
		*status = LIGHTING_DRAIN_DONE;
	/* proto-light-1008 P2: direct LIFO access to the same stack storage
	 * (same element layout as lighting_drain_seed / the frozen kernel) */
	assert(queue->element_size == sizeof(struct server_light_entry));
	struct server_light_pop pop = {.center = NULL}; /* no memo across floods */
	/* g9-trial-1009 (T1): the depth peak (compared on every pop) lives in a
	 * local (st is only this call's out-parameter; nothing reads it before
	 * the flood returns) and is stored on every exit. */
	uint32_t st_depth = st ? st->depth_peak : 0;
	/* g9-compute-1009 (T1): the stripe of the last written column, kept
	 * across writes and dropped before anything that may block, allocate or
	 * let another thread run (yield, queue growth, every exit). A main-thread
	 * stripe wait is bounded by one 256-pop slice. With the priority lease ON
	 * the writer keeps its per-write locking: the lease reasons about stripe
	 * holds (lease_lock_held), so its schedule stays exactly as reviewed. */
	struct thread_mutex* held = NULL;
	struct thread_mutex** const hold = lease_on ? NULL : &held;
#define SERVER_LIGHT_RELEASE() ((void)(held ? (thread_stripe_unlock(held), held = NULL) : NULL))
	/* g9-trial-1009 (T1): the frontier's data/index/length in locals.
	 * queue->index is stored on every change (as before), so the queue is
	 * always current for anyone who looks; the locals are re-read after
	 * every call that may change the storage (reserve, push fallback) and
	 * after the yield. */
	struct server_light_entry* qd = queue->data;
	size_t qi = queue->index, qlen = queue->length;
	const bool hooks = capture || lease_on;
	/* g9-trial-1009 (T1): the three block properties a pop reads, per id,
	 * copied from blocks[] at the start of every flood (blocks[] and the
	 * block descriptors are not written while the server floods): bits 0-3
	 * luminance (0 for a NULL entry), 8-12 31 - max(opacity, 1) (opacity 1
	 * for NULL), bit 15 "neighbours are read" (NULL or can_see_through). One
	 * table load replaces the blocks[] pointer chase; same values. */
	uint16_t block_light[256];
	for(unsigned id = 0; id < 256; id++) {
		const struct block* const b = blocks[id];
		const int opacity = b ? server_light_max((int8_t)b->opacity, 1) : 1;
		block_light[id] = (uint16_t)((b ? b->luminance : 0u)
			| (unsigned)(31 - opacity) << 8
			| (!b || b->can_see_through ? SERVER_LIGHT_READS : 0u));
	}
	while(qi != 0) {

		if(max_pops && pops >= max_pops)
			break;

		if(++pops > ceiling) {
			SERVER_LIGHT_RELEASE();
			thread_g9_lease_end();
			lighting_drain_ceiling_hit();
			if(status) { /* lossless: frontier intact, caller stops */
				*status = LIGHTING_DRAIN_CEILING;
				pops--; /* nothing was popped */
			}
			break;
		}
		if(status && !(qi + 5 <= qlen && qd)) {
			SERVER_LIGHT_RELEASE();
			const bool reserved = lighting_drain_reserve(queue, qi + 5);
			qd = queue->data;
			qlen = queue->length;
			if(!reserved) {
				thread_g9_lease_end();
				*status = LIGHTING_DRAIN_OOM; /* frontier intact, nothing dropped */
				pops--;
				break;
			}
		}

		if(st && qi > st_depth)
			st_depth = (uint32_t)qi;

		/* the popped slot is overwritten by this pop's pushes: copy it */
		const struct server_light_entry current = qd[--qi];
		queue->index = qi;
		G9B_QUEUE("pop", queue, current.x, current.y, current.z,
			current.forced, qi + 1, qi);
		if(hooks) {
			if(capture) g9_capture_pop(); /* exact last-pop time when armed */
			if(lease_on) thread_g9_lease_pops(1); /* before any dependency or expiry */
		}

		if((pops % 256) == 0) {
			SERVER_LIGHT_RELEASE();
			lighting_drain_yield();
			pop.memo = pop.center = NULL; /* the yield may run other server work */
			qd = queue->data;
			qi = queue->index;
			qlen = queue->length;
		}

		/* g9-trial-1009 (T1): the former server_light_pop_begin, inline:
		 * the y range first, then the (memoised) centre resolve, then the
		 * local coordinates; the same reads of the same cells. */
		if(current.y < 0 || current.y >= WORLD_HEIGHT) {
			G9B_QUEUE("skip-height", queue, current.x, current.y, current.z,
				current.forced, qi, qi);
			continue;
		}
		const w_coord_t cx = current.x >> 4, cz = current.z >> 4;
		struct server_chunk* c = pop.memo;
		if(!c || pop.cx != cx || pop.cz != cz) {
			pop.world = w;
			pop.cx = cx;
			pop.cz = cz;
			pop.census = census;
			pop.center = c = server_world_resolve_chunk_counted(w, cx, cz, census);
			pop.resolved = 0;
			pop.memo = census ? NULL : c;
			if(!c) {
				G9B_QUEUE("skip-absent", queue, current.x, current.y, current.z,
					current.forced, qi, qi);
				continue;
			}
		}
		const unsigned lx = W2C_COORD(current.x), lz = W2C_COORD(current.z),
					   y = (unsigned)current.y;
		const unsigned index = S_CHUNK_IDX(lx, y, lz);
		const uint8_t* const sp = c->lighting_sky;
		const uint8_t* const tp = c->lighting_torch;
		/* nibble `index` is byte index/2, shift (index%2)*4 (nibble_read) */
		const unsigned half = index / 2, sh = (index % 2) * 4;

		const uint8_t old_light = (uint8_t)((((tp[half] >> sh) & 0xF) << 4)
			| ((sp[half] >> sh) & 0xF));
		/* World.java:1300-1301 rejects Nether Sky queue entries; a Block
		 * drain preserves plain-setter Sky (Chunk.java:242-276). */
		uint8_t new_light_sky = ignore_sky_light ? old_light & 15 : 0;
		uint8_t new_light_torch = 0;

		if(!ignore_sky_light
		   && current.y >= c->heightmap[lx + lz * CHUNK_SIZE])
			new_light_sky = 0xF;
		/* Beta MetadataChunkBlock.func_4127_a: exposed sky supplies 15,
		 * and neighbour values (0..15) lose at least one to opacity. They
		 * cannot raise that source. Preserve all neighbour existence/resolves,
		 * torch reads, pushes and stores; omit only irrelevant SKY loads.
		 * Nether already ignores sky. Frozen ordered drains prove equality. */
		const bool need_sky = !ignore_sky_light && new_light_sky != 15;

		/* g9-compute-1009 (T1): one block lookup per pop (it was read up to
		 * four times; nothing in between can change it).
		 * g9-trial-1009 (T1): from the flood's block_light copy. */
		const unsigned bl = block_light[c->ids[index]];
		const unsigned luminance = bl & 0xFu;
		new_light_torch = (uint8_t)luminance;

		bool neighbour_known = false;
		uint8_t neighbour_exists = 0;

		if(bl & SERVER_LIGHT_READS) {
			neighbour_known = true;
			const int k = (int)((bl >> 8) & 0x1Fu); /* 31 - opacity */
			/* g9-trial-1009 (T1): one-hot neighbour maxima (see
			 * SERVER_LIGHT_BIT). Both masks start with value 0's bit: a
			 * value-0 term max(0 - opacity, 0) == 0 (opacity >= 1) can never
			 * raise the fold, whose floor is already 0, so a side that does
			 * not exist and the phantom 0 change nothing -- same value as
			 * folding only the existing sides (and plain luminance when none
			 * exists). max_k max(v_k - o, 0) == max(max_k v_k - o, 0), and
			 * max(max(t, 0), luminance) == max(t, luminance) (luminance >= 0).
			 * Every horizontal neighbour (+-2048 / +-128 cells, or the seam
			 * cell +-15*2048 / +-15*128 next door) keeps the nibble parity:
			 * byte half + offset/2, the same shift sh. */
			uint32_t tm = 1u, sm = 1u;
			if(lx - 1u < 14u && lz - 1u < 14u && y - 1u < 126u) {
				/* g9-compute-1009 (T1): an interior cell (not on a chunk seam,
				 * not at y 0 / 127) has all six neighbours in the centre column
				 * at fixed offsets -- exactly what server_light_pop_neighbour
				 * returns per side, without its seam switch.
				 * g9-trial-1009 (T1): index+-1 flips the parity: nibble i+1 is
				 * byte (i+1)/2 = half + (i%2), and i-1 is byte half + (i%2) - 1,
				 * both at shift 4 - sh. */
				neighbour_exists = (uint8_t)((1u << SIDE_MAX) - 1u);
				const int o = (int)(index % 2);
				const unsigned sv = 4u ^ sh;
				const uint8_t* const tq = tp + half;
				tm = SERVER_LIGHT_BIT(server_light_rnib(tq[o], sv))
					| SERVER_LIGHT_BIT(server_light_rnib(tq[o - 1], sv))
					| SERVER_LIGHT_BIT(server_light_rnib(tq[-1024], sh))
					| SERVER_LIGHT_BIT(server_light_rnib(tq[1024], sh))
					| SERVER_LIGHT_BIT(server_light_rnib(tq[-64], sh))
					| SERVER_LIGHT_BIT(server_light_rnib(tq[64], sh));
				if(need_sky) {
					const uint8_t* const sq = sp + half;
					sm = SERVER_LIGHT_BIT(server_light_rnib(sq[o], sv))
						| SERVER_LIGHT_BIT(server_light_rnib(sq[o - 1], sv))
						| SERVER_LIGHT_BIT(server_light_rnib(sq[-1024], sh))
						| SERVER_LIGHT_BIT(server_light_rnib(sq[1024], sh))
						| SERVER_LIGHT_BIT(server_light_rnib(sq[-64], sh))
						| SERVER_LIGHT_BIT(server_light_rnib(sq[64], sh));
				}
			} else {
				/* g9-trial-1009 (T1): a seam / y-edge cell. Each side's
				 * existence and cell are exactly server_light_pop_neighbour's,
				 * with the seam resolves made in the same side order
				 * (LEFT/RIGHT before FRONT/BACK, through the same memo). */
				if(y != WORLD_HEIGHT - 1) {
					neighbour_exists |= 1u << SIDE_TOP;
					tm |= SERVER_LIGHT_BIT(server_light_nib(tp, index + 1));
					if(need_sky) sm |= SERVER_LIGHT_BIT(server_light_nib(sp, index + 1));
				}
				if(y) {
					neighbour_exists |= 1u << SIDE_BOTTOM;
					tm |= SERVER_LIGHT_BIT(server_light_nib(tp, index - 1));
					if(need_sky) sm |= SERVER_LIGHT_BIT(server_light_nib(sp, index - 1));
				}
#define SERVER_LIGHT_GATHER_H(side, inside, off, seam_off) do { \
	const struct server_chunk* const n_ = (inside) ? c : server_light_pop_seam(&pop, side); \
	if(n_) { \
		const unsigned b_ = (inside) ? half + (off) : half + (seam_off); \
		neighbour_exists |= (uint8_t)(1u << (side)); \
		tm |= SERVER_LIGHT_BIT(server_light_rnib(n_->lighting_torch[b_], sh)); \
		if(need_sky) sm |= SERVER_LIGHT_BIT(server_light_rnib(n_->lighting_sky[b_], sh)); \
	} \
} while(0)
				SERVER_LIGHT_GATHER_H(SIDE_LEFT, lx != 0, -1024, 15 * 1024);
				SERVER_LIGHT_GATHER_H(SIDE_RIGHT, lx != 15, 1024, -15 * 1024);
				SERVER_LIGHT_GATHER_H(SIDE_FRONT, lz != 0, -64, 15 * 64);
				SERVER_LIGHT_GATHER_H(SIDE_BACK, lz != 15, 64, -15 * 64);
#undef SERVER_LIGHT_GATHER_H
			}
			if(need_sky) {
				const int sky = k - __builtin_clz(sm);
				new_light_sky = (uint8_t)(sky > 0 ? sky : 0);
			}
			const int torch = k - __builtin_clz(tm);
			new_light_torch = (uint8_t)(torch > (int)luminance ? torch : (int)luminance);
		}

		uint8_t new_light = (new_light_torch << 4) | new_light_sky;
		if(old_light == new_light && !current.forced)
			G9B_QUEUE("noop", queue, current.x, current.y, current.z,
				false, qi, qi);

		if(old_light != new_light || current.forced) {
			server_light_pop_set(c, index, current.x, current.y, current.z,
				new_light, old_light, hold);
			if(st)
				st->writes++;

			if(neighbour_exists == (1u << SIDE_MAX) - 1u
			   && qi + SIDE_MAX <= qlen && qd) {
				/* g9-compute-1009 (T1): all six neighbours exist and the
				 * reserve holds them: the same six entries, same side order. */
				struct server_light_entry* q = qd + qi;
				for(unsigned k = 0; k < SIDE_MAX; k++) {
					G9B_QUEUE("push", queue, current.x + SERVER_LIGHT_SIDE[k][0],
						current.y + SERVER_LIGHT_SIDE[k][1],
						current.z + SERVER_LIGHT_SIDE[k][2], false, qi + k, qi + k + 1);
					q[k] = (struct server_light_entry) {
						.x = current.x + SERVER_LIGHT_SIDE[k][0],
						.y = current.y + SERVER_LIGHT_SIDE[k][1],
						.z = current.z + SERVER_LIGHT_SIDE[k][2],
						.forced = false};
				}
				qi += SIDE_MAX;
				queue->index = qi;
				if(st)
					st->pushes += SIDE_MAX;
			} else {
				/* the cell as server_light_pop_neighbour reads it */
				pop.lx = lx; pop.lz = lz; pop.y = y; pop.index = index;
				for(enum side s = 0; s < SIDE_MAX; s++) {
					const int dx = SERVER_LIGHT_SIDE[s][0], dy = SERVER_LIGHT_SIDE[s][1],
							  dz = SERVER_LIGHT_SIDE[s][2];

					const bool exists = neighbour_known ?
						(neighbour_exists & (1u << s)) != 0 :
						server_light_pop_neighbour(&pop, s, NULL, NULL);
					if(exists) {
						G9B_QUEUE("push", queue, current.x + dx, current.y + dy,
							current.z + dz, false, qi, qi + 1);
						if(st)
							st->pushes++;
						const struct server_light_entry next = {
							.x = current.x + dx,
							.y = current.y + dy,
							.z = current.z + dz,
							.forced = false,
						};
						if(qi < qlen && qd) {
							qd[qi++] = next;
							queue->index = qi;
						} else { /* only without a reserve (status NULL) */
							SERVER_LIGHT_RELEASE(); /* may allocate */
							stack_push(queue, (void*)&next);
							qd = queue->data;
							qi = queue->index;
							qlen = queue->length;
						}
					}
				}
			}
		}
	}
	SERVER_LIGHT_RELEASE();
#undef SERVER_LIGHT_RELEASE
	if(st)
		st->depth_peak = st_depth;
	return pops;
}
static enum lighting_drain_result server_light_kernel(struct server_world* w,
 bool nether, size_t ceiling, size_t* pops, struct lighting_drain_stats* st) {
 enum lighting_drain_result r = LIGHTING_DRAIN_DONE;
 *st = (struct lighting_drain_stats){0};
 *pops = server_light_flood(&w->light_frontier, nether, 0, ceiling, &r, st, w);
 return r;
}

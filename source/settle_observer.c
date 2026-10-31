/* Copyright (c) 2026 CaveWii_v2 contributors
 * Passive, bounded storage. No engine-owned field is written here. All shared
 * diagnostic state is protected by one short mutex; never emit while held.
 */
#include "settle_observer.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "chunk.h"
#include "platform/thread.h"
#include "platform/mem2.h"
#include "tps_observer.h"
#include "world.h"

#define JOBS 8u
#define RPCS 32u
#define COLS 32u
#define PROBES 16u

/* Chunk sidecars retain identity, not event timestamps/frame numbers. */
struct settle_identity {
	uint32_t job_id;
	int32_t cx, cz;
	uint32_t dirty_generation;
	uint16_t world_epoch;
	uint8_t cy, stage;
};
static struct settle_identity save_key(struct settle_event e) {
	return (struct settle_identity){e.job_id,e.cx,e.cz,e.dirty_generation,e.world_epoch,e.cy,e.stage};
}
static struct settle_event load_key(struct settle_identity k) {
	return (struct settle_event){.job_id=k.job_id,.cx=k.cx,.cz=k.cz,
		.dirty_generation=k.dirty_generation,.world_epoch=k.world_epoch,.cy=k.cy,.stage=k.stage};
}
struct settle_chunk {
	const struct chunk* ptr;
	struct settle_identity key;
	uint64_t first_dirty;
	uint32_t dirty_count, visited;
	struct settle_identity draw_key;
	uint64_t draw_submit;
	bool awaiting_draw, retired, submitted;
	unsigned gate_stage;
};
struct settle_job {
	const void* token;
	struct settle_event key;
	uint64_t dirty, gather, submitted, worker, complete, consume, sleep, alloc;
	uint64_t sleep_begin, alloc_begin;
	bool dequeued, completed;
};
struct settle_column {
	bool used;
	struct settle_event key;
	uint64_t ready, slot, send, enqueue, dequeue, install;
};
struct settle_rpc {
	const void* token;
	struct settle_column col;
};
struct settle_worst {
	struct settle_event key;
	struct settle_phases phases;
	uint64_t sleep, alloc;
};
struct settle_state {
	bool active, gameplay, episode, have_center, finished;


	struct settle_event ring[SETTLE_RING_CAP];
	struct settle_event raw_worst[SETTLE_PULL_CAP];
	uint32_t raw_count;
	struct settle_chunk chunks[SETTLE_CHUNK_CAP];
	struct settle_job jobs[JOBS];
	struct settle_column columns[COLS];
	struct settle_rpc rpcs[RPCS];
	struct settle_worst worst[2];
	struct settle_counts counts;
	uint64_t count[2][2][SET_STAGE_COUNT], duration[2][2][SET_STAGE_COUNT];
	uint64_t light[2][5], light_begin, frame_us, published_us;
	uint64_t write, read, stripe_begin, skin_begin, episode_started;
	uint32_t episode_pending;
	uint32_t frame, next_id, episode_id, sequence;
	uint16_t server_epoch, client_epoch;
	int32_t cx, cz;
	int radius;
	struct settle_event blocker;
	struct settle_job* worker_job; /* only accessed under mutex */
#ifdef PLATFORM_PC
	pthread_t worker_thread;
#else
	lwp_t worker_thread;
#endif
	bool have_worker;
	uint64_t episode_read, worst_read, worst_write, worst_elapsed;
	uint64_t closed_begin, closed_end;
	struct settle_event closed_key;
	uint32_t closed_id, closed_count;
	uint8_t hops, path_stage[8];
	uint32_t path_id[8], path_us[8];
	bool path_valid;
} ;
static struct settle_state* state;
#define obs (*state)
static struct thread_mutex mutex;
static bool active;

static uint64_t (*test_clock)(void);

static uint64_t stamp(void) {
	obs.counts.clock_reads++;
	return test_clock ? test_clock() : tps_wall_now_ns() / 1000;
}
static void lost(void) {
	obs.counts.lost++;
}
static uint32_t identity(void) {
	if(++obs.next_id == 0)
		lost();
	return obs.next_id;
}
static void push(struct settle_event e, unsigned stage, uint64_t now) {
	e.us = now;
	e.frame = obs.frame;
	e.stage = (e.stage & 192u) | (uint8_t)stage;
	if(obs.write - obs.read == SETTLE_RING_CAP) {
		obs.counts.overflow++;
		obs.read++;
	}
	obs.ring[obs.write++ % SETTLE_RING_CAP] = e;
}
static unsigned mode(const struct settle_event* k) {
	return k->stage >> 7;
}
static unsigned band(const struct settle_event* k) {
	return (k->stage >> 6) & 1;
}
static void count(const struct settle_event* k, unsigned st, uint64_t us) {
	obs.count[mode(k)][band(k)][st]++;
	obs.duration[mode(k)][band(k)][st] += us;
}
static struct settle_event key(int32_t x, int32_t z, unsigned far,
							   bool server) {
	return (struct settle_event) {
		.cx = x,
		.cz = z,
		.cy = 255,
		.world_epoch = server ? obs.server_epoch : obs.client_epoch,
		.stage = (far ? 64 : 0) | (obs.gameplay ? 128 : 0)};
}
static struct settle_chunk* chunk(const struct chunk* c, bool create) {
	unsigned hash = (unsigned)(((uintptr_t)c >> 5) * 2654435761u);
	struct settle_chunk* empty = NULL;
	for(unsigned i = 0; i < SETTLE_CHUNK_PROBES; i++) {
		struct settle_chunk* s = &obs.chunks[(hash + i) % SETTLE_CHUNK_CAP];
		obs.counts.probes++;
		if(s->ptr == c)
			return s;
		if(!s->ptr && !empty)
			empty = s;
		if(!s->ptr && !s->key.job_id)
			break;
	}
	if(!create)
		return NULL;
	if(!empty) {
		lost();
		return NULL;
	}
	memset(empty, 0, sizeof(*empty));
	empty->ptr = c;
	empty->visited = UINT32_MAX;
	empty->gate_stage = SET_STAGE_COUNT;
	empty->key = save_key(key(c->x / 16, c->z / 16, c->lod != 0, false));
	empty->key.cy = (uint8_t)(c->y / 16);
	empty->key.dirty_generation = c->dirty_generation;
	empty->key.job_id = identity();
	return empty;
}
static struct settle_job* job(const void* token, bool create) {
	struct settle_job* empty = NULL;
	for(unsigned i = 0; i < JOBS; i++) {
		if(obs.jobs[i].token == token)
			return &obs.jobs[i];
		if(!obs.jobs[i].token)
			empty = &obs.jobs[i];
	}
	if(create && empty) {
		memset(empty, 0, sizeof(*empty));
		empty->token = token;
		return empty;
	}
	lost();
	return NULL;
}
static struct settle_column* column(const struct settle_input* in) {
	unsigned hash = (uint32_t)in->cx * 73856093u ^ (uint32_t)in->cz * 19349663u;
	struct settle_column* empty = NULL;
	for(unsigned i = 0; i < PROBES; i++) {
		struct settle_column* s = &obs.columns[(hash + i) % COLS];
		if(s->used && s->key.cx == in->cx && s->key.cz == in->cz
		   && s->key.world_epoch == obs.server_epoch
		   && band(&s->key) == !!in->far_band)
			return s;
		if(!s->used)
			empty = s;
	}
	if(!empty) {
		lost();
		return NULL;
	}
	memset(empty, 0, sizeof(*empty));
	empty->used = true;
	empty->key = key(in->cx, in->cz, in->far_band, true);
	empty->key.job_id = identity();
	return empty;
}
static bool worker_self(void) {
	if(!__atomic_load_n(&obs.have_worker, __ATOMIC_ACQUIRE))
		return false;
#ifdef PLATFORM_PC
	return pthread_equal(pthread_self(), obs.worker_thread);
#else
	return LWP_GetSelf() == obs.worker_thread;
#endif
}
static void blocker(struct settle_event k, uint64_t now) {
	if(obs.episode && !band(&k) && k.world_epoch == obs.client_epoch
	   && (int64_t)k.cx - obs.cx >= -obs.radius
	   && (int64_t)k.cx - obs.cx <= obs.radius
	   && (int64_t)k.cz - obs.cz >= -obs.radius
	   && (int64_t)k.cz - obs.cz <= obs.radius) {
		obs.blocker = k;
		push(k, SET_BLOCKER, now);
	}
}

static void close_episode(uint64_t now);

/* IN-FLIGHT GATE and the RESET-PATH CONTRACT.
 *
 * WHAT THE HARDWARE SHOWED (2026-09-07, split_anchor_6m): main stopped right
 * after world_reset_spawn_apply_done. WHAT IT DID NOT SHOW: a lock cycle. The
 * observer takes NO engine lock while holding `mutex` -- the acquisition order
 * is `engine lock -> observer mutex` everywhere (the mesher worker holds a
 * column_backing mutex across SET_STRIPE_END; mem2_lock's contended arm emits
 * SET_ALLOC_END with s_mem2_mutex HELD) and never the reverse. There is no ABBA
 * cycle to name, so the round-1 "deadlock" reading was NOT proven and is not
 * claimed here.
 *
 * What the reset path DID do was the one thing a world reset must never do:
 * mem2_free_sized(143 KB) + mem2_alloc_strict(143 KB) from inside a callback,
 * while the carve is being rebuilt (world_reset_post_chunk_gx_free) -- a strict
 * MEM2 allocation contending for s_mem2_mutex against the prio-32 mesher and
 * the prio-40 telemetry sender, both BELOW main's 64, on a scheduler with no
 * priority inheritance. That is the suspect this change removes outright:
 *
 *   RETIRE-IN-PLACE. A world reset never frees and never allocates. It bumps
 *   the lifecycle generation and zeroes the ring/tables under the mutex it is
 *   already holding. The armed buffer is allocated ONCE (first arm) and freed
 *   ONLY on a true disarm/shutdown. See the SET_RESET branch in observe_body.
 *
 * The gate below therefore serves exactly one purpose: letting a true
 * stop() know when no caller can still be inside observe_body, so the buffer
 * can be released without a use-after-free. It is a BOUNDED wait, not a lock,
 * so main is never parked behind a lower-priority holder for an unbounded time.
 * Callers enter the gate in the SETTLE macro BEFORE re-validating the hook
 * (settle_observer.h), which is what makes the drain exact rather than
 * best-effort. If the drain still expires, the block is retired but NOT freed
 * and the counter says so; the next start() re-drains and REUSES it.
 * (settle_observer_inflight is DEFINED in platform/mem2.c beside
 * settle_observer_control -- mem2.c emits SETTLE from pools that do not link
 * this file.) */
/* Counted, not silent: a non-zero value means a callback was still inside
 * observe() when the observer was torn down and its buffer was carried over.
 *
 * SCOPE (corrected, round 3): these two describe the STOP/START paths ONLY --
 * i.e. the arm at main.c's --cw-tps-cpu setup and the shutdown stop after the
 * thread joins. The WORLD-RESET path never reaches them by construction: it
 * retires in place inside observe_body and neither drains nor frees. A record
 * emitted during normal play therefore reads 0/0 for both, and that 0/0 is
 * information, not a dead field -- it says the stall under investigation never
 * reached a teardown. The reset path has its own pair below. */
static uint32_t deferred_retirements;
static uint32_t last_drain_us;
/* RESET-PATH cost, in-process. The retire-in-place branch IS the whole reset
 * path: one memset of the armed buffer between two wall-clock reads. There is
 * no room for these on the wire (the payload is at its exact 1,023 B envelope
 * ceiling -- see the payload note), so client_interface.c reads them and writes
 * ONE deferred trace line AFTER the reset completes, never on the path. */
static uint32_t reset_retirements;
static uint32_t last_reset_us;
#ifdef CAVEWII_TESTING
/* Test-only seam, and now compiled ONLY into test objects: a shipping
 * non-const static that flips the teardown back to the pre-fix blocking lock
 * has no business existing in a release binary. See
 * settle_observer_test_blocking_stop. */
static bool blocking_stop_seam;
#endif
/* A WALL-CLOCK budget, not a sleep count: thread_msleep's granularity is the
 * host tick (~15 ms on Windows, ~1 ms on the Wii), so a fixed iteration count
 * would give two platforms two completely different bounds. */
#define SETTLE_DRAIN_BUDGET_NS UINT64_C(200000000) /* 200 ms */

static bool drain_inflight(void) {
	uint64_t begin = tps_wall_now_ns();
	uint64_t deadline = begin + SETTLE_DRAIN_BUDGET_NS;
	for(;;) {
		uint64_t now = tps_wall_now_ns();
		if(__atomic_load_n(&settle_observer_inflight, __ATOMIC_ACQUIRE) == 0) {
			last_drain_us = (uint32_t)((now - begin) / 1000);
			return true;
		}
		if(now >= deadline) {
			last_drain_us = (uint32_t)((now - begin) / 1000);
			return false;
		}
		/* usleep-based (thread_msleep), NOT LWP_YieldThread: a yield only
		 * reaches the caller's own priority band, and the thread that must be
		 * let run -- the prio-32 mesher worker -- sits below it. */
		thread_msleep(1);
	}
}

static void observe_body(const struct settle_input* in) {
	thread_mutex_lock(&mutex);
	if(!state || in->observer_generation
			   != (__atomic_load_n(&settle_observer_control, __ATOMIC_ACQUIRE)
				   & ~1u)
	   || obs.finished) {
		thread_mutex_unlock(&mutex);
		return;
	}
	obs.counts.calls++;
	unsigned st = in->stage;
	/* Dirty callbacks can occur inside a flood. Their timestamps are the
	 * preceding frame boundary, deliberately no per-voxel clock reads. */
	uint64_t now
		= (st <= SET_DIRTY_EDIT || st == SET_VISIT) ? obs.frame_us : stamp();
	struct settle_event k = key(in->cx, in->cz, in->far_band, false);
	if(st == SET_RESET) {
		if(!in->value) {
			/* RETIRE-IN-PLACE (see the IN-FLIGHT GATE note). The world reset
			 * used to unlock, tear the observer down and re-arm it -- a 143 KB
			 * MEM2 free plus a 143 KB strict MEM2 allocation from inside a
			 * callback, on the exact frame the carve is being rebuilt. It now
			 * recycles the buffer it is already holding: no free, no alloc, no
			 * drain, no second lock acquisition, and no window in which the
			 * observer reads inactive. */
			uint32_t sequence = obs.sequence, next_id = obs.next_id;
			uint32_t episode_id = obs.episode_id;
			uint16_t server_epoch = obs.server_epoch;
			uint16_t client_epoch = obs.client_epoch + 1;
			/* A reset boundary discards old-world history. Never certify
			 * an interrupted queue/job as reconciled in the new capture. */
			uint64_t lost_before = obs.counts.lost;
			if(!client_epoch || obs.counts.rpc_outstanding
			   || obs.counts.outstanding || obs.counts.result_outstanding)
				lost_before++;
			/* Same two atomics stop()+start() published, in the same order, so
			 * a caller that loaded a hook before this reset still carries a
			 * stale generation and is dropped: +2 advances the lifecycle
			 * generation, &~1 discards a publish request for a capture that no
			 * longer exists. Done under the mutex, so no callback can be
			 * mid-decision. */
			__atomic_fetch_add(&settle_observer_control, 2u, __ATOMIC_RELEASE);
			__atomic_fetch_and(&settle_observer_control, ~1u, __ATOMIC_RELEASE);
			/* The whole cost of the reset path, measured rather than asserted.
			 * tps_wall_now_ns(), NOT stamp(): stamp() charges a clock read to
			 * the capture's own counters and the memset is about to zero them.
			 * Two gettime reads bracketing one memset -- the path holds no
			 * allocation, no free, no drain and (since round 3) no SD I/O. */
			uint64_t reset_begin = tps_wall_now_ns();
			memset(state, 0, sizeof(*state));
			last_reset_us = (uint32_t)((tps_wall_now_ns() - reset_begin) / 1000);
			reset_retirements++;
			obs.sequence = sequence;
			obs.next_id = next_id;
			obs.episode_id = episode_id;
			obs.server_epoch = server_epoch;
			obs.client_epoch = client_epoch;
			obs.counts.lost = lost_before;
			obs.frame_us = stamp();
			goto done;
		}
		if(++obs.server_epoch == 0)
			lost();
		memset(obs.columns, 0, sizeof(obs.columns));
		goto done;
	}
	if(st == SET_ALLOC_BEGIN || st == SET_ALLOC_END) {
		if(worker_self() && obs.worker_job) {
			struct settle_job* j = obs.worker_job;
			if(st == SET_ALLOC_BEGIN)
				j->alloc_begin = now;
			else {
				j->alloc += now - j->alloc_begin;
				count(&j->key, st, now - j->alloc_begin);
			}
		}
		goto done;
	}
	if(st == SET_SLEEP_BEGIN || st == SET_SLEEP_END) {
		if(obs.worker_job) {
			struct settle_job* j = obs.worker_job;
			if(st == SET_SLEEP_BEGIN)
				j->sleep_begin = now;
			else {
				j->sleep += now - j->sleep_begin;
				count(&j->key, st, now - j->sleep_begin);
			}
			push(j->key, st, now);
		}
		goto done;
	}
	if(st == SET_LIGHT_BEGIN) {
		obs.light_begin = now;
		push(k, st, now);
		goto done;
	}
	if(st == SET_LIGHT_END) {
		const struct settle_light* l = in->detail;
		const uint32_t v[]
			= {l->pops, l->stale, l->sources, l->frontier, l->budget_hit};
		for(unsigned i = 0; i < 5; i++)
			obs.light[obs.gameplay][i] += v[i];
		count(&k, st, now - obs.light_begin);
		k.job_id = l->pops;
		k.cx = (int32_t)l->stale;
		k.cz = (int32_t)l->sources;
		k.dirty_generation = l->frontier;
		k.cy = (uint8_t)l->budget_hit;
		push(k, st, now);
		goto done;
	}
	if(st >= SET_READY && st <= SET_STOP_BACKOFF) {
		if(st >= SET_STOP_UNLOAD) {
			k = key(in->cx, in->cz, in->far_band, true);
			count(&k, st, 0);
			/* Aggregate stop hits; readiness/job events carry chronology. */
			goto done;
		}
		struct settle_column* col = NULL;
		struct settle_rpc* rpc = NULL;
		if(st == SET_DEQUEUE || st == SET_INSTALL_BEGIN || st == SET_INSTALL_OK
		   || st == SET_INSTALL_FAIL) {
			for(unsigned i = 0; i < RPCS; i++)
				if(obs.rpcs[i].token == in->token)
					rpc = &obs.rpcs[i];
			if(!rpc) {
				if(st != SET_INSTALL_FAIL || in->value)
					lost();
				goto done;
			}
			col = &rpc->col;
		} else {
			col = column(in);
			if(!col)
				goto done;
		}
		k = col->key;
		uint64_t elapsed = 0;
		switch(st) {
			case SET_READY: col->ready = now; break;
			case SET_SLOT_BEGIN: col->slot = now; break;
			case SET_SLOT_END: elapsed = now - col->slot; break;
			case SET_SEND_BEGIN: col->send = now; break;
			case SET_ENQUEUE:
				for(unsigned i = 0; i < RPCS; i++)
					if(!obs.rpcs[i].token) {
						rpc = &obs.rpcs[i];
						break;
					}
				if(!rpc) {
					lost();
					goto done;
				}
				col->enqueue = now;
				rpc->token = in->token;
				rpc->col = *col;
				obs.counts.enqueue++;
				obs.counts.rpc_outstanding++;
				break;
			case SET_SEND_END:
				elapsed = now - col->send;
				col->used = false;
				break;
			case SET_DEQUEUE:
				col->dequeue = now;
				elapsed = now - col->enqueue;
				obs.counts.dequeue++;
				if(obs.counts.rpc_outstanding)
					obs.counts.rpc_outstanding--;
				else
					lost();
				break;
			case SET_INSTALL_BEGIN: col->install = now; break;
			case SET_INSTALL_OK:
			case SET_INSTALL_FAIL:
				elapsed = col->install ? now - col->install : 0;
				rpc->token = NULL;
				if(st == SET_INSTALL_OK)
					blocker(k, now);
				break;
			default: break;
		}
		count(&k, st, elapsed);
		push(k, st, now);
		goto done;
	}
	if(in->chunk) {
		struct settle_chunk* s = chunk(in->chunk, st != SET_EVICT);
		if(!s)
			goto done;
		k = load_key(s->key);
		k.stage = (in->chunk->lod ? 64 : 0) | (obs.gameplay ? 128 : 0);
		if(st <= SET_DIRTY_EDIT) {
			if(st == SET_DIRTY_LOD && !(s->key.stage & 64) && in->chunk->lod)
				blocker(load_key(s->key), now);
			if(k.dirty_generation != in->chunk->dirty_generation
			   || s->submitted) {
				s->dirty_count = 0;
			}
			if(!s->dirty_count)
				s->first_dirty = now;
			s->dirty_count++;
			if(in->chunk->dirty_generation < s->key.dirty_generation)
				lost();
			if(k.dirty_generation != in->chunk->dirty_generation
			   || s->submitted) {
				k.job_id = identity();
				s->gate_stage = SET_STAGE_COUNT;
			}
			k.dirty_generation = in->chunk->dirty_generation;
			s->submitted = false;
			s->key = save_key(k);
			s->visited = UINT32_MAX;
		} else if(st == SET_VISIT) {
			s->visited = obs.frame;
			/* Visits are counted per pass; ring stores only gate transitions.
			 */
			count(&k, st, 0);
			goto done;
		} else if(st >= SET_GATE_NEIGHBOUR && st <= SET_GATE_NOTVISITED) {
			count(&k, st, 0);
			if(s->gate_stage == st)
				goto done;
			s->gate_stage = st;
			push(k, st, now);
			goto done;
		} else if(st == SET_GATHER) {
			struct settle_job* j = job(in->token, true);
			if(!j)
				goto done;
			/* Retries may reuse the engine generation, but never a job ID. */
			if(s->submitted)
				k.job_id = identity();
			s->submitted = true;
			k.dirty_generation = in->chunk->dirty_generation;
			k.us = now;
			k.frame = obs.frame;
			s->key = save_key(k);
			s->visited = UINT32_MAX;
			j->key = k;
			j->gather = now;
			j->dirty = s->first_dirty;
			s->gate_stage = SET_STAGE_COUNT;
			s->dirty_count = 0;
			s->first_dirty = 0;
		} else if(st == SET_DRAW) {
			if(!s->awaiting_draw)
				goto done;
			k = load_key(s->draw_key);
			s->awaiting_draw = false;
			obs.duration[mode(&k)][band(&k)][SET_DRAW] += now - s->draw_submit;
		} else if(st == SET_RETIRE) {
			blocker(k, now);
			s->retired = true;
		} else if(st == SET_EVICT) {
			if(!s->retired)
				blocker(k, now);
			if(s->awaiting_draw) {
				count(&(struct settle_event){.stage=s->draw_key.stage}, SET_CULLED, 0);
				push(load_key(s->draw_key), SET_CULLED, now);
			}
			s->ptr = NULL;
		}
		count(&k, st, 0);
		push(k, st, now);
		goto done;
	}
	if(st == SET_STRIPE_BEGIN || st == SET_SKIN_BEGIN) {
		if(st == SET_STRIPE_BEGIN)
			obs.stripe_begin = now;
		else
			obs.skin_begin = now;
		goto done;
	}
	if(st == SET_STRIPE_END || st == SET_SKIN_END) {
		/* Main thread has at most one gather active. */
		for(unsigned i = 0; i < JOBS; i++)
			if(obs.jobs[i].token && !obs.jobs[i].submitted) {
				count(&obs.jobs[i].key, st,
					  now
						  - (st == SET_STRIPE_END ? obs.stripe_begin :
													obs.skin_begin));
				break;
			}
		goto done;
	}
	{
		struct settle_job* j
			= in->token ? job(in->token, false) : obs.worker_job;
		if(!j) {
			lost();
			goto done;
		}
		k = j->key;
		uint64_t elapsed = 0;
		switch(st) {
			case SET_SUBMIT:
				j->submitted = now;
				elapsed = now - j->gather;
				obs.counts.submitted++;
				obs.counts.outstanding++;
				obs.counts.queue_outstanding++;
				count(&k, SET_DIRTY_WAIT, j->dirty ? now - j->dirty : 0);
				break;
			case SET_JOB_DEQUEUE:
				j->dequeued = true;
				obs.counts.worker_dequeued++;
				if(obs.counts.queue_outstanding)
					obs.counts.queue_outstanding--;
				else
					lost();
				elapsed = now - j->submitted;
				break;
			case SET_WORKER: j->worker = now; obs.worker_job = j;
#ifdef PLATFORM_PC
				if(!obs.have_worker)
					obs.worker_thread = pthread_self();
#else
				if(!obs.have_worker)
					obs.worker_thread = LWP_GetSelf();
#endif
				__atomic_store_n(&obs.have_worker, true, __ATOMIC_RELEASE);
				break;
			case SET_PHASES: {
				const struct settle_phases* p = in->detail;
				obs.duration[mode(&k)][band(&k)][SET_VL_BEGIN]
					+= (uint64_t)(p->vl * 1000.0F);
				obs.duration[mode(&k)][band(&k)][SET_EMIT_BEGIN]
					+= (uint64_t)(p->emit * 1000.0F);
				obs.duration[mode(&k)][band(&k)][SET_FIN_BEGIN]
					+= (uint64_t)(p->fin * 1000.0F);
				obs.duration[mode(&k)][band(&k)][SET_BFS_BEGIN]
					+= (uint64_t)(p->bfs * 1000.0F);
				struct settle_worst* worst = &obs.worst[mode(&k)];
				if(p->total > worst->phases.total)
					*worst = (struct settle_worst) {k, *p, j->sleep, j->alloc};
				goto done;
			}
			case SET_COMPLETE:
				j->complete = now;
				j->completed = true;
				elapsed = now - j->worker;
				obs.counts.completed++;
				obs.counts.result_outstanding++;
				if(obs.counts.outstanding)
					obs.counts.outstanding--;
				else
					lost();
				obs.worker_job = NULL;
				break;
			case SET_CONSUME:
				j->consume = now;
				elapsed = now - j->complete;
				break;
			case SET_ACCEPT:
			case SET_STALE:
			case SET_RETRY:
				elapsed = now - j->consume;
				if(st == SET_ACCEPT)
					obs.counts.accepted++;
				else if(st == SET_STALE)
					obs.counts.stale++;
				else
					obs.counts.retry++;
				if(obs.counts.result_outstanding)
					obs.counts.result_outstanding--;
				else
					lost();
				/* Main owns chunk identity, passed in detail after
				 * installation. */
				if(st == SET_ACCEPT && in->detail) {
					struct settle_chunk* s = chunk(in->detail, false);
					if(s) {
						if(s->retired) {
							count(&k, SET_EVICT, 0);
							push(k, SET_EVICT, now);
						}
						if(s->awaiting_draw) {
							count(&(struct settle_event){.stage=s->draw_key.stage}, SET_CULLED, 0);
							push(load_key(s->draw_key), SET_CULLED, now);
						}
						s->draw_key = save_key(k);
						s->draw_submit = j->submitted;
						s->awaiting_draw = !s->retired;
					}
				}
				if(st == SET_ACCEPT && in->detail) {
					const struct chunk* c = in->detail;
					struct settle_chunk* s = chunk(c, false);
					if(s && !s->retired && !c->lod)
						blocker(k, now);
				}
				j->token = NULL;
				break;
			case SET_CANCEL:
				if(!j->dequeued) {
					obs.counts.queue_cancelled++;
					if(obs.counts.queue_outstanding)
						obs.counts.queue_outstanding--;
					else
						lost();
				}
				if(j->completed) {
					obs.counts.result_cancelled++;
					if(obs.counts.result_outstanding)
						obs.counts.result_outstanding--;
					else
						lost();
				} else {
					obs.counts.cancelled++;
					if(obs.counts.outstanding)
						obs.counts.outstanding--;
					else
						lost();
				}
				j->token = NULL;
				break;
			default: break;
		}
		count(&k, st, elapsed);
		push(k, st, now);
	}
done:
	thread_mutex_unlock(&mutex);
}

/* The gate is entered by the SETTLE macro BEFORE it re-validates the hook, not
 * here: a counter incremented after the hook load would miss a caller preempted
 * between the load and the call, and the drain would be best-effort instead of
 * exact. This wrapper is therefore just the hook's identity. */
static void observe(const struct settle_input* in) {
	observe_body(in);
}

static void observe_mem2(const struct settle_input* in) {
	observe(in);
}

bool settle_observer_start(void) {
	if(settle_observer_active())
		return false;
	if(!mutex.initialized && !thread_mutex_init(&mutex))
		return false;
	/* Same reason as stop(): the buffer must not be recycled while a callback
	 * could still be writing into it. `state` is non-NULL here only when a
	 * previous stop() timed out on its drain and carried the block; this
	 * completes that retirement WITHOUT any MEM2 traffic. */
	if(!drain_inflight()) {
		/* The straggler is still inside observe_body. Do NOT allocate a second
		 * block -- the carried one stays the module's sole buffer, reachable
		 * from `state`, and the next start() retries. Counted, never silent. */
		deferred_retirements++;
		return false;
	}
	if(!state) {
		/* The ONLY MEM2 allocation in the observer's whole lifecycle: first
		 * arm. Resets recycle in place and re-arms reuse the carried block. */
		struct settle_state* allocated = mem2_alloc_strict(sizeof(*allocated));
		if(!allocated)
			return false;
		state = allocated;
	}
	thread_mutex_lock(&mutex);
	memset(state, 0, sizeof(*state));
	__atomic_store_n(&active, true, __ATOMIC_RELEASE);
	obs.server_epoch = obs.client_epoch = 1;
	obs.frame_us = stamp();
	__atomic_store_n(&chunk_settle_observe, observe, __ATOMIC_RELEASE);
	__atomic_store_n(&world_settle_observe, observe, __ATOMIC_RELEASE);
	__atomic_store_n(&mesher_settle_observe, observe, __ATOMIC_RELEASE);
	__atomic_store_n(&client_settle_observe, observe, __ATOMIC_RELEASE);
	__atomic_store_n(&server_settle_observe, observe, __ATOMIC_RELEASE);
	__atomic_store_n(&mem2_settle_observe, observe_mem2, __ATOMIC_RELEASE);
	thread_mutex_unlock(&mutex);
	return true;
}
bool settle_observer_active(void) {
	return __atomic_load_n(&active, __ATOMIC_ACQUIRE);
}
bool settle_observer_test_clock(uint64_t (*clock_us)(void)) {
	if(settle_observer_active())
		return false;
	test_clock = clock_us;
	return true;
}
size_t settle_observer_static_bytes(void) {
	/* Reports THIS build's footprint, so the test-only seam is counted only
	 * where it exists. A release build carries one byte less. */
	return sizeof(state) + sizeof(mutex) + sizeof(settle_observer_control)
		+ sizeof(active) + sizeof(test_clock) + sizeof(settle_observer_inflight)
		+ sizeof(deferred_retirements) + sizeof(last_drain_us)
		+ sizeof(reset_retirements) + sizeof(last_reset_us)
#ifdef CAVEWII_TESTING
		+ sizeof(blocking_stop_seam)
#endif
		+ 6 * sizeof(void*);
}
static void snapshot_counts(struct settle_counts* out) {
	*out = obs.counts;
	out->finished = obs.finished;
	out->ring_used = (uint32_t)(obs.write - obs.read);
	out->valid = !out->lost
		&& out->enqueue == out->dequeue + out->rpc_outstanding
		&& out->submitted == out->completed + out->cancelled + out->outstanding
		&& out->submitted
			== out->worker_dequeued + out->queue_cancelled
				+ out->queue_outstanding
		&& out->completed
			== out->accepted + out->stale + out->retry + out->result_outstanding
				+ out->result_cancelled;
}
void settle_observer_counts(struct settle_counts* out) {
	if(!settle_observer_active()) {
		memset(out, 0, sizeof(*out));
		return;
	}
	thread_mutex_lock(&mutex);
	if(state) snapshot_counts(out);
	else memset(out, 0, sizeof(*out));
	thread_mutex_unlock(&mutex);
}

void settle_observer_frame(struct world* w, bool gameplay, int32_t cx,
						   int32_t cz, int radius, bool allow_far,
						   size_t tokens) {
	if(!settle_observer_active())
		return;
	thread_mutex_lock(&mutex);
	if(!state || obs.finished) {
		thread_mutex_unlock(&mutex);
		return;
	}
	uint64_t now = stamp();
	obs.frame_us = now;
	bool moved = !obs.have_center || obs.cx != cx || obs.cz != cz
		|| obs.gameplay != gameplay;
	if(moved && obs.episode)
		push(obs.blocker, SET_EP_CANCEL, now);
	obs.gameplay = gameplay;
	obs.cx = cx;
	obs.cz = cz;
	obs.radius = radius;
	if(moved) {
		obs.episode = true;
		obs.have_center = true;
		obs.episode_id++;
		obs.episode_started = now;
		obs.episode_read = obs.write;
		obs.blocker = key(cx, cz, 0, false);
		obs.blocker.job_id = obs.episode_id;
		push(obs.blocker, SET_EP_BEGIN, now);
	}
	unsigned pending = 0;
	struct settle_event unresolved = {0};
	/* Same bounded square and lod==0 predicate as world_near_ring_status. */
	for(int dz = -radius; dz <= radius; dz++)
		for(int dx = -radius; dx <= radius; dx++) {
			struct world_section* sec = dict_wsection_get(
				w->sections, SECTION_TO_ID(cx + dx, cz + dz));
			if(!sec) {
				pending++;
				unresolved = key(cx + dx, cz + dz, 0, false);
				continue;
			}
			for(unsigned cy = 0; cy < COLUMN_HEIGHT; cy++) {
				struct chunk* c = sec->column[cy];
				if(!c || (!c->rebuild_displist && !c->mesh_in_flight))
					continue;
				struct settle_chunk* s = chunk(c, true);
				if(c->lod == 0) {
					pending++;
					if(s)
						unresolved = load_key(s->key);
				}
				if(s
				   && (s->visited != obs.frame)) {
					unsigned gate = c->lod && !allow_far ? SET_GATE_FAR :
						!tokens							 ? SET_GATE_TOKEN :
														   SET_GATE_NOTVISITED;
					count(&(struct settle_event){.stage=s->key.stage}, gate, 0);
					if(s->gate_stage != gate) {
						push(load_key(s->key), gate, now);
						s->gate_stage = gate;
					}
				}
			}
		}
	if(!obs.episode && pending) {
		obs.episode = true;
		obs.episode_id++;
		obs.episode_started = now;
		obs.episode_read = obs.write;
		struct settle_event begin = key(cx, cz, 0, false);
		begin.job_id = obs.episode_id;
		push(begin, SET_EP_BEGIN, now);
	}
	obs.episode_pending = pending;
	if(pending && (pending == 1 || !obs.blocker.job_id))
		obs.blocker = unresolved;
	if(obs.episode && !pending) {
		push(obs.blocker, SET_EP_END, now);
		close_episode(now);
		obs.episode = false;
	}
	if(++obs.frame == 0)
		lost();
	thread_mutex_unlock(&mutex);
}


/* Only closed episodes enter the wire. Eight chronological milestone hops;
 * overflow/history gaps suppress the path instead of inventing attribution. */
static void close_episode(uint64_t now) {
	uint64_t elapsed = now - obs.episode_started;
	obs.closed_count++;
	if(obs.closed_count > 1 && elapsed <= obs.closed_end - obs.closed_begin)
		return;
	obs.closed_begin = obs.episode_started;
	obs.closed_end = now;
	obs.closed_key = obs.blocker;
	obs.closed_id = obs.episode_id;
	obs.hops = 0;
	obs.path_valid = obs.episode_read >= obs.read && elapsed <= UINT32_MAX;
	uint64_t cursor = obs.episode_started;
	unsigned stage = 63; /* explicit unattributed interval */
	uint32_t id = 0;
	for(uint64_t i = obs.episode_read; obs.path_valid && i < obs.write; i++) {
		const struct settle_event* e = &obs.ring[i % SETTLE_RING_CAP];
		unsigned st = e->stage & 63;
		if(e->world_epoch != obs.blocker.world_epoch || e->cx != obs.blocker.cx
		   || e->cz != obs.blocker.cz || e->cy != obs.blocker.cy
		   || e->job_id != obs.blocker.job_id)
			continue;
		/* Boundaries of exclusive, inclusive service intervals. Detailed worker
		 * phases and older generations remain available in the raw pull. */
		if(st != SET_GATHER && st != SET_SUBMIT && st != SET_WORKER
		   && st != SET_COMPLETE && st != SET_CONSUME && st != SET_ACCEPT
		   && st != SET_READY && st != SET_ENQUEUE && st != SET_DEQUEUE
		   && st != SET_INSTALL_BEGIN && st != SET_INSTALL_OK && st != SET_EP_END)
			continue;
		uint64_t t = e->us < cursor ? cursor : e->us > now ? now : e->us;
		if(t > cursor) {
			if(obs.hops == 8) { obs.path_valid = false; break; }
			unsigned h = obs.hops++;
			obs.path_stage[h] = (uint8_t)stage;
			obs.path_id[h] = id;
			obs.path_us[h] = (uint32_t)(t - cursor);
		}
		cursor = t;
		stage = st == SET_ACCEPT || st == SET_INSTALL_OK ? 63 : st;
		id = e->job_id;
	}
	if(!obs.path_valid) obs.hops = 0;
	if(elapsed >= obs.worst_elapsed) {
		obs.worst_elapsed = elapsed;
		obs.worst_read = obs.episode_read;
		obs.worst_write = obs.write;
		obs.raw_count = 0;
		if(obs.episode_read >= obs.read && obs.write - obs.episode_read <= SETTLE_PULL_CAP)
			for(uint64_t i = obs.episode_read; i < obs.write; i++)
				obs.raw_worst[obs.raw_count++] = obs.ring[i % SETTLE_RING_CAP];
	}
}

size_t settle_observer_armed_bytes(void) { return sizeof(struct settle_state); }
typedef char settle_armed_limit[sizeof(struct settle_state) <= 160 * 1024 ? 1 : -1];

/* Fixed v2 payload, explicitly little endian: 63 u32 hit deltas, 19 u32
 * reconciliation counters, closed episode id/duration/key, close count,
 * hop count/status, 8 (stage:u8,id:u32,us:u32) hops, close time:u64 = 436 B,
 * plus the two teardown diagnostics = 438 B.
 *
 * WHY EXACTLY TWO BYTES, AND WHY MILLISECONDS. base64 rounds to 3-byte groups,
 * so 436..438 all encode to 584 chars: the record stays 632 B of body and
 * 1,023 B on the wire (worst-case seq + the 63 B run ID, fully escaped) --
 * unchanged, and inside the 1,024 B budget the envelope test pins. A 439th
 * byte would cost 4 base64 chars and push the record to 1,027 B, over it. So
 * the diagnostics are a saturating u8 retirement count and a saturating u8
 * drain time in MILLISECONDS, not microseconds: the drain polls with
 * thread_msleep(1), so a millisecond IS its resolution and a microsecond field
 * would be false precision. 0..255 ms also covers the whole 200 ms budget, so
 * "instant" and "timed out" are never confused. Full microsecond precision
 * stays available in-process through settle_observer_last_drain_us().
 * Counter narrowing is checked; saturation INVALIDATES, never wraps. */
static void put(uint8_t** out, uint64_t value, unsigned bytes) {
	for(unsigned i = 0; i < bytes; i++) *(*out)++ = (uint8_t)(value >> (i * 8));
}
static uint32_t narrow(uint64_t value, bool* valid) {
	if(value > UINT32_MAX) { *valid = false; return UINT32_MAX; }
	return (uint32_t)value;
}
static void drain_now(bool (*emit)(const char*)) {
	uint8_t payload[438], *p = payload;
	char line[SETTLE_RECORD_BODY_MAX + 1];
	if(!settle_observer_active()) return;
	thread_mutex_lock(&mutex);
	if(!state) { thread_mutex_unlock(&mutex); return; }
	struct settle_counts c;
	snapshot_counts(&c);
	bool valid = c.valid;
	for(unsigned st = 0; st < SET_STAGE_COUNT; st++) {
		uint64_t hits = 0;
		for(unsigned m = 0; m < 2; m++)
			for(unsigned b = 0; b < 2; b++) {
				hits += obs.count[m][b][st];
				obs.count[m][b][st] = 0;
			}
		put(&p, narrow(hits, &valid), 4);
	}
	const uint64_t counters[] = {c.enqueue,c.dequeue,c.rpc_outstanding,
		c.submitted,c.completed,c.cancelled,c.outstanding,c.worker_dequeued,
		c.queue_cancelled,c.queue_outstanding,c.accepted,c.stale,c.retry,
		c.result_outstanding,c.result_cancelled,c.calls,c.clock_reads,c.overflow,c.lost};
	for(unsigned i = 0; i < 19; i++) put(&p, narrow(counters[i], &valid), 4);
	put(&p, obs.closed_count ? obs.closed_id : 0, 4);
	put(&p, obs.closed_count ? narrow(obs.closed_end - obs.closed_begin, &valid) : 0, 4);
	struct settle_event k = obs.closed_count ? obs.closed_key : (struct settle_event){0};
	put(&p, (uint32_t)k.cx, 4); put(&p, (uint32_t)k.cz, 4);
	put(&p, k.dirty_generation, 4); put(&p, k.world_epoch, 2);
	put(&p, k.cy, 1); put(&p, k.stage & 192, 1);
	if(obs.closed_count > UINT16_MAX) valid = false;
	put(&p, obs.closed_count > UINT16_MAX ? UINT16_MAX : obs.closed_count, 2);
	put(&p, obs.closed_count && valid && obs.path_valid ? obs.hops : 0, 1);
	put(&p, (valid ? 1 : 0) | (obs.finished ? 2 : 0)
		| (obs.closed_count && obs.path_valid ? 4 : 0), 1);
	for(unsigned h = 0; h < 8; h++) {
		bool used = obs.closed_count && valid && obs.path_valid && h < obs.hops;
		put(&p, used ? obs.path_stage[h] : 0, 1);
		put(&p, used ? obs.path_id[h] : 0, 4);
		put(&p, used ? obs.path_us[h] : 0, 4);
	}
	put(&p, obs.closed_count ? obs.closed_end : 0, 8);
	/* Teardown diagnostics. Module statics, not capture state: they survive a
	 * retire-in-place reset on purpose, so a hardware rerun can tell a slow
	 * drain (starvation: drain_ms near the 200 ms budget) from a teardown that
	 * carried its buffer (deferred>0) from a stall that never reached either
	 * (both zero). Saturating, never wrapping -- see the payload note above.
	 *
	 * WHAT THEY DO NOT SAY (corrected, round 3): they discriminate the
	 * SHUTDOWN and ARM paths only. In production start()/stop() run exactly
	 * twice -- the --cw-tps-cpu arm and the shutdown stop -- so a record
	 * emitted while the game is running reads 0/0 here BY CONSTRUCTION, and
	 * that is the answer to "did the stall reach a teardown?", not a missing
	 * measurement. The world-reset path is memset-only and never touches
	 * either: its cost lives in reset_retirements/last_reset_us, which have no
	 * wire byte to spare (the payload is exactly at the envelope ceiling) and
	 * are read out of process by the deferred trace line client_interface.c
	 * writes AFTER the reset. */
	uint32_t drain_ms = last_drain_us / 1000u;
	put(&p, deferred_retirements > 255u ? 255u : deferred_retirements, 1);
	put(&p, drain_ms > 255u ? 255u : drain_ms, 1);
	obs.closed_count = 0;
	uint32_t seq = obs.sequence++;
	thread_mutex_unlock(&mutex);
	int off = snprintf(line, sizeof(line), "{\"event\":\"settle\",\"v\":2,\"seq\":%u,\"b\":\"", seq);
	const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	for(size_t i = 0; i < sizeof(payload); i += 3) {
		uint32_t v = (uint32_t)payload[i] << 16;
		if(i + 1 < sizeof(payload)) v |= (uint32_t)payload[i + 1] << 8;
		if(i + 2 < sizeof(payload)) v |= payload[i + 2];
		line[off++] = alphabet[v >> 18]; line[off++] = alphabet[(v >> 12) & 63];
		line[off++] = i + 1 < sizeof(payload) ? alphabet[(v >> 6) & 63] : '=';
		line[off++] = i + 2 < sizeof(payload) ? alphabet[v & 63] : '=';
	}
	memcpy(line + off, "\"}", 3);
	if(!emit || !emit(line)) {
		thread_mutex_lock(&mutex);
		if(state) lost();
		thread_mutex_unlock(&mutex);
	}
}
void settle_observer_publish(bool (*emit)(const char*)) {
	if(!settle_observer_active()) return;
#ifdef PLATFORM_WII
	(void)emit;
	__atomic_fetch_or(&settle_observer_control, 1u, __ATOMIC_RELEASE);
#else
	drain_now(emit);
#endif
}
bool settle_observer_pump(bool (*emit)(const char*)) {
	if(!(__atomic_fetch_and(&settle_observer_control, ~1u, __ATOMIC_ACQ_REL) & 1u)) return false;
	drain_now(emit);
	return true;
}
void settle_observer_finish(void) {
	if(!settle_observer_active()) return;
	thread_mutex_lock(&mutex);
	if(state) { obs.episode = false; obs.finished = true; }
	thread_mutex_unlock(&mutex);
}
bool settle_observer_pull(struct settle_event* out, size_t cap, size_t* used) {
	*used = 0;
	if(!settle_observer_active()) return false;
	thread_mutex_lock(&mutex);
	bool ok = state && obs.raw_count && obs.raw_count <= cap && out;
	if(ok) {
		*used = obs.raw_count;
		memcpy(out, obs.raw_worst, *used * sizeof(*out));
	}
	thread_mutex_unlock(&mutex);
	return ok;
}
/* The ONE place the armed buffer is released. Both stop() paths below funnel
 * through it so they can never diverge on what "retired" means. */
static void retire_state(void) {
	thread_mutex_lock(&mutex);
	struct settle_state* released = state;
	state = NULL;
	__atomic_store_n(&active, false, __ATOMIC_RELEASE);
	thread_mutex_unlock(&mutex);
	if(released)
		mem2_free_sized(released, sizeof(*released));
}
void settle_observer_stop(bool (*emit)(const char*)) {
	(void)emit; /* publication belongs to the 1 Hz/profile boundary only */
	if(!settle_observer_active()) {
		/* NOT a no-op, and this is the round-2 LEAK. `active` false does not
		 * imply "nothing to free": a stop() whose drain expired sets active
		 * false and KEEPS the buffer, and a start() whose drain also expires
		 * returns false without re-arming. The observer is then inactive while
		 * still owning its 150,936 B MEM2 block, and the old early return took
		 * shutdown straight past the only free() in the module.
		 *
		 * Shutdown is the last caller, so it retires unconditionally instead.
		 * Every hook is already NULL and the generation already moved, so
		 * nothing new can enter; the in-flight drain is the only thing between
		 * the carried block and the free, and main.c calls this after the
		 * server join, chunk_mesher_shutdown() and hw_telemetry_shutdown(), so
		 * that drain has no straggler left to find. If one somehow remains the
		 * block is STILL not freed under a live writer -- counted, never a
		 * silent use-after-free. */
		if(!state)
			return;
		if(!drain_inflight()) {
			deferred_retirements++;
			return;
		}
		retire_state();
		return;
	}
	__atomic_store_n(&chunk_settle_observe, NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&world_settle_observe, NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&mesher_settle_observe, NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&client_settle_observe, NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&server_settle_observe, NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&mem2_settle_observe, NULL, __ATOMIC_RELEASE);
	/* Publish a new generation after every hook is closed. A caller that
	 * already loaded a hook carries the old value and is rejected under the
	 * mutex; a caller already inside observe_body is counted in the gate the
	 * drain below waits on. */
	__atomic_fetch_add(&settle_observer_control, 2u, __ATOMIC_RELEASE);
	__atomic_fetch_and(&settle_observer_control, ~1u, __ATOMIC_RELEASE);
	/* Generation flip + BOUNDED in-flight drain instead of a blocking lock: see
	 * the IN-FLIGHT GATE note above. The drain is EXACT, not best-effort -- the
	 * SETTLE macro enters the gate before it re-validates the hook, and the
	 * hooks were nulled above before this drain began, so any caller that will
	 * still reach observe_body is already counted.
	 *
	 * This does NOT claim the lock below is uncontended: settle_observer_frame/
	 * _counts/_pull/_finish (main) and settle_observer_pump (the prio-40
	 * telemetry sender) take the mutex directly and are deliberately NOT part of
	 * the gate. Counting them would let a 1 Hz pump burn a teardown's whole
	 * drain budget for no safety gain, and skipping them would silently drop the
	 * frame bookkeeping and the records the observer exists to produce. What
	 * bounds the lock instead is the shape of those critical sections: each is a
	 * fixed amount of in-memory work with no allocation, no I/O and no nested
	 * lock (drain_now unlocks before it formats or emits), so main waits out one
	 * bounded section, never a worker's engine work. */
#ifdef CAVEWII_TESTING
	if(blocking_stop_seam) {
		/* Test-only: the pre-fix teardown. It WAITS ON THE MUTEX instead of
		 * draining the gate, which is the whole behaviour being demonstrated --
		 * a callback wedged inside observe_body parks main for as long as it
		 * holds the lock. The retirement below then runs as usual. */
		thread_mutex_lock(&mutex);
		thread_mutex_unlock(&mutex);
	} else
#endif
	if(!drain_inflight()) {
		/* A callback is still inside observe_body. Disarm it (hooks are already
		 * NULL and the generation already moved, so nothing new can enter) but
		 * do NOT free the buffer under a live writer. `state` stays the module's
		 * sole buffer and is REUSED by the next settle_observer_start(), which
		 * repeats the drain -- there is never a second allocation and never an
		 * unreachable block. Counted, so the record names it. */
		deferred_retirements++;
		__atomic_store_n(&active, false, __ATOMIC_RELEASE);
		return;
	}
	/* Shutdown frees unconditionally: main.c calls this after the server join,
	 * chunk_mesher_shutdown() and (on Wii) hw_telemetry_shutdown(), i.e. after
	 * every thread that can emit a SETTLE has been joined, so the drain above
	 * cannot find a straggler and this free always runs. */
	retire_state();
}
uint32_t settle_observer_deferred_retirements(void) {
	return deferred_retirements;
}
uint32_t settle_observer_last_drain_us(void) {
	return last_drain_us;
}
uint32_t settle_observer_reset_retirements(void) {
	return reset_retirements;
}
uint32_t settle_observer_last_reset_us(void) {
	return last_reset_us;
}
#ifdef CAVEWII_TESTING
bool settle_observer_test_blocking_stop(bool blocking) {
	blocking_stop_seam = blocking;
	return true;
}
#endif

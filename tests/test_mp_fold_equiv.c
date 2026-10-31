/* IN-009b: byte-exact fold witnesses for a retained remote column.
 *
 * What Beta does (IN-009 plan step 3, tmp/jobs/loop/IN-009-astra-plan.md):
 * the client applies EVERY Packet51 region to a held chunk in arrival order,
 * whatever the render radius -- NetClientHandler.handleMapChunk ->
 * World.setChunkData -> Chunk.setChunkData (client CFR, class-local lines
 * 362-365 / 1913-1949 / 480-518 per the plan). Three queued partials is no
 * Beta limit: PlayerInstance.java:90-153 in the server decompile emits any
 * number of bounding-box Packet51s per column over a session. Packet52/53
 * apply in arrival order too, so an edit OUTSIDE a later region survives it
 * and an edit inside a later region loses to it.
 *
 * Wire layout of a region body (the server's encoder, mirrored by the client
 * decoder): C:/tmp/mcb173-server-decompile/out/net/minecraft/src/
 * Chunk.java:474-519 getChunkData and World.java:1728-1765 -- for x in
 * [x0,x1), z in [z0,z1): the y stripe of block ids at (x<<11 | z<<7 | y0),
 * length y1-y0; then the same x/z walk three more times copying
 * (y1-y0)/2 BYTES at index >> 1 from the metadata, block-light and
 * sky-light nibble arrays, in that order. The 81920-byte full column is the
 * same layout with the whole column as the region.
 *
 * THE REFERENCE is an independent re-implementation of that sequential
 * application (fe_ref_apply / fe_ref_edit). It is updated at each packet's
 * WIRE position when the witness builds the packet, for every valid offered
 * region, whether or not production later accepts it: it never calls the
 * production fold, export or codec, and never looks at production's answer.
 *
 * THE READ THREAD / PUMP SPLIT. Every packet the transport's read thread
 * handles (Packet51 retention, Packet50 load/unload, Packet9 dimension
 * change, and the receive-stamped queueing of Packet52/53) is executed on a
 * separate READER thread under impl->lock, in wire order, exactly as
 * server_remote_transport.c's read loop does (lines 1040-1080). The main
 * thread is the PUMP: it drains the queued events through the production
 * process_event and then runs the promotion/residency steps under the lock,
 * in server_remote_pump's order.
 *
 * HELD PUMP. A burst of reader packets runs with the pump HELD: the main
 * thread does not pump while the reader works, so the reader meets fold
 * pressure with older edits still queued and/or a promotion still owning the
 * column's head. A fixed implementation may legitimately make the reader
 * wait for the pump (plan step 5: busy promotion/recode and unprocessed older
 * events are transient backpressure), so the pump may be released for a
 * waiting reader -- but NEVER before the burst's RACE GATE has been passed.
 *
 * RACE GATE (a handshake, not a timer). A race witness marks the reader
 * packet at which its race happens (the fold-pressure body, the Packet50
 * unload, the Packet9 dimension change). The reader itself, under impl->lock
 * and at that exact packet, snapshots production-visible state (receive-queue
 * depth, the column's entry state, the promotion's owner) and only then
 * signals the gate; until the gate is signalled the pump cannot run at all.
 * After the burst the snapshot is judged: the required state (every older
 * edit still queued, the promotion still mid-decode on this column, a head
 * held) must have been reached, or the witness exits 2 as SETUP naming the
 * state it failed to reach. So every race is exercised before any byte
 * verdict, and no check is skipped or depends on scheduling. A pump released
 * after the gate only steps a valid Beta-order schedule; the final bytes must
 * still match. A reader that never reaches its gate or never finishes
 * (FE_HARD_MS), or any wait that never returns (watchdog), exits 2 as SETUP
 * -- never a hang, never a test-runner timeout.
 *
 * Witnesses (plan steps 5 and 9): >3 non-covering partials, overlap,
 * light-only partials, Packet52/53 edits inside and outside later regions
 * (applied by the pump between bodies, and still QUEUED when folding becomes
 * necessary), held-pump backpressure with a promotion in flight, unload
 * during an active promotion, dimension change during an active promotion
 * and during an active demotion re-encode, an out-of-radius column with a
 * demote/promote round trip, and the real column (12,-9) of the IN-007 walk
 * fixture compared BEFORE promotion (backed) and after promotion on entry.
 * Two CONTROL witnesses with at most three queued bodies validate the
 * reference against today's production; a control failure is a harness
 * fault (exit 2), not RED.
 *
 * Edit witnesses use opaque ids and all-zero light planes in every body, so
 * the client's relight around an edit has nothing to change and all four
 * planes stay comparable byte for byte.
 *
 * Output: "PASS <witness>" per witness; any refused body, any byte mismatch
 * or any ended session prints "IN-009b RED: <witness>: ..." and the test
 * exits 1. Usage: test_mp_fold_equiv <tests/fixtures/mp_walk/stream.bin>. */
#define main existing_mp_suite_main
#include "test_mp_s0_env.c"
#undef main
#include "lighting.h"

#define FE_CELLS (16u * 128u * 16u)
#define FE_NIB (FE_CELLS / 2u)
#define FE_RAW (FE_CELLS + 3u * FE_NIB) /* 81920 */
#define FE_PUMPS 400
#define FE_HOLD_MS 250u      /* pump held this long before a waiting reader */
#define FE_HARD_MS 30000u    /* a reader still unfinished after this = hang */
#define FE_WATCHDOG_MS 180000u
#define FE_ACTS_MAX 48

static struct server_remote fe_owner;
static struct server_remote_impl* s_r;
static int fe_red_witnesses;
static int fe_witnesses_run;
static int fe_setup_faults;
static const char* volatile fe_current = "init";
static atomic_bool fe_all_done;

static void fe_setup_exit(const char* what) {
	printf("IN-009b SETUP: witness %s: %s\n", fe_current, what);
	fflush(stdout);
	_Exit(2);
}

/* Bounds every wait in the file, including a lock that is never released. */
static void* fe_watchdog_entry(void* user) {
	(void)user;
	ptime_t end = time_add_ms(time_get(), FE_WATCHDOG_MS);
	while(!atomic_load(&fe_all_done)) {
		if(time_reached(time_get(), end))
			fe_setup_exit("watchdog: no verdict within the bound (a wait hung)");
		thread_msleep(20);
	}
	return NULL;
}

/* A Packet51 region, LOCAL to its column (x0+sx <= 16, z0+sz <= 16,
 * y0+sy <= 128; y0 and sy even, as PlayerInstance always sends). */
struct fe_region {
	int x0, y0, z0, sx, sy, sz;
};

static const struct fe_region FE_FULL = {0, 0, 0, 16, 128, 16};

/* ---- the independent reference (CFR Chunk.getChunkData mirror) --------- */

static size_t fe_index(int x, int y, int z) {
	return ((size_t)x << 11) | ((size_t)z << 7) | (size_t)y;
}

/* Encode `src`'s region into Packet51 body order. Returns the length. */
static size_t fe_encode(const uint8_t* src, const struct fe_region* r,
						uint8_t* out) {
	size_t o = 0;
	for(int x = r->x0; x < r->x0 + r->sx; x++)
		for(int z = r->z0; z < r->z0 + r->sz; z++) {
			memcpy(out + o, src + fe_index(x, r->y0, z), (size_t)r->sy);
			o += (size_t)r->sy;
		}
	for(unsigned plane = 0; plane < 3; plane++) {
		const uint8_t* base = src + FE_CELLS + plane * FE_NIB;
		for(int x = r->x0; x < r->x0 + r->sx; x++)
			for(int z = r->z0; z < r->z0 + r->sz; z++) {
				size_t n = (size_t)r->sy / 2u;
				memcpy(out + o, base + (fe_index(x, r->y0, z) >> 1), n);
				o += n;
			}
	}
	return o;
}

/* Sequential CFR-region application: the client's Chunk.setChunkData. */
static void fe_ref_apply(uint8_t* ref, const struct fe_region* r,
						 const uint8_t* body) {
	size_t o = 0;
	for(int x = r->x0; x < r->x0 + r->sx; x++)
		for(int z = r->z0; z < r->z0 + r->sz; z++) {
			memcpy(ref + fe_index(x, r->y0, z), body + o, (size_t)r->sy);
			o += (size_t)r->sy;
		}
	for(unsigned plane = 0; plane < 3; plane++) {
		uint8_t* base = ref + FE_CELLS + plane * FE_NIB;
		for(int x = r->x0; x < r->x0 + r->sx; x++)
			for(int z = r->z0; z < r->z0 + r->sz; z++) {
				size_t n = (size_t)r->sy / 2u;
				memcpy(base + (fe_index(x, r->y0, z) >> 1), body + o, n);
				o += n;
			}
	}
}

/* Packet53 on the reference: id byte plus the NibbleArray metadata nibble
 * (low nibble for an even index). */
static void fe_ref_edit(uint8_t* ref, int lx, int y, int lz, uint8_t id,
						uint8_t meta) {
	size_t i = fe_index(lx, y, lz);
	ref[i] = id;
	uint8_t* m = ref + FE_CELLS + (i >> 1);
	if(i & 1u)
		*m = (uint8_t)((*m & 0x0Fu) | ((meta & 0x0Fu) << 4));
	else
		*m = (uint8_t)((*m & 0xF0u) | (meta & 0x0Fu));
}

/* ---- seeded content ------------------------------------------------------ */

enum fe_palette { FE_PAL_ALL, FE_PAL_OPAQUE };
enum fe_light { FE_LIGHT_RANDOM, FE_LIGHT_ZERO };

static uint32_t fe_rng(uint32_t* s) {
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

static uint8_t fe_pick_id(uint32_t r, enum fe_palette pal) {
	static const uint8_t all[] = {BLOCK_AIR,  BLOCK_STONE,  BLOCK_DIRT,
								  BLOCK_SAND, BLOCK_GRAVEL, BLOCK_COBBLESTONE};
	static const uint8_t opaque[] = {BLOCK_STONE, BLOCK_DIRT, BLOCK_COBBLESTONE};
	return pal == FE_PAL_ALL ? all[r % (sizeof(all) / sizeof(all[0]))]
							 : opaque[r % (sizeof(opaque) / sizeof(opaque[0]))];
}

/* Short runs: varies cell by cell (a wrong offset or a swapped light plane
 * shows) but deflates small. */
static void fe_fill_light(uint32_t* s, uint8_t* src, enum fe_light light) {
	for(unsigned plane = 0; plane < 3; plane++) {
		uint8_t* dst = src + FE_CELLS + plane * FE_NIB;
		if(plane > 0 && light == FE_LIGHT_ZERO) {
			memset(dst, 0, FE_NIB);
			continue;
		}
		size_t j = 0;
		while(j < FE_NIB) {
			uint32_t r = fe_rng(s);
			size_t run = 1 + (r >> 8) % 7u;
			for(size_t k = 0; k < run && j < FE_NIB; k++, j++)
				dst[j] = (uint8_t)(r & 0xFFu);
		}
	}
}

static void fe_fill_source(uint32_t* s, uint8_t* src, enum fe_palette pal,
						   enum fe_light light) {
	size_t i = 0;
	while(i < FE_CELLS) {
		uint32_t r = fe_rng(s);
		uint8_t id = fe_pick_id(r, pal);
		size_t run = 1 + (r >> 8) % 13u;
		for(size_t k = 0; k < run && i < FE_CELLS; k++, i++)
			src[i] = id;
	}
	fe_fill_light(s, src, light);
}

/* ---- witness state ------------------------------------------------------- */

struct fe_witness {
	const char* name;
	int cx, cz;
	bool control;
	bool red;
	bool pumped_during_burst; /* the hold was released for a waiting reader */
	int races_required; /* race gates this witness declared */
	int races_verified; /* race gates whose required state was reached */
	int bodies;   /* bodies offered for this column */
	int refused;  /* bodies the production retention refused */
	uint8_t ref[FE_RAW];
	uint8_t src[FE_RAW];
	uint8_t wire[FE_RAW];
	uint8_t out[FE_RAW];
};

static struct fe_witness fe_w;

static void fe_begin(const char* name, int cx, int cz, bool control) {
	memset(&fe_w, 0, sizeof(fe_w));
	fe_w.name = name;
	fe_w.cx = cx;
	fe_w.cz = cz;
	fe_w.control = control;
	fe_current = name;
	fe_witnesses_run++;
}

static void fe_fail(const char* fmt, const char* detail) {
	if(fe_w.control) {
		printf("IN-009b SETUP: control witness %s failed: ", fe_w.name);
		printf(fmt, detail);
		printf("\n");
		fe_setup_faults++;
	} else {
		printf("IN-009b RED: %s: ", fe_w.name);
		printf(fmt, detail);
		printf("\n");
		fe_w.red = true;
	}
}

static void fe_check_region(const struct fe_region* r) {
	if(r->sx < 1 || r->sz < 1 || r->sy < 2 || (r->y0 & 1) || (r->sy & 1)
	   || r->x0 < 0 || r->z0 < 0 || r->y0 < 0 || r->x0 + r->sx > 16
	   || r->z0 + r->sz > 16 || r->y0 + r->sy > 128)
		fe_setup_exit("invalid region");
}

static size_t fe_entry_locked(int cx, int cz) {
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++)
		if(s_r->chunks[i].state != REMOTE_CHUNK_UNUSED
		   && s_r->chunks[i].chunk_x == cx && s_r->chunks[i].chunk_z == cz)
			return i;
	return REMOTE_CHUNK_ENTRY_CAPACITY;
}

static bool fe_installed(void) {
	return world_find_chunk(&gstate.world, fe_w.cx * 16, 0, fe_w.cz * 16)
		!= NULL;
}

/* ---- the PUMP (main thread, server_remote_pump's order) ----------------- */

static bool fe_pump_dead; /* a promotion step ended the session */

static void fe_pump_once(void) {
	/* Drain the receive-stamped events, oldest first (server_remote.c:278-314). */
	for(;;) {
		struct remote_event event;
		thread_mutex_lock(&s_r->lock);
		bool have = s_r->event_count != 0;
		if(have)
			event = s_r->events[s_r->event_head];
		bool current = have && event.generation == s_r->generation;
		thread_mutex_unlock(&s_r->lock);
		if(!have)
			break;
		if(current) {
			s_r->pump_packet_seq = event.seq;
			s_r->pump_packet_sub = 0;
			(void)process_event(&fe_owner, &event);
		} else
			mp_event_destroy(&event.packet);
		thread_mutex_lock(&s_r->lock);
		s_r->event_head = (s_r->event_head + 1) % REMOTE_QUEUE_CAPACITY;
		s_r->event_count--;
		thread_mutex_unlock(&s_r->lock);
	}
	/* The pump tail (server_remote.c:363-376). */
	thread_mutex_lock(&s_r->lock);
	remote_emit_pending_unloads_locked(s_r);
	if(!s_r->stop_now) {
		if(!remote_promotion_step_locked(s_r) && !fe_pump_dead) {
			fe_pump_dead = true;
			char detail[320];
			snprintf(detail, sizeof(detail),
					 "a promotion step ended the session: reason=\"%s\"",
					 s_r->reason);
			thread_mutex_unlock(&s_r->lock);
			fe_fail("%s", detail);
			thread_mutex_lock(&s_r->lock);
		}
		remote_residency_step_locked(s_r);
	}
	thread_mutex_unlock(&s_r->lock);
}

static void fe_pump(int n) {
	for(int i = 0; i < n; i++)
		fe_pump_once();
}

/* Promotion steps only (no drain, no residency), until the promotion is
 * mid-inflate on this witness's column and owns its head body. */
static void fe_promote_until_in_flight(void) {
	for(int i = 0; i < 400; i++) {
		thread_mutex_lock(&s_r->lock);
		size_t target = fe_entry_locked(fe_w.cx, fe_w.cz);
		bool in_flight = target != REMOTE_CHUNK_ENTRY_CAPACITY
			&& s_r->promotion.active && s_r->promotion.entry == target
			&& !s_r->promotion.ready_to_publish
			&& s_r->promotion.stream.total_out > 0
			&& s_r->chunks[target].state == REMOTE_CHUNK_PROMOTING;
		bool ok = in_flight || remote_promotion_step_locked(s_r);
		thread_mutex_unlock(&s_r->lock);
		if(in_flight)
			return;
		if(!ok)
			fe_setup_exit("a promotion step failed while starting the race");
	}
	fe_setup_exit("the column's promotion never started");
}

/* ---- the READER (read-thread packets, wire order, under impl->lock) ----- */

enum fe_act_kind {
	FE_ACT_BODY,
	FE_ACT_QEDIT,
	FE_ACT_LOAD,
	FE_ACT_UNLOAD,
	FE_ACT_RESPAWN,
};

enum fe_gate_mode {
	FE_GATE_NONE,
	FE_GATE_PRE,  /* signalled just before the packet is handled */
	FE_GATE_POST, /* signalled once the packet has been handled */
};

/* Production-visible state, read by the reader under impl->lock. */
struct fe_snap {
	bool taken;
	uint32_t queued_events; /* receive-queue depth (unprocessed 52/53s) */
	bool head_held;         /* the column holds a head (RETAINED/PROMOTING) */
	bool promo_in_flight;   /* the promotion is mid-decode on this column */
	bool promo_on_entry;    /* the promotion still owns the gate's entry */
	size_t entry;
};

struct fe_act {
	enum fe_act_kind kind;
	enum fe_gate_mode gate;
	const char* gate_name;
	uint32_t want_queued;
	bool want_promo;
	struct fe_snap pre, post;
	int cx, cz;
	struct mp_event event;
	int body_no;
	bool refused;
	bool fault;
	uint32_t reconnects_before, reconnects_after;
	char reason[256];
};

static struct fe_act fe_acts[FE_ACTS_MAX];
static size_t fe_act_count;
static atomic_bool fe_reader_finished;
static atomic_bool fe_gate_passed;
static bool fe_flush_active;

/* `entry` is the entry to test promotion ownership against
 * (REMOTE_CHUNK_ENTRY_CAPACITY: this column's current entry). */
static void fe_snap_locked(const struct fe_act* a, size_t entry,
						   struct fe_snap* out) {
	size_t here = fe_entry_locked(a->cx, a->cz);
	if(entry == REMOTE_CHUNK_ENTRY_CAPACITY)
		entry = here;
	out->taken = true;
	out->entry = here;
	out->queued_events = (uint32_t)s_r->event_count;
	out->head_held = here != REMOTE_CHUNK_ENTRY_CAPACITY
		&& (s_r->chunks[here].state == REMOTE_CHUNK_RETAINED
			|| s_r->chunks[here].state == REMOTE_CHUNK_PROMOTING);
	out->promo_in_flight = here != REMOTE_CHUNK_ENTRY_CAPACITY
		&& s_r->promotion.active && s_r->promotion.entry == here
		&& !s_r->promotion.ready_to_publish
		&& s_r->chunks[here].state == REMOTE_CHUNK_PROMOTING;
	out->promo_on_entry = entry != REMOTE_CHUNK_ENTRY_CAPACITY
		&& s_r->promotion.active && s_r->promotion.entry == entry
		&& !s_r->promotion.ready_to_publish;
}

/* One read-thread packet (server_remote_transport.c:1040-1080). */
static void fe_reader_act_locked(struct fe_act* a) {
	switch(a->kind) {
		case FE_ACT_BODY:
			a->reconnects_before = s_r->forced_reconnects;
			a->refused = !remote_retain_map_locked(s_r, &a->event);
			a->reconnects_after = s_r->forced_reconnects;
			if(a->refused)
				snprintf(a->reason, sizeof(a->reason), "%s", s_r->reason);
			break;
		case FE_ACT_QEDIT: {
			/* push_event_locked: stamp from the shared receive counter. */
			if(s_r->event_count >= REMOTE_QUEUE_CAPACITY || s_r->stop_now) {
				a->fault = true;
				break;
			}
			struct remote_event event;
			memset(&event, 0, sizeof(event));
			event.kind = REMOTE_EVENT_PACKET;
			event.generation = s_r->generation;
			event.world_epoch = s_r->world_epoch;
			event.packet = a->event;
			event.seq = ++s_r->arrival_sequence;
			s_r->events[(s_r->event_head + s_r->event_count)
						% REMOTE_QUEUE_CAPACITY]
				= event;
			s_r->event_count++;
			memset(&a->event, 0, sizeof(a->event)); /* the queue owns it */
		} break;
		case FE_ACT_LOAD:
		case FE_ACT_UNLOAD:
			a->fault = !remote_pre_chunk_locked(s_r, a->cx, a->cz,
												a->kind == FE_ACT_LOAD);
			break;
		case FE_ACT_RESPAWN:
			/* A Packet9 that changes dimension, in stream order. */
			s_r->world_epoch++;
			remote_chunks_clear_locked(s_r);
			break;
	}
}

static void* fe_reader_entry(void* user) {
	(void)user;
	for(size_t i = 0; i < fe_act_count; i++) {
		struct fe_act* a = &fe_acts[i];
		thread_mutex_lock(&s_r->lock);
		if(a->gate != FE_GATE_NONE)
			fe_snap_locked(a, REMOTE_CHUNK_ENTRY_CAPACITY, &a->pre);
		/* A PRE gate opens before the packet is handled: a fixed production
		 * may wait for the pump inside it (releasing the lock). */
		if(a->gate == FE_GATE_PRE)
			atomic_store(&fe_gate_passed, true);
		fe_reader_act_locked(a);
		if(a->gate != FE_GATE_NONE)
			fe_snap_locked(a, a->pre.entry, &a->post);
		thread_mutex_unlock(&s_r->lock);
		if(a->gate == FE_GATE_POST)
			atomic_store(&fe_gate_passed, true);
	}
	atomic_store(&fe_reader_finished, true);
	return NULL;
}

static struct fe_act* fe_new_act(enum fe_act_kind kind) {
	if(fe_act_count == FE_ACTS_MAX)
		fe_setup_exit("too many reader packets in one burst");
	struct fe_act* a = &fe_acts[fe_act_count++];
	memset(a, 0, sizeof(*a));
	a->kind = kind;
	a->cx = fe_w.cx;
	a->cz = fe_w.cz;
	return a;
}

/* Mark the last queued reader packet as this burst's race gate. */
static void fe_gate_last(enum fe_gate_mode mode, const char* name,
						 uint32_t want_queued, bool want_promo) {
	if(!fe_act_count || fe_acts[fe_act_count - 1].gate != FE_GATE_NONE)
		fe_setup_exit("race gate on no packet");
	struct fe_act* a = &fe_acts[fe_act_count - 1];
	a->gate = mode;
	a->gate_name = name;
	a->want_queued = want_queued;
	a->want_promo = want_promo;
	fe_w.races_required++;
}

/* Judge a gate's snapshot after the burst: the required state was reached
 * (else SETUP, exit 2), and an unload gate cancelled the promotion. */
static void fe_judge_gate(const struct fe_act* a) {
	char detail[320];
	if(!a->pre.taken || !a->post.taken) {
		snprintf(detail, sizeof(detail), "the reader never reached %s",
				 a->gate_name);
		fe_setup_exit(detail);
	}
	if(!a->pre.head_held) {
		snprintf(detail, sizeof(detail),
				 "at %s the column held no head (required state not reached)",
				 a->gate_name);
		fe_setup_exit(detail);
	}
	if(a->pre.queued_events != a->want_queued) {
		snprintf(detail, sizeof(detail),
				 "at %s %u edit(s) were queued, the race needs %u still "
				 "unprocessed (required state not reached)",
				 a->gate_name, (unsigned)a->pre.queued_events,
				 (unsigned)a->want_queued);
		fe_setup_exit(detail);
	}
	if(a->want_promo && !a->pre.promo_in_flight) {
		snprintf(detail, sizeof(detail),
				 "at %s the column's promotion was not mid-decode (required "
				 "state not reached)",
				 a->gate_name);
		fe_setup_exit(detail);
	}
	printf("  race %s: %s reached with %u queued edit(s)%s before any pump "
		   "step\n",
		   fe_w.name, a->gate_name, (unsigned)a->pre.queued_events,
		   a->want_promo ? " and the promotion mid-decode" : "");
	fe_w.races_verified++;
	if(a->kind == FE_ACT_UNLOAD && a->post.promo_on_entry)
		fe_fail("%s", "the unload did not cancel the in-flight promotion");
}

/* Run the queued reader packets with the pump HELD (see the top comment). */
static void fe_flush_ex(bool may_pump) {
	if(!fe_act_count)
		return;
	if(fe_flush_active)
		fe_setup_exit("nested reader burst");
	fe_flush_active = true;
	bool gated = false;
	for(size_t i = 0; i < fe_act_count; i++)
		if(fe_acts[i].gate != FE_GATE_NONE) {
			if(gated)
				fe_setup_exit("two race gates in one burst");
			gated = true;
		}
	atomic_store(&fe_gate_passed, !gated);
	atomic_store(&fe_reader_finished, false);
	struct thread reader;
	if(!thread_create(&reader, fe_reader_entry, NULL, 64))
		fe_setup_exit("reader thread create failed");
	ptime_t start = time_get();
	ptime_t hold_end = time_add_ms(start, FE_HOLD_MS);
	ptime_t hard_end = time_add_ms(start, FE_HARD_MS);
	while(!atomic_load(&fe_reader_finished)) {
		ptime_t now = time_get();
		if(time_reached(now, hard_end))
			fe_setup_exit(atomic_load(&fe_gate_passed)
							  ? "the reader never finished its burst (hang)"
							  : "the reader never reached the burst's race "
								"gate (required state not reached)");
		/* The pump is released only after the race gate (the handshake). */
		if(may_pump && atomic_load(&fe_gate_passed)
		   && time_reached(now, hold_end)) {
			fe_w.pumped_during_burst = true;
			fe_pump_once();
		}
		thread_msleep(1);
	}
	thread_join(&reader);
	fe_flush_active = false;
	if(fe_w.pumped_during_burst)
		printf("  note %s: the reader waited past the hold; the pump was "
			   "released\n", fe_w.name);
	for(size_t i = 0; i < fe_act_count; i++) {
		struct fe_act* a = &fe_acts[i];
		if(a->fault)
			fe_setup_exit(a->kind == FE_ACT_QEDIT
							  ? "the event queue could not take a queued edit"
							  : "a Packet50 load/unload was refused");
		if(a->gate != FE_GATE_NONE)
			fe_judge_gate(a);
		if(a->kind != FE_ACT_BODY)
			continue;
		const struct mp_map_chunk* w = &a->event.payload.map_chunk;
		if(a->refused) {
			char detail[640];
			snprintf(detail, sizeof(detail),
					 "body %d (x=%d y=%d z=%d %ux%ux%u) refused: "
					 "forced_reconnects %u -> %u, reason=\"%s\"",
					 a->body_no, (int)w->x, (int)w->y, (int)w->z,
					 (unsigned)w->sx, (unsigned)w->sy, (unsigned)w->sz,
					 (unsigned)a->reconnects_before,
					 (unsigned)a->reconnects_after, a->reason);
			fe_w.refused++;
			fe_fail("%s", detail);
			mp_event_destroy(&a->event); /* ownership stayed here */
		} else if(w->compressed)
			fe_setup_exit("accepted body not taken");
	}
	fe_act_count = 0;
}

static void fe_flush(void) {
	fe_flush_ex(true);
}

/* Queue one body (region of `fe_w.src`) for the reader, and apply it to the
 * reference at its wire position -- unconditionally (Beta applies every
 * region to a held chunk). */
static void fe_q_body(const struct fe_region* r) {
	fe_check_region(r);
	size_t bytes = fe_encode(fe_w.src, r, fe_w.wire);
	if(bytes != (size_t)r->sx * (size_t)r->sy * (size_t)r->sz * 5u / 2u)
		fe_setup_exit("encoder length");
	uLongf n = compressBound((uLong)bytes);
	unsigned char* compressed = malloc(n);
	if(!compressed || compress(compressed, &n, fe_w.wire, (uLong)bytes) != Z_OK)
		fe_setup_exit("compress failed");
	struct fe_act* a = fe_new_act(FE_ACT_BODY);
	a->event.type = MP_EVENT_MAP_CHUNK;
	a->event.payload.map_chunk = (struct mp_map_chunk) {
		.x = fe_w.cx * 16 + r->x0, .y = (int16_t)r->y0, .z = fe_w.cz * 16 + r->z0,
		.sx = (uint16_t)r->sx, .sy = (uint16_t)r->sy, .sz = (uint16_t)r->sz,
		.compressed = compressed, .compressed_length = n,
		.inflated_length = bytes};
	a->body_no = ++fe_w.bodies;
	fe_ref_apply(fe_w.ref, r, fe_w.wire);
}

/* One body as its own held burst. */
static void fe_body(const struct fe_region* r) {
	fe_q_body(r);
	fe_flush();
}

static void fe_q_load(void) {
	(void)fe_new_act(FE_ACT_LOAD);
}
static void fe_q_unload(void) {
	(void)fe_new_act(FE_ACT_UNLOAD);
}
static void fe_q_respawn(void) {
	(void)fe_new_act(FE_ACT_RESPAWN);
}

struct fe_cell {
	int lx, y, lz;
	uint8_t id, meta;
};

/* Packet53 received by the reader and QUEUED for the pump. */
static void fe_q_edit53(int lx, int y, int lz, uint8_t id, uint8_t meta) {
	struct fe_act* a = fe_new_act(FE_ACT_QEDIT);
	a->event.type = MP_EVENT_BLOCK_CHANGE;
	a->event.payload.block_change.x = fe_w.cx * 16 + lx;
	a->event.payload.block_change.y = (uint8_t)y;
	a->event.payload.block_change.z = fe_w.cz * 16 + lz;
	a->event.payload.block_change.block_type = id;
	a->event.payload.block_change.metadata = meta;
	fe_ref_edit(fe_w.ref, lx, y, lz, id, meta);
}

/* Packet52 received by the reader and QUEUED for the pump (cell order). */
static void fe_q_edit52(const struct fe_cell* cells, size_t count) {
	struct fe_act* a = fe_new_act(FE_ACT_QEDIT);
	a->event.type = MP_EVENT_MULTI_BLOCK_CHANGE;
	struct mp_multi_block_change* m = &a->event.payload.multi_block_change;
	m->chunk_x = fe_w.cx;
	m->chunk_z = fe_w.cz;
	m->count = (uint16_t)count;
	m->coordinate = malloc(count * sizeof(*m->coordinate));
	m->type = malloc(count);
	m->metadata = malloc(count);
	if(!m->coordinate || !m->type || !m->metadata)
		fe_setup_exit("Packet52 allocation");
	for(size_t i = 0; i < count; i++) {
		m->coordinate[i] = (uint16_t)((cells[i].lx << 12) | (cells[i].lz << 8)
									  | (cells[i].y & 0xFF));
		m->type[i] = cells[i].id;
		m->metadata[i] = cells[i].meta;
		fe_ref_edit(fe_w.ref, cells[i].lx, cells[i].y, cells[i].lz, cells[i].id,
					cells[i].meta);
	}
}

/* New random source content for the next body. */
static void fe_new_source(uint32_t* seed, enum fe_palette pal,
						  enum fe_light light) {
	fe_fill_source(seed, fe_w.src, pal, light);
}

/* Light-only source: the reference's current ids and metadata, fresh light. */
static void fe_light_only_source(uint32_t* seed) {
	memcpy(fe_w.src, fe_w.ref, FE_RAW);
	for(unsigned plane = 1; plane < 3; plane++) {
		uint8_t* dst = fe_w.src + FE_CELLS + plane * FE_NIB;
		for(size_t j = 0; j < FE_NIB; j++)
			dst[j] = (uint8_t)(fe_rng(seed) & 0xFFu);
	}
}

/* Packet53 PROCESSED BY THE PUMP right away (no older body or event left
 * waiting): one received packet, one edit. */
static void fe_edit53(int lx, int y, int lz, uint8_t id, uint8_t meta) {
	thread_mutex_lock(&s_r->lock);
	uint64_t seq = ++s_r->arrival_sequence;
	thread_mutex_unlock(&s_r->lock);
	s_r->pump_packet_seq = seq;
	s_r->pump_packet_sub = 0;
	remote_apply_block_change(s_r, fe_w.cx * 16 + lx, (uint8_t)y,
							  fe_w.cz * 16 + lz, id, meta);
	fe_ref_edit(fe_w.ref, lx, y, lz, id, meta);
}

/* Packet52 processed by the pump right away (sub-stamps from 0). */
static void fe_edit52(const struct fe_cell* cells, size_t count) {
	thread_mutex_lock(&s_r->lock);
	uint64_t seq = ++s_r->arrival_sequence;
	thread_mutex_unlock(&s_r->lock);
	s_r->pump_packet_seq = seq;
	s_r->pump_packet_sub = 0;
	for(size_t i = 0; i < count; i++) {
		remote_apply_block_change(s_r, fe_w.cx * 16 + cells[i].lx,
								  (uint8_t)cells[i].y, fe_w.cz * 16 + cells[i].lz,
								  cells[i].id, cells[i].meta);
		fe_ref_edit(fe_w.ref, cells[i].lx, cells[i].y, cells[i].lz, cells[i].id,
					cells[i].meta);
	}
}

/* ---- comparisons --------------------------------------------------------- */

static void fe_diff(const char* when) {
	size_t first = FE_RAW, diffs = 0;
	for(size_t i = 0; i < FE_RAW; i++)
		if(fe_w.out[i] != fe_w.ref[i]) {
			if(first == FE_RAW)
				first = i;
			diffs++;
		}
	if(!diffs)
		return;
	static const char* const planes[] = {"ids", "metadata", "block-light",
										 "sky-light"};
	unsigned plane = first < FE_CELLS ? 0u
		: (unsigned)(1u + (first - FE_CELLS) / FE_NIB);
	size_t cell = first < FE_CELLS ? first
		: ((first - FE_CELLS) % FE_NIB) * 2u;
	char detail[320];
	snprintf(detail, sizeof(detail),
			 "%s: %zu of %u decoded bytes differ from sequential CFR-region "
			 "application; first at byte %zu (%s, local x=%zu z=%zu y=%zu): "
			 "expected 0x%02X got 0x%02X",
			 when, diffs, FE_RAW, first, planes[plane], cell >> 11,
			 (cell >> 7) & 15u, cell & 127u, fe_w.ref[first], fe_w.out[first]);
	fe_fail("%s", detail);
}

static bool fe_export(void) {
	memset(fe_w.out, 0xCD, sizeof(fe_w.out));
	return server_remote_test_export_column(
		&fe_owner, fe_w.cx, fe_w.cz, fe_w.out, fe_w.out + FE_CELLS,
		fe_w.out + FE_CELLS + 2u * FE_NIB, fe_w.out + FE_CELLS + FE_NIB);
}

/* Export the column (resident voxels, or the backed head) and compare all
 * 81920 bytes with the reference. `when` names the phase. */
static void fe_compare(const char* when) {
	if(!fe_export()) {
		char detail[96];
		snprintf(detail, sizeof(detail), "%s: column does not export", when);
		fe_fail("%s", detail);
		return;
	}
	fe_diff(when);
}

/* BEFORE PROMOTION: the column is BACKED (retained, never installed). Its
 * decoded content is its head body followed by every queued body still in
 * its chain, in chain order -- whatever mix of folded head and chain
 * production holds. All 81920 bytes are compared against the reference. */
static void fe_compare_backed(const char* when) {
	struct mp_map_chunk queued[REMOTE_CHUNK_QUEUE_MAX];
	uint8_t* bytes[REMOTE_CHUNK_QUEUE_MAX] = {0};
	uint8_t count = 0;
	thread_mutex_lock(&s_r->lock);
	size_t i = fe_entry_locked(fe_w.cx, fe_w.cz);
	bool backed = i != REMOTE_CHUNK_ENTRY_CAPACITY
		&& s_r->chunks[i].state == REMOTE_CHUNK_RETAINED
		&& !s_r->chunks[i].installed;
	if(backed) {
		count = s_r->chunks[i].queued_count;
		for(uint8_t k = 0; k < count; k++) {
			queued[k] = s_r->chunks[i].queued[k];
			bytes[k] = malloc(queued[k].compressed_length);
			if(!bytes[k])
				fe_setup_exit("allocation");
			memcpy(bytes[k], queued[k].compressed, queued[k].compressed_length);
		}
	}
	thread_mutex_unlock(&s_r->lock);
	char detail[200];
	if(!backed) {
		snprintf(detail, sizeof(detail), "%s: column is not BACKED", when);
		fe_fail("%s", detail);
		return;
	}
	bool ok = fe_export();
	for(uint8_t k = 0; k < count && ok; k++) {
		uLongf n = (uLongf)queued[k].inflated_length;
		struct fe_region r = {(int)queued[k].x - fe_w.cx * 16, queued[k].y,
							  (int)queued[k].z - fe_w.cz * 16, queued[k].sx,
							  queued[k].sy, queued[k].sz};
		ok = n <= FE_RAW
			&& uncompress(fe_w.wire, &n, bytes[k],
						  (uLong)queued[k].compressed_length) == Z_OK
			&& n == queued[k].inflated_length;
		if(ok)
			fe_ref_apply(fe_w.out, &r, fe_w.wire);
	}
	for(uint8_t k = 0; k < count; k++)
		free(bytes[k]);
	if(!ok) {
		snprintf(detail, sizeof(detail),
				 "%s: the backed head and chain do not decode to a column", when);
		fe_fail("%s", detail);
		return;
	}
	fe_diff(when);
}

static void fe_end(void) {
	if(fe_w.races_verified != fe_w.races_required)
		fe_setup_exit("a race gate was declared but never judged");
	if(fe_w.control || !fe_w.red)
		printf("%s %s: %d bodies, %d refused, all %u bytes match\n",
			   fe_w.red ? "FAIL" : "PASS", fe_w.name, fe_w.bodies, fe_w.refused,
			   FE_RAW);
	if(fe_w.red)
		fe_red_witnesses++;
}

/* The consumer's half of a dimension change (process_event's RESPAWN case,
 * server_remote_protocol.c:1097-1185): the epoch is applied and the client
 * world is replaced. */
static void fe_consumer_applies_respawn(void) {
	thread_mutex_lock(&s_r->lock);
	s_r->applied_epoch = s_r->world_epoch;
	thread_mutex_unlock(&s_r->lock);
	remote_admission_centre_clear(s_r);
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 42);
}

/* ---- witnesses ----------------------------------------------------------- */

/* CONTROL: head + three non-covering partials -- the depth today's chain
 * holds. Validates the reference, the export and the light handling. */
static void w_control_three(void) {
	fe_begin("control_three_partials", 2, 0, true);
	uint32_t seed = 0xC0A1u;
	fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
	fe_body(&FE_FULL);
	const struct fe_region p[] = {
		{0, 0, 0, 16, 8, 16}, {3, 40, 5, 4, 6, 7}, {8, 100, 0, 8, 20, 16}};
	for(size_t i = 0; i < 3; i++) {
		fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
		fe_body(&p[i]);
	}
	fe_pump(FE_PUMPS);
	if(!fe_installed())
		fe_fail("%s", "column never installed");
	fe_compare("after promotion");
	fe_end();
}

/* CONTROL: edits around two partials, opaque ids, zero light; one edit
 * queued and drained by the pump. */
static void w_control_edits(void) {
	fe_begin("control_edits", 3, 0, true);
	uint32_t seed = 0xC0E2u;
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_body(&FE_FULL);
	fe_edit53(4, 10, 4, BLOCK_DIRT, 3);  /* inside the later partial: older */
	fe_edit53(9, 60, 9, BLOCK_STONE, 5); /* outside it */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {2, 8, 2, 6, 8, 6});
	fe_q_edit53(5, 12, 5, BLOCK_COBBLESTONE, 9); /* inside, newer, queued */
	fe_q_edit53(12, 90, 12, BLOCK_DIRT, 2);      /* outside, newer, queued */
	fe_flush();
	fe_pump(FE_PUMPS);
	fe_compare("after promotion");
	fe_end();
}

/* >3 non-covering partials on a retained column (the IN-007 shape: small
 * bounding boxes scattered through one column). */
static void w_many_partials(void) {
	fe_begin("seven_non_covering_partials", 4, 0, false);
	uint32_t seed = 0x5E7Eu;
	fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
	fe_body(&FE_FULL);
	const struct fe_region p[] = {
		{0, 0, 0, 16, 2, 16},  {8, 10, 0, 3, 4, 6},  {1, 20, 9, 5, 2, 7},
		{12, 30, 12, 4, 6, 4}, {0, 64, 0, 2, 10, 2}, {6, 90, 3, 9, 8, 11},
		{15, 120, 15, 1, 8, 1}};
	for(size_t i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
		fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
		fe_body(&p[i]);
	}
	fe_pump(FE_PUMPS);
	fe_compare("after promotion");
	fe_end();
}

/* Overlapping, nested and covering partials on every axis. */
static void w_overlap(void) {
	fe_begin("overlapping_partials", 5, 0, false);
	uint32_t seed = 0x0E7Au;
	fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
	fe_body(&FE_FULL);
	const struct fe_region p[] = {
		{0, 10, 0, 8, 20, 8},   /* A */
		{4, 20, 4, 8, 20, 8},   /* B overlaps A on x, y and z */
		{6, 24, 6, 2, 4, 2},    /* C nested inside B (and A) */
		{2, 0, 10, 12, 40, 4},  /* D overlaps A's y, disjoint z */
		{0, 10, 0, 8, 20, 8},   /* E equal to A: newer, covers A */
		{5, 22, 5, 4, 8, 4},    /* F overlaps B, C and E */
		{3, 4, 3, 10, 50, 10}}; /* G covers C and F, cuts A, B, D, E */
	for(size_t i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
		fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
		fe_body(&p[i]);
	}
	fe_pump(FE_PUMPS);
	fe_compare("after promotion");
	fe_end();
}

/* Partials that change ONLY the light planes (ids and metadata re-sent
 * unchanged): every one must still land. */
static void w_light_only(void) {
	fe_begin("light_only_partials", 6, 0, false);
	uint32_t seed = 0x11Eu;
	fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
	fe_body(&FE_FULL);
	const struct fe_region p[] = {
		{0, 0, 0, 16, 16, 16}, {2, 30, 2, 4, 4, 4}, {10, 60, 1, 6, 12, 3},
		{0, 100, 8, 16, 2, 8}, {7, 50, 7, 2, 70, 2}};
	for(size_t i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
		fe_light_only_source(&seed);
		fe_body(&p[i]);
	}
	fe_pump(FE_PUMPS);
	fe_compare("after promotion");
	fe_end();
}

/* Packet52/53 interleaved with >3 partials on a RETAINED column, each edit
 * processed by the pump as soon as it arrives: edits older than a later
 * region inside it lose to it, edits outside every later region survive,
 * edits newer than a region win inside it. */
static void w_edits_retained(void) {
	fe_begin("edits_retained", 7, 0, false);
	uint32_t seed = 0xED17u;
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_body(&FE_FULL);
	fe_edit53(4, 21, 4, BLOCK_DIRT, 1);  /* inside P3 (later): superseded */
	fe_edit53(13, 70, 13, BLOCK_STONE, 2); /* outside every later region */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_body(&(struct fe_region) {0, 2, 0, 8, 6, 8}); /* P1 */
	{
		const struct fe_cell c[] = {
			{9, 41, 9, BLOCK_COBBLESTONE, 4}, /* inside P4 (later) */
			{2, 90, 12, BLOCK_DIRT, 6},       /* outside every later region */
			{3, 4, 3, BLOCK_STONE, 7}};       /* inside P1 (earlier): wins */
		fe_edit52(c, sizeof(c) / sizeof(c[0]));
	}
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_body(&(struct fe_region) {10, 10, 0, 6, 4, 6}); /* P2 */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_body(&(struct fe_region) {2, 20, 2, 6, 4, 6}); /* P3 */
	fe_edit53(11, 11, 2, BLOCK_COBBLESTONE, 8); /* inside P2 (earlier): wins */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_body(&(struct fe_region) {8, 40, 8, 4, 4, 4}); /* P4 */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_body(&(struct fe_region) {4, 100, 4, 8, 6, 8}); /* P5 */
	{
		const struct fe_cell c[] = {
			{5, 102, 5, BLOCK_DIRT, 10},  /* inside P5, newer: wins */
			{9, 42, 9, BLOCK_STONE, 11}}; /* inside P4, newer: wins */
		fe_edit52(c, sizeof(c) / sizeof(c[0]));
	}
	fe_pump(FE_PUMPS);
	fe_compare("after promotion");
	fe_end();
}

/* The same on a column that is already RESIDENT when the partials arrive. */
static void w_edits_resident(void) {
	fe_begin("edits_resident", 8, 0, false);
	uint32_t seed = 0x2E5Du;
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_body(&FE_FULL);
	fe_pump(FE_PUMPS);
	if(!fe_installed())
		fe_fail("%s", "head never installed");
	const struct fe_region p[] = {
		{0, 4, 0, 4, 4, 4},   {6, 30, 6, 4, 6, 4}, {12, 50, 1, 3, 8, 3},
		{1, 70, 10, 5, 2, 5}, {8, 90, 8, 8, 10, 8}};
	for(size_t i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
		/* an edit inside the NEXT region (older than it), one outside */
		fe_edit53(p[i].x0 + 1, p[i].y0 + 1, p[i].z0 + 1, BLOCK_DIRT,
				  (uint8_t)(i + 1));
		fe_edit53(14, 110 + (int)i, 14, BLOCK_COBBLESTONE, (uint8_t)(i + 6));
		fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
		fe_body(&p[i]);
	}
	fe_edit53(9, 95, 9, BLOCK_STONE, 13); /* inside the last region, newer */
	fe_pump(FE_PUMPS);
	fe_compare("after promotion");
	fe_end();
}

/* PLAN STEP 5 ORDERING HAZARD. The pump is held: every Packet52/53 below is
 * still in the receive queue, unprocessed, when the column needs folding
 * (body 5 = head + 3 queued + 1). An edit OLDER than a later partial but
 * OUTSIDE it must survive the fold; one inside it must lose; one newer than
 * a partial must win inside it. A fold that stamps its merged snapshot with
 * the newest arrival would wrongly supersede the outside edits. */
static void w_queued_edits_fold_pressure(void) {
	fe_begin("queued_edits_under_fold_pressure", 10, 0, false);
	uint32_t seed = 0x0FE5u;
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_load();
	fe_q_body(&FE_FULL);
	fe_q_edit53(13, 70, 13, BLOCK_COBBLESTONE, 2); /* outside every later region */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {0, 2, 0, 8, 6, 8}); /* P1 */
	{
		const struct fe_cell c[] = {
			{4, 21, 4, BLOCK_DIRT, 1},   /* inside P3 (later): loses */
			{2, 90, 12, BLOCK_STONE, 3}}; /* outside every later region */
		fe_q_edit52(c, sizeof(c) / sizeof(c[0]));
	}
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {10, 10, 0, 6, 4, 6}); /* P2 */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {2, 20, 2, 6, 4, 6}); /* P3 */
	fe_q_edit53(3, 4, 3, BLOCK_STONE, 7); /* inside P1, newer: wins */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {8, 40, 8, 4, 4, 4}); /* P4: fold needed */
	fe_gate_last(FE_GATE_PRE, "the fold-pressure body P4", 3, false);
	fe_q_edit53(9, 41, 9, BLOCK_DIRT, 4); /* inside P4, newer: wins */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {4, 100, 4, 8, 6, 8}); /* P5 */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {12, 60, 12, 4, 8, 4}); /* P6 */
	fe_q_edit53(5, 102, 5, BLOCK_COBBLESTONE, 10); /* inside P5, newer: wins */
	fe_flush();
	fe_pump(FE_PUMPS);
	if(!fe_installed())
		fe_fail("%s", "column never installed");
	fe_compare("after promotion");
	fe_end();
}

/* HELD-PUMP BACKPRESSURE + PROMOTION RACE (plan steps 5 and 9). The column's
 * head is mid-decode (the promotion owns it and its inflate buffer) when six
 * more partials and two queued edits arrive with the pump held. Folding is
 * needed at the fourth partial while the head is not the fold's to touch:
 * production must wait (backpressure) or fold the rest, never refuse, and
 * the in-flight head must still install under every later region. */
static void w_promotion_race_backpressure(void) {
	fe_begin("promotion_race_backpressure", 11, 0, false);
	uint32_t seed = 0xBAC0u;
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_load();
	fe_q_body(&FE_FULL);
	fe_flush();
	fe_promote_until_in_flight();
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {0, 0, 0, 16, 2, 16}); /* P1 */
	fe_q_edit53(14, 100, 14, BLOCK_COBBLESTONE, 5); /* outside later regions */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {1, 20, 9, 5, 2, 7}); /* P2 */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {12, 30, 12, 4, 6, 4}); /* P3 */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {0, 64, 0, 2, 10, 2}); /* P4: fold pressure */
	fe_gate_last(FE_GATE_PRE, "the fold-pressure body P4", 1, true);
	{
		const struct fe_cell c[] = {
			{7, 92, 4, BLOCK_DIRT, 6},    /* inside P5 (later): loses */
			{15, 50, 0, BLOCK_STONE, 8}}; /* outside every later region */
		fe_q_edit52(c, sizeof(c) / sizeof(c[0]));
	}
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {6, 90, 3, 9, 8, 11}); /* P5 */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {15, 120, 15, 1, 8, 1}); /* P6 */
	fe_flush();
	fe_pump(FE_PUMPS);
	if(!fe_installed())
		fe_fail("%s", "column never installed");
	fe_compare("after promotion");
	fe_end();
}

/* UNLOAD DURING ACTIVE WORK (plan step 5 cancellation). Residency 1's head
 * is mid-decode with a chain behind it and an edit still queued when the
 * Packet50 unload arrives: the in-flight decode must be cancelled, nothing of
 * residency 1 (head, chain or queued edit) may install, and residency 2's
 * head plus five partials (folding needed, an older queued edit outside the
 * later regions) must land exactly. */
static void w_unload_during_promotion(void) {
	fe_begin("unload_during_promotion", 9, 0, false);
	uint32_t seed = 0x0B10u;
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_load();
	fe_q_body(&FE_FULL);
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {0, 0, 0, 4, 4, 4});
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {5, 20, 5, 4, 4, 4});
	fe_flush();
	fe_promote_until_in_flight();
	fe_q_edit53(14, 110, 14, BLOCK_COBBLESTONE, 3); /* residency 1, queued */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {10, 40, 10, 4, 4, 4});
	fe_q_unload(); /* mid-promotion */
	fe_gate_last(FE_GATE_POST, "the Packet50 unload", 1, true);
	fe_q_load();
	memset(fe_w.ref, 0, sizeof(fe_w.ref)); /* the full head below covers it */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&FE_FULL);
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {1, 2, 1, 6, 4, 6});
	fe_q_edit53(15, 100, 0, BLOCK_STONE, 9); /* outside every later region */
	const struct fe_region q[] = {
		{9, 26, 3, 5, 8, 5}, {2, 50, 11, 7, 2, 4}, {11, 74, 11, 5, 4, 5},
		{0, 110, 0, 16, 2, 16}};
	for(size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
		fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
		fe_q_body(&q[i]);
	}
	fe_flush();
	fe_pump(FE_PUMPS);
	if(!fe_installed())
		fe_fail("%s", "reloaded column never installed");
	fe_compare("after reload");
	fe_end();
}

/* The IN-007 column shape: OUTSIDE the admission radius, a full head plus
 * >3 non-covering partials, held BACKED. It must promote on entry with every
 * region applied, survive a demote (backed re-encode) byte-exact, and
 * re-promote byte-exact. */
static void w_far_column(void) {
	fe_begin("far_column_demote_round_trip", 105, 0, false);
	uint32_t seed = 0xFA2u;
	server_remote_set_admit_radius(&fe_owner, 2);
	remote_admission_centre_set(s_r, 100 * 16 + 8.0, 8.0, true);
	fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
	fe_body(&FE_FULL);
	const struct fe_region p[] = {
		{8, 10, 0, 3, 4, 6}, {0, 64, 0, 4, 2, 4},  {12, 70, 10, 2, 6, 2},
		{4, 12, 4, 2, 2, 2}, {9, 30, 9, 6, 6, 6}, {0, 126, 15, 16, 2, 1}};
	for(size_t i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
		fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
		fe_body(&p[i]);
	}
	fe_pump(FE_PUMPS);
	if(fe_installed())
		fe_fail("%s", "out-of-radius column installed (admission gate leaked)");
	fe_compare_backed("backed, before promotion");
	/* Walk to it: promote on entry. */
	remote_admission_centre_set(s_r, 105 * 16 + 8.0, 8.0, true);
	fe_pump(FE_PUMPS);
	if(!fe_installed())
		fe_fail("%s", "column never promoted on entry");
	fe_compare("after promotion on entry");
	/* Walk away: demote to a backed re-encode, export that. */
	remote_admission_centre_set(s_r, 100 * 16 + 8.0, 8.0, true);
	fe_pump(FE_PUMPS);
	thread_mutex_lock(&s_r->lock);
	size_t i = fe_entry_locked(fe_w.cx, fe_w.cz);
	bool backed = i != REMOTE_CHUNK_ENTRY_CAPACITY
		&& s_r->chunks[i].state == REMOTE_CHUNK_RETAINED;
	thread_mutex_unlock(&s_r->lock);
	if(!backed || fe_installed())
		fe_fail("%s", "column did not demote to BACKED");
	else
		fe_compare("backed after demotion");
	/* And back. */
	remote_admission_centre_set(s_r, 105 * 16 + 8.0, 8.0, true);
	fe_pump(FE_PUMPS);
	fe_compare("after re-promotion");
	remote_admission_centre_clear(s_r);
	server_remote_set_admit_radius(&fe_owner, SERVER_REMOTE_ADMIT_RADIUS_MAX);
	fe_end();
}

/* THE REAL COLUMN: (12,-9) of the IN-007 walk fixture (locked, read only),
 * its Packet50 load and all five Packet51 bodies exactly as Poseidon sent
 * them (head at 277603, partials at 307437, 312435, 313273, 336517), decoded
 * by the production codec and retained at the Wii radius 4 from the walk's
 * end position (80.27, -35.75) -- distance 7, so it stays BACKED. Compared
 * BEFORE promotion (backed head + chain) and after promotion on entry. The
 * column's Packet52/53s in the stream are not replayed here: their relight
 * is outside a region reference, and edit ordering has its own witnesses. */
static const char* fe_stream_path;

static void w_walk_column(void) {
	fe_begin("walk_column_12_-9", 12, -9, false);
	FILE* f = fopen(fe_stream_path, "rb");
	if(!f)
		fe_setup_exit("cannot open the walk stream fixture");
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* bytes = size > 0 ? malloc((size_t)size) : NULL;
	if(!bytes || fread(bytes, 1, (size_t)size, f) != (size_t)size)
		fe_setup_exit("cannot read the walk stream fixture");
	fclose(f);
	static const struct {
		int x, y, z, sx, sy, sz;
	} expected[] = {{192, 0, -144, 16, 128, 16}, {192, 24, -144, 5, 12, 8},
					{200, 8, -144, 7, 8, 14},	 {203, 4, -134, 4, 6, 6},
					{200, 10, -144, 3, 4, 6}};
	struct mp_decoder decoder;
	mp_decoder_init(&decoder);
	int found = 0;
	bool loaded = false;
	fe_q_load();
	for(long i = 0; i < size; i++) {
		struct mp_event event;
		enum mp_decode_result r = mp_decoder_feed(&decoder, bytes[i], &event);
		if(r == MP_DECODE_ERROR)
			fe_setup_exit("the walk stream does not decode");
		if(r != MP_DECODE_EVENT)
			continue;
		if(event.type == MP_EVENT_PRE_CHUNK
		   && event.payload.pre_chunk.chunk_x == 12
		   && event.payload.pre_chunk.chunk_z == -9
		   && event.payload.pre_chunk.mode)
			loaded = true;
		if(event.type != MP_EVENT_MAP_CHUNK
		   || (int32_t)floor(event.payload.map_chunk.x / 16.0) != 12
		   || (int32_t)floor(event.payload.map_chunk.z / 16.0) != -9) {
			mp_event_destroy(&event);
			continue;
		}
		const struct mp_map_chunk* w = &event.payload.map_chunk;
		if(!loaded || found == 5 || w->x != expected[found].x
		   || w->y != expected[found].y || w->z != expected[found].z
		   || w->sx != expected[found].sx || w->sy != expected[found].sy
		   || w->sz != expected[found].sz)
			fe_setup_exit("column (12,-9) bodies differ from the fixture README");
		struct fe_region region = {(int)w->x - 192, w->y, (int)w->z + 144,
								   w->sx, w->sy, w->sz};
		fe_check_region(&region);
		uLongf n = (uLongf)w->inflated_length;
		if(n > FE_RAW
		   || uncompress(fe_w.wire, &n, w->compressed,
						 (uLong)w->compressed_length) != Z_OK
		   || n != w->inflated_length)
			fe_setup_exit("a (12,-9) body does not inflate");
		fe_ref_apply(fe_w.ref, &region, fe_w.wire); /* every region, in order */
		struct fe_act* a = fe_new_act(FE_ACT_BODY);
		a->event = event; /* the act owns the decoder's body now */
		a->body_no = ++fe_w.bodies;
		found++;
	}
	mp_decoder_destroy(&decoder);
	free(bytes);
	if(found != 5)
		fe_setup_exit("column (12,-9) does not have five bodies in the fixture");
	server_remote_set_admit_radius(&fe_owner, 4);
	remote_admission_centre_set(s_r, 80.27, -35.75, true);
	fe_flush();
	fe_pump(FE_PUMPS);
	if(fe_installed())
		fe_fail("%s", "out-of-radius column installed (admission gate leaked)");
	fe_compare_backed("backed at distance 7, before promotion");
	remote_admission_centre_set(s_r, 12 * 16 + 8.0, -9 * 16 + 8.0, true);
	fe_pump(FE_PUMPS);
	if(!fe_installed())
		fe_fail("%s", "column never promoted on entry");
	fe_compare("after promotion on entry");
	remote_admission_centre_clear(s_r);
	server_remote_set_admit_radius(&fe_owner, SERVER_REMOTE_ADMIT_RADIUS_MAX);
	fe_end();
}

/* DIMENSION CHANGE DURING AN ACTIVE PROMOTION (plan step 5). The read
 * thread's Packet9 drops the table while the old world's head is mid-decode
 * and an old-world edit is still queued. Nothing of the old world may
 * install; the new world's same-coordinate head plus five partials (folding
 * needed, a queued edit outside the later regions) waits for the consumer's
 * epoch and then lands exactly. */
static void w_epoch_during_promotion(void) {
	fe_begin("epoch_change_during_promotion", 12, 0, false);
	uint32_t seed = 0xE90Cu;
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_load();
	fe_q_body(&FE_FULL);
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {0, 0, 0, 2, 2, 2});
	fe_flush();
	fe_promote_until_in_flight();
	fe_q_edit53(14, 110, 14, BLOCK_COBBLESTONE, 3); /* old world, queued */
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {4, 8, 4, 2, 2, 2});
	fe_q_respawn(); /* mid-promotion */
	fe_gate_last(FE_GATE_PRE, "the Packet9 dimension change", 1, true);
	fe_q_load();
	memset(fe_w.ref, 0, sizeof(fe_w.ref));
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&FE_FULL);
	fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
	fe_q_body(&(struct fe_region) {3, 6, 3, 5, 4, 5});
	fe_q_edit53(15, 120, 15, BLOCK_STONE, 6); /* new world, outside later */
	const struct fe_region q[] = {
		{10, 40, 2, 4, 10, 4}, {0, 80, 9, 8, 6, 7}, {14, 100, 14, 2, 10, 2},
		{6, 0, 6, 4, 128, 4}};
	for(size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
		fe_new_source(&seed, FE_PAL_OPAQUE, FE_LIGHT_ZERO);
		fe_q_body(&q[i]);
	}
	fe_flush();
	fe_pump(FE_PUMPS);
	if(fe_installed())
		fe_fail("%s", "a column installed before the new world's respawn "
					  "applied (stale install)");
	fe_consumer_applies_respawn();
	fe_pump(FE_PUMPS);
	if(!fe_installed())
		fe_fail("%s", "new-epoch column never installed");
	fe_compare("after epoch change");
	fe_end();
}

/* DIMENSION CHANGE DURING AN ACTIVE RE-ENCODE (plan step 5). The pump's
 * demotion has released impl->lock for its gather + deflate (the recode is
 * busy) when the read thread's Packet9 drops the table: the re-encode must
 * abort and install nothing. The new world's same-coordinate head plus five
 * partials are then compared BEFORE promotion (backed) and after. */
static bool fe_probe_armed;
static unsigned fe_probe_fired;
static void fe_recode_probe(struct server_remote_impl* impl) {
	(void)impl;
	if(!fe_probe_armed)
		return;
	fe_probe_armed = false;
	fe_probe_fired++;
	fe_q_respawn();
	fe_flush_ex(false); /* this IS the pump: it must not pump itself */
}

static void w_epoch_during_recode(void) {
	fe_begin("epoch_change_during_recode", 40, 0, false);
	uint32_t seed = 0x2EC0u;
	server_remote_set_admit_radius(&fe_owner, 2);
	remote_admission_centre_set(s_r, 40 * 16 + 8.0, 8.0, true);
	fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
	fe_q_load();
	fe_q_body(&FE_FULL);
	fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
	fe_q_body(&(struct fe_region) {2, 30, 2, 6, 4, 6});
	fe_flush();
	fe_pump(FE_PUMPS);
	if(!fe_installed())
		fe_setup_exit("the old-world column never installed");
	uint32_t aborts = s_r->recode_aborts;
	fe_probe_fired = 0;
	fe_probe_armed = true;
	server_remote_test_set_deflate_probe(fe_recode_probe);
	remote_admission_centre_set(s_r, 80 * 16 + 8.0, 8.0, true); /* walk away */
	fe_pump(8);
	server_remote_test_set_deflate_probe(NULL);
	fe_probe_armed = false;
	if(fe_probe_fired != 1 || s_r->recode_aborts != aborts + 1)
		fe_setup_exit("the dimension change did not land inside an active "
					  "re-encode");
	memset(fe_w.ref, 0, sizeof(fe_w.ref));
	fe_q_load();
	fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
	fe_q_body(&FE_FULL);
	const struct fe_region q[] = {
		{0, 0, 0, 3, 4, 3},    {5, 20, 5, 6, 2, 6}, {12, 44, 0, 4, 8, 16},
		{0, 70, 12, 16, 6, 4}, {7, 100, 7, 2, 28, 2}};
	for(size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
		fe_new_source(&seed, FE_PAL_ALL, FE_LIGHT_RANDOM);
		fe_q_body(&q[i]);
	}
	fe_flush();
	fe_compare_backed("new world, backed, before promotion");
	fe_consumer_applies_respawn();
	remote_admission_centre_set(s_r, 40 * 16 + 8.0, 8.0, true);
	fe_pump(FE_PUMPS);
	if(!fe_installed())
		fe_fail("%s", "new-epoch column never installed");
	fe_compare("after promotion");
	remote_admission_centre_clear(s_r);
	server_remote_set_admit_radius(&fe_owner, SERVER_REMOTE_ADMIT_RADIUS_MAX);
	fe_end();
}

/* ---- IN-009d: fold + parked-edit RELIGHT witnesses ----------------------
 *
 * The witnesses above keep light out of the picture (opaque ids, zero light,
 * relight never drained). These put it back: parked Packet52/53 edits PLACE
 * light emitters (torch 50) and REMOVE light blockers (stone -> air) within
 * light reach of LATER Packet51 regions, the regions carry a light emitter
 * (glowstone 89), a transparent block (glass 20) and opaque changes, and the
 * column's light is then RELIT by the client (world_update_lighting run to
 * quiescence) and compared.
 *
 * Three isolated columns (no loaded neighbour within one column, so no flood
 * crosses between them), one wire schedule:
 *   S (72,30)  the SERVER stand-in. Every edit and every region's id change
 *              is applied with world_set_block + relight drained, and each
 *              Packet51 body is encoded from S's relit state at that moment
 *              -- so a region re-sends light that already contains the torch
 *              / opened-wall light, as Beta's server (PlayerInstance.java:
 *              133-153 sends the region from its own lit chunk) would.
 *   Q (70,30)  the SEQUENTIAL reference: resident, every body pumped onto the
 *              installed voxels before the next packet, every edit applied
 *              live through remote_apply_block_change's CRPC_SET_BLOCK, in
 *              wire order -- the relight order before the fold existed
 *              (edits push their relight in arrival order; the deferred
 *              driver relights after the bodies landed).
 *   F (90,30)  the FOLDED column: out of the admission radius, so it stays
 *              BACKED while a full head and five partials arrive (more than
 *              the three-body chain: folding is required) and the edits are
 *              PARKED (pump-processed against a non-resident column) or
 *              QUEUED (held pump, one reader burst). It then promotes on
 *              entry and its parked edits drain through CRPC_SET_BLOCK.
 * Both Q and F relight to quiescence before export. All 81920 decoded bytes
 * of F must equal Q's -- ids, metadata, block light and sky light -- and F's
 * ids/metadata must equal the independent CFR-region reference. A difference
 * prints "IN-009d RED: <witness>: ... first at byte N (<plane>, local x z y)"
 * and the test exits 1. */

static int fr_cz = 30; /* per witness: columns of different witnesses never touch */
#define FR_S_CX 72
#define FR_Q_CX 70
#define FR_F_CX 90
#define FR_CENTRE_CX 71
#define FR_LIGHT_FRAMES 200000

enum fr_kind { FR_E53, FR_E52, FR_BODY };

struct fr_step {
	enum fr_kind kind;
	struct fe_region region;   /* FR_BODY */
	struct fe_cell cells[3];   /* FR_E53: cells[0]; FR_E52: count cells;
								  FR_BODY: the server's id changes inside the
								  region before it is sent */
	size_t count;
};

/* Wire schedule. Cave: x 2..13, z 2..13, y 20..40 air, split by a stone
 * wall at x = 8; stone below/around, open sky from y 64. */
static const struct fr_step fr_script[] = {
	/* torch on the west cave floor; P1 (later) starts two cells above it */
	{FR_E53, {0}, {{5, 20, 7, BLOCK_TORCH, 5}}, 1},
	{FR_BODY, {2, 22, 2, 4, 6, 6}, {{2, 26, 2, BLOCK_COBBLESTONE, 0}}, 1},
	/* remove a wall block: west torch light reaches P2's cells (x 9..) */
	{FR_E53, {0}, {{8, 21, 7, BLOCK_AIR, 0}}, 1},
	{FR_BODY, {9, 20, 5, 4, 4, 4}, {{12, 22, 7, BLOCK_GLOWSTONE, 0}}, 1},
	/* Packet52: a blocker beside the torch, a torch outside every later
	 * region, and a torch INSIDE P3 (older than it: P3 carries it) */
	{FR_E52, {0},
	 {{6, 23, 9, BLOCK_STONE, 0}, {11, 30, 11, BLOCK_TORCH, 5},
	  {11, 25, 9, BLOCK_TORCH, 5}},
	 3},
	{FR_BODY, {10, 24, 8, 4, 6, 4}, {{13, 28, 11, BLOCK_DIRT, 0}}, 1},
	{FR_BODY, {3, 30, 3, 4, 4, 4}, {{4, 31, 4, BLOCK_COBBLESTONE, 0}}, 1},
	/* a second wall hole, adjacent to P5 (later) */
	{FR_E53, {0}, {{8, 35, 10, BLOCK_AIR, 0}}, 1},
	{FR_BODY, {9, 34, 8, 2, 4, 4}, {{10, 36, 9, BLOCK_GLASS, 0}}, 1},
	/* newer than P5, inside it: wins */
	{FR_E53, {0}, {{9, 35, 9, BLOCK_TORCH, 5}}, 1},
};
#define FR_STEPS (sizeof(fr_script) / sizeof(fr_script[0]))

static uint8_t fr_snap[FR_STEPS][FE_RAW]; /* S's state at each body */
static uint8_t fr_q_out[FE_RAW];

static void fr_head(uint8_t* src) {
	memset(src, 0, FE_RAW);
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = 0; y < 128; y++) {
				bool cave = x >= 2 && x <= 13 && z >= 2 && z <= 13 && y >= 20
					&& y <= 40 && x != 8;
				size_t i = fe_index(x, y, z);
				src[i] = (y >= 64 || cave) ? BLOCK_AIR : BLOCK_STONE;
				if(y >= 64) { /* sky light 15 above the surface */
					uint8_t* sky = src + FE_CELLS + 2u * FE_NIB + (i >> 1);
					*sky |= (uint8_t)((i & 1u) ? 0xF0u : 0x0Fu);
				}
			}
}

static void fr_drain_light(void) {
	struct world* w = &gstate.world;
	for(int i = 0; i < FR_LIGHT_FRAMES; i++) {
		if(stack_empty(&w->lighting_updates)
		   && lighting_flood_queue_empty(&w->lighting_flood_queue))
			return;
		world_update_lighting(w);
	}
	fe_setup_exit("the client relight never settled");
}

static void fr_set_cx(int cx) {
	fe_w.cx = cx;
	fe_w.cz = fr_cz;
}

static bool fr_resident_settled(int cx) {
	thread_mutex_lock(&s_r->lock);
	size_t i = fe_entry_locked(cx, fr_cz);
	bool ok = i != REMOTE_CHUNK_ENTRY_CAPACITY
		&& s_r->chunks[i].state == REMOTE_CHUNK_RESIDENT
		&& s_r->chunks[i].queued_count == 0;
	thread_mutex_unlock(&s_r->lock);
	return ok;
}

/* Install a full head on a resident (in-radius) column. */
static void fr_install_head(int cx, const uint8_t* head) {
	fr_set_cx(cx);
	memcpy(fe_w.src, head, FE_RAW);
	fe_q_load();
	fe_q_body(&FE_FULL);
	fe_flush();
	fe_pump(FE_PUMPS);
	if(!fe_installed() || !fr_resident_settled(cx))
		fe_setup_exit("a resident column's head never installed");
}

static void fr_apply_edit(const struct fr_step* st, bool queued) {
	if(st->kind == FR_E53) {
		const struct fe_cell* c = &st->cells[0];
		if(queued)
			fe_q_edit53(c->lx, c->y, c->lz, c->id, c->meta);
		else
			fe_edit53(c->lx, c->y, c->lz, c->id, c->meta);
	} else if(queued)
		fe_q_edit52(st->cells, st->count);
	else
		fe_edit52(st->cells, st->count);
}

/* Phase S: the server stand-in builds every body's source. */
static void fr_server(const uint8_t* head) {
	fr_install_head(FR_S_CX, head);
	struct world* w = &gstate.world;
	for(size_t s = 0; s < FR_STEPS; s++) {
		const struct fr_step* st = &fr_script[s];
		for(size_t k = 0; k < st->count; k++) {
			const struct fe_cell* c = &st->cells[k];
			world_set_block(w, FR_S_CX * 16 + c->lx, c->y, fr_cz * 16 + c->lz,
							(struct block_data) {.type = c->id,
												 .metadata = c->meta & 0x0Fu},
							true);
		}
		fr_drain_light();
		if(st->kind != FR_BODY)
			continue;
		memset(fr_snap[s], 0xCD, FE_RAW);
		if(!server_remote_test_export_column(
			   &fe_owner, FR_S_CX, fr_cz, fr_snap[s], fr_snap[s] + FE_CELLS,
			   fr_snap[s] + FE_CELLS + 2u * FE_NIB,
			   fr_snap[s] + FE_CELLS + FE_NIB))
			fe_setup_exit("the server stand-in column does not export");
	}
}

static void fr_diff(const char* when, const uint8_t* want, const uint8_t* got,
					size_t bytes, const char* against) {
	size_t first = bytes, diffs = 0;
	for(size_t i = 0; i < bytes; i++)
		if(want[i] != got[i]) {
			if(first == bytes)
				first = i;
			diffs++;
		}
	if(!diffs)
		return;
	static const char* const planes[] = {"ids", "metadata", "block-light",
										 "sky-light"};
	unsigned plane = first < FE_CELLS ? 0u
		: (unsigned)(1u + (first - FE_CELLS) / FE_NIB);
	size_t cell = first < FE_CELLS ? first
		: ((first - FE_CELLS) % FE_NIB) * 2u;
	printf("IN-009d RED: %s: %s: %zu of %zu bytes differ from %s; first at "
		   "byte %zu (%s, local x=%zu z=%zu y=%zu%s): expected 0x%02X got "
		   "0x%02X\n",
		   fe_w.name, when, diffs, bytes, against, first, planes[plane],
		   cell >> 11, (cell >> 7) & 15u, cell & 127u,
		   plane ? ", nibble pair y/y+1" : "", want[first], got[first]);
	fe_w.red = true;
}

static void w_relight(const char* name, int cz, bool queued,
					  uint32_t seed_meta) {
	fr_cz = cz;
	fe_begin(name, FR_F_CX, fr_cz, false);
	static uint8_t head[FE_RAW];
	fr_head(head);
	/* seeded metadata in the head (light-neutral; must survive exactly) */
	for(size_t j = 0; j < FE_NIB; j++)
		head[FE_CELLS + j] = (uint8_t)(fe_rng(&seed_meta) & 0xFFu);
	fr_drain_light(); /* earlier witnesses' pending relights: not ours */
	server_remote_set_admit_radius(&fe_owner, 2);
	remote_admission_centre_set(s_r, FR_CENTRE_CX * 16 + 8.0,
								fr_cz * 16 + 8.0, true);

	/* S: the server. */
	fr_server(head);

	/* Q: sequential reference, resident, strictly one packet at a time. */
	fr_install_head(FR_Q_CX, head);
	for(size_t s = 0; s < FR_STEPS; s++) {
		const struct fr_step* st = &fr_script[s];
		if(st->kind != FR_BODY) {
			fr_apply_edit(st, false);
			continue;
		}
		memcpy(fe_w.src, fr_snap[s], FE_RAW);
		fe_body(&st->region);
		fe_pump(FE_PUMPS);
		if(!fr_resident_settled(FR_Q_CX))
			fe_setup_exit("the sequential reference's body never landed");
	}
	fr_drain_light();
	memset(fr_q_out, 0xCD, sizeof(fr_q_out));
	if(!server_remote_test_export_column(
		   &fe_owner, FR_Q_CX, fr_cz, fr_q_out, fr_q_out + FE_CELLS,
		   fr_q_out + FE_CELLS + 2u * FE_NIB, fr_q_out + FE_CELLS + FE_NIB))
		fe_setup_exit("the sequential reference does not export");

	/* F: backed, then folded; the independent id/metadata reference starts
	 * over (the full head covers every cell). */
	fr_set_cx(FR_F_CX);
	memset(fe_w.ref, 0, sizeof(fe_w.ref));
	fe_w.bodies = 0;
	memcpy(fe_w.src, head, FE_RAW);
	fe_q_load();
	fe_q_body(&FE_FULL);
	if(!queued)
		fe_flush();
	for(size_t s = 0; s < FR_STEPS; s++) {
		const struct fr_step* st = &fr_script[s];
		if(st->kind != FR_BODY) {
			fr_apply_edit(st, queued);
			continue;
		}
		memcpy(fe_w.src, fr_snap[s], FE_RAW);
		fe_q_body(&st->region);
		if(!queued)
			fe_flush();
	}
	if(queued)
		fe_flush();
	fe_pump(FE_PUMPS);
	if(fe_installed())
		fe_fail("%s", "out-of-radius column installed (admission gate leaked)");
	remote_admission_centre_set(s_r, FR_F_CX * 16 + 8.0, fr_cz * 16 + 8.0,
								true);
	fe_pump(FE_PUMPS);
	if(!fe_installed())
		fe_fail("%s", "column never promoted on entry");
	fr_drain_light();
	if(fe_export()) {
		fr_diff("after promotion and relight", fr_q_out, fe_w.out, FE_RAW,
				"the sequential reference (all four planes)");
		fr_diff("after promotion and relight", fe_w.ref, fe_w.out,
				FE_CELLS + FE_NIB,
				"the CFR-region reference (ids and metadata)");
	} else
		fe_fail("%s", "after promotion: column does not export");
	/* The reference itself must have seen the edits' light. */
	fr_diff("sequential reference vs CFR-region reference", fe_w.ref, fr_q_out,
			FE_CELLS + FE_NIB, "the CFR-region reference (ids and metadata)");
	{
		/* Non-vacuity: the torch cell, a P1 cell above it, and a P2 cell
		 * reached only through the removed wall block all carry block
		 * light, so the edits' light really reaches the later regions. */
		static const int lit[][3] = {{5, 20, 7}, {5, 22, 7}, {9, 21, 7}};
		for(size_t k = 0; k < sizeof(lit) / sizeof(lit[0]); k++) {
			size_t i = fe_index(lit[k][0], lit[k][1], lit[k][2]);
			uint8_t b = fr_q_out[FE_CELLS + FE_NIB + (i >> 1)];
			if(((i & 1u) ? (b >> 4) : (b & 0x0Fu)) == 0)
				fe_setup_exit("the reference relight left a cell the edits "
							  "must light dark (witness is vacuous)");
		}
	}
	remote_admission_centre_clear(s_r);
	server_remote_set_admit_radius(&fe_owner, SERVER_REMOTE_ADMIT_RADIUS_MAX);
	fe_end();
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if(argc < 2) {
		printf("IN-009b SETUP: usage: test_mp_fold_equiv <stream.bin>\n");
		return 2;
	}
	fe_stream_path = argv[1];
	atomic_init(&fe_all_done, false);
	atomic_init(&fe_reader_finished, false);
	atomic_init(&fe_gate_passed, false);
	struct thread watchdog;
	if(!thread_create(&watchdog, fe_watchdog_entry, NULL, 64)) {
		printf("IN-009b SETUP: watchdog thread create failed\n");
		return 2;
	}
	assert(svin_init() && clin_init());
	blocks_init();
	items_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	assert(server_remote_init(&fe_owner));
	s_r = fe_owner.impl;
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 42);

	w_control_three();
	w_control_edits();
	if(fe_setup_faults) {
		printf("IN-009b SETUP: %d control witness fault(s): the reference or "
			   "harness disagrees with today's working depth\n",
			   fe_setup_faults);
		fflush(stdout);
		_Exit(2);
	}
	w_many_partials();
	w_overlap();
	w_light_only();
	w_edits_retained();
	w_edits_resident();
	w_queued_edits_fold_pressure();
	w_promotion_race_backpressure();
	w_unload_during_promotion();
	w_far_column();
	w_walk_column();
	w_relight("relight_parked_edits_next_to_later_regions", 30, false,
			  0xD1A7u);
	w_relight("relight_queued_edits_held_pump", 40, true, 0xD2B8u);
	w_epoch_during_promotion(); /* clears the table + world: keep these last */
	w_epoch_during_recode();

	fe_current = "teardown";
	server_remote_destroy(&fe_owner);
	atomic_store(&fe_all_done, true);
	thread_join(&watchdog);
	int witnesses = fe_witnesses_run - 2; /* the two controls are not counted */
	if(fe_red_witnesses) {
		printf("IN-009b RED: %d of %d fold witnesses fail: retained columns do "
			   "not fold every Packet51 body losslessly\n",
			   fe_red_witnesses, witnesses);
		return 1;
	}
	printf("PASS test_mp_fold_equiv: all %d fold witnesses byte-exact against "
		   "sequential CFR-region application\n",
		   witnesses);
	return 0;
}

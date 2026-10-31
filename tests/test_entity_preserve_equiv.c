/* lava-aabb-0924/H2: chunk_entities_preserve owner-index EQUIVALENCE proof.
 *
 * H1 measured the preserving merge at O(n_old * n_generated) once one old
 * record defeats the codec-complete fast path (3.7 s for 4096 records on PC).
 * H2 replaces the per-record linear owner scan with a sorted key index
 * (entity_archive.c, "O(n log n) owner search"). This test runs the SAME merge
 * body twice on every input -- chunk_entities_preserve_reference (the pre-H2
 * linear entity_same_saved_owner scan, kept verbatim as the oracle) and the
 * production chunk_entities_preserve -- and demands byte-identical output
 * (nbt_dump_binary of the merged list) plus identical NULL-ness and identical
 * fail sub-codes (enum chunk_entities_preserve_fail), on:
 *   - randomized inputs: items + mobs, legacy no-Air, departed, foreign
 *     (unknown id, no CaveWii* tags), exact duplicates, records with a zero or
 *     missing runtime id, coincident positions, missing / malformed Pos on
 *     BOTH sides, generated records with their runtime id stripped, current
 *     == NULL, native on/off, sizes 0..512 with a few at 2048;
 *   - hand-built adversarial cases (every record the same Pos, every runtime
 *     id zero, no Pos anywhere, duplicated owners, current == NULL);
 *   - CHUNK_ENTITY_ARCHIVE_WRITE_MAX records through the production merge
 *     only (the oracle is quadratic there: hours), timed.
 * The two paths share one body with one swapped search, so what this proves is
 * exactly the search: same owner count (0 / 1 / >= 2) and the same first
 * owner index for every old record. */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cNBT/list.h"
#include "cNBT/nbt.h"
#include "entity/monsters_object.h"
#include "network/entity_archive.h"
#include "platform/time.h"

/* ---- deterministic rng ---- */
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
	s_rng = s_rng * 6364136223846793005ull + 1442695040888963407ull;
	return (uint32_t)(s_rng >> 33);
}
static uint32_t rnd_below(uint32_t n) {
	return n ? rnd() % n : 0;
}
static bool chance(uint32_t percent) {
	return rnd_below(100) < percent;
}

/* ---- entity fixtures ---- */
static void fill_entity(struct archived_entity* e, uint32_t i, bool mob) {
	memset(e, 0, sizeof(*e));
	e->source_id = i + 1;
	/* UNIQUE positions by index (up to 3 * 30720 records), so the Pos-identity
	 * fallback (records without a nonzero runtime id) never collides in the
	 * base population; the aggressive tier creates collisions on purpose. */
	e->pos[0] = (float)(i % 16) + 0.5f + (float)(i / 30720) * 0.125f;
	e->pos[1] = 1.0f + (float)((i / 256) % 120);
	e->pos[2] = (float)((i / 16) % 16) + 0.5f;
	e->fire = -1;
	e->air = 300;
	if(mob) {
		static const int ids[] = {MONSTER_PIG, MONSTER_COW, MONSTER_SHEEP,
								  MONSTER_CHICKEN, MONSTER_SPIDER};
		e->kind = ARCHIVED_ENTITY_MONSTER;
		e->health = 10;
		e->data.monster = (struct archived_monster_state) {
			.monster_id = ids[rnd_below(5)], .fire_time = -1, .air = 300,
			.fuse = -1,
		};
	} else {
		e->kind = ARCHIVED_ENTITY_ITEM;
		e->health = 5;
		e->data.item.item.id = (uint16_t)(1 + rnd_below(8));
		e->data.item.item.count = (uint8_t)(1 + rnd_below(64));
		e->data.item.age = (int32_t)rnd_below(6000);
	}
}

/* Heap-owned "Entities" TAG_LIST node built from a snapshot (the builder's
 * arena is released; the clone is free to mutate and nbt_free). */
static nbt_node* build_list(const struct chunk_entities* ents) {
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(ents, &arena);
	assert(list || ents->count == 0);
	nbt_node wrapper = {.type = TAG_LIST, .name = "Entities",
						.payload.tag_list = list};
	nbt_node* out;
	if(list)
		out = nbt_clone(&wrapper);
	else {
		/* An empty snapshot: build an empty TAG_LIST by cloning a built empty
		 * list is not possible, so assemble one of TAG_COMPOUND elements. */
		struct chunk_entities one = {0};
		struct archived_entity e;
		fill_entity(&e, 0, false);
		one.entities = &e;
		one.count = 1;
		void* a2 = NULL;
		struct nbt_list* l2 = chunk_entities_build(&one, &a2);
		assert(l2);
		wrapper.payload.tag_list = l2;
		out = nbt_clone(&wrapper);
		chunk_entities_build_free(a2);
		assert(out);
		struct list_head* first = out->payload.tag_list->entry.flink;
		struct nbt_list* w = list_entry(first, struct nbt_list, entry);
		list_del(first);
		nbt_free(w->data);
		free(w);
	}
	chunk_entities_build_free(arena);
	assert(out && out->type == TAG_LIST);
	return out;
}

static nbt_node* child(nbt_node* comp, const char* name) {
	assert(comp && comp->type == TAG_COMPOUND);
	struct list_head* pos;
	list_for_each(pos, &comp->payload.tag_compound->entry) {
		nbt_node* c = list_entry(pos, struct nbt_list, entry)->data;
		if(c && c->name && !strcmp(c->name, name))
			return c;
	}
	return NULL;
}
static void remove_child(nbt_node* comp, const char* name) {
	struct list_head *p, *next;
	list_for_each_safe(p, next, &comp->payload.tag_compound->entry) {
		struct nbt_list* w = list_entry(p, struct nbt_list, entry);
		if(w->data && w->data->name && !strcmp(w->data->name, name)) {
			list_del(p);
			nbt_free(w->data);
			free(w);
			return;
		}
	}
}
static void remove_cavewii_tags(nbt_node* comp) {
	struct list_head *p, *next;
	list_for_each_safe(p, next, &comp->payload.tag_compound->entry) {
		struct nbt_list* w = list_entry(p, struct nbt_list, entry);
		if(w->data && w->data->name && !strncmp(w->data->name, "CaveWii", 7)) {
			list_del(p);
			nbt_free(w->data);
			free(w);
		}
	}
}
static void set_id_string(nbt_node* comp, const char* id) {
	nbt_node* n = child(comp, "id");
	assert(n && n->type == TAG_STRING);
	free(n->payload.tag_string);
	n->payload.tag_string = strdup(id);
	assert(n->payload.tag_string);
}
static nbt_node* record_at(nbt_node* list, size_t index) {
	size_t k = 0;
	struct list_head* pos;
	list_for_each(pos, &list->payload.tag_list->entry)
		if(k++ == index)
			return list_entry(pos, struct nbt_list, entry)->data;
	return NULL;
}
static size_t list_len(const nbt_node* list) {
	size_t n = 0;
	struct list_head* pos;
	list_for_each(pos, &list->payload.tag_list->entry)
		n++;
	return n;
}
static void append_record(nbt_node* list, nbt_node* rec) {
	struct nbt_list* w = malloc(sizeof(*w));
	assert(w);
	w->data = rec;
	list_add_tail(&w->entry, &list->payload.tag_list->entry);
}
/* Copy Pos (and, with the flag, the runtime id) from another record. */
static void copy_pos_from(nbt_node* dst, nbt_node* src, bool rid_too) {
	if(dst == src)
		return;
	nbt_node* p = child(src, "Pos");
	if(p) {
		nbt_node* c = nbt_clone(p); /* clone BEFORE removing: src may alias */
		remove_child(dst, "Pos");
		append_record(dst, c); /* a compound is a list of children */
	}
	if(rid_too) {
		nbt_node* r = child(src, "CaveWiiRuntimeId");
		if(r) {
			nbt_node* c = nbt_clone(r);
			remove_child(dst, "CaveWiiRuntimeId");
			append_record(dst, c);
		}
	}
}
/* The codec writes CaveWiiRuntimeId as TAG_LONG; legacy archives carried
 * TAG_INT. Both are honoured by the owner match since H3, so set whichever
 * the record has -- and sometimes flip the type to the legacy one. */
static void set_rid(nbt_node* rec, int32_t value) {
	nbt_node* r = child(rec, "CaveWiiRuntimeId");
	if(!r)
		return;
	if(chance(20)) {
		r->type = TAG_INT;
		/* H5: the legacy -1 orphan sentinel (UINT32_MAX after read_u32) is
		 * no identity on either side of the equivalence. */
		r->payload.tag_int = chance(25) ? -1 : value;
	} else {
		r->type = TAG_LONG;
		r->payload.tag_long = value;
	}
}
/* Malform Pos: drop it, or drop one element (a 2-double list). */
static void corrupt_pos(nbt_node* rec) {
	nbt_node* p = child(rec, "Pos");
	if(!p)
		return;
	if(chance(50)) {
		remove_child(rec, "Pos");
		return;
	}
	assert(p->type == TAG_LIST);
	struct list_head* last = p->payload.tag_list->entry.blink;
	if(last != &p->payload.tag_list->entry) {
		struct nbt_list* w = list_entry(last, struct nbt_list, entry);
		list_del(last);
		nbt_free(w->data);
		free(w);
	}
}

/* Randomly mutate an OLD list in place (the record count may change). */
static void mutate_old(nbt_node* list, uint32_t aggression) {
	size_t n = list_len(list);
	struct list_head *p, *next;
	size_t k = 0;
	list_for_each_safe(p, next, &list->payload.tag_list->entry) {
		struct nbt_list* w = list_entry(p, struct nbt_list, entry);
		nbt_node* rec = w->data;
		k++;
		if(chance(aggression / 4)) { /* departed */
			list_del(p);
			nbt_free(rec);
			free(w);
			continue;
		}
		if(chance(aggression / 3))
			remove_child(rec, "Air"); /* legacy no-Air */
		if(chance(aggression / 6))
			set_rid(rec, 0); /* runtime id zero: position identity */
		if(chance(aggression / 8))
			remove_child(rec, "CaveWiiRuntimeId");
		if(chance(aggression / 8) && list_len(list) > 1)
			copy_pos_from(rec, record_at(list, rnd_below((uint32_t)list_len(list))),
						  chance(50)); /* live length: departures above shrink it */
		if(chance(aggression / 10))
			corrupt_pos(rec);
		if(chance(aggression / 10))
			set_rid(rec, (int32_t)(1 + rnd_below((uint32_t)n + 4)));
	}
	/* additions: foreign, duplicates, near-duplicates */
	size_t adds = rnd_below(1 + aggression / 10);
	for(size_t a = 0; a < adds && list_len(list) > 0; a++) {
		nbt_node* src = record_at(list, rnd_below((uint32_t)list_len(list)));
		nbt_node* c = nbt_clone(src);
		assert(c);
		switch(rnd_below(4)) {
		case 0: /* foreign: unknown id, no CaveWii* tags */
			remove_cavewii_tags(c);
			set_id_string(c, chance(50) ? "EquivForeign" : "Zombie");
			break;
		case 1: /* exact duplicate */
			break;
		case 2: /* same position, different runtime id */
			set_rid(c, (int32_t)(1000 + rnd_below(2000)));
			break;
		default: /* same runtime id, moved */
		{
			nbt_node* pos = child(c, "Pos");
			if(pos) {
				struct list_head* f = pos->payload.tag_list->entry.flink;
				nbt_node* x = list_entry(f, struct nbt_list, entry)->data;
				if(x->type == TAG_DOUBLE)
					x->payload.tag_double += 0.25;
			}
		} break;
		}
		if(chance(50))
			append_record(list, c);
		else {
			struct nbt_list* w = malloc(sizeof(*w));
			assert(w);
			w->data = c;
			/* prepend: insert before the first element */
			list_add_tail(&w->entry, list->payload.tag_list->entry.flink);
		}
	}
}
/* Mutate a GENERATED list in place without changing its record count (it
 * stays index-aligned with `current`). */
static void mutate_generated(nbt_node* list, uint32_t aggression) {
	size_t n = list_len(list);
	struct list_head* p;
	list_for_each(p, &list->payload.tag_list->entry) {
		nbt_node* rec = list_entry(p, struct nbt_list, entry)->data;
		if(chance(aggression / 8))
			set_rid(rec, 0);
		if(chance(aggression / 10))
			remove_child(rec, "CaveWiiRuntimeId");
		if(chance(aggression / 8) && n > 1)
			copy_pos_from(rec, record_at(list, rnd_below((uint32_t)n)), false);
		if(chance(aggression / 12))
			corrupt_pos(rec);
	}
}

static struct {
	size_t cases, null_both, ok_both;
	float ref_s, fast_s;
} s_stats;

/* THE CHECK: both paths on one input. Returns the production result's
 * dump length (0 when NULL) for callers that want to assert shape. */
static size_t check_equiv(const nbt_node* old, const nbt_node* generated,
						  const struct chunk_entities* current, bool native,
						  const char* label) {
	chunk_entities_preserve_test_reset_fail();
	ptime_t t0 = time_get();
	nbt_node* ref = chunk_entities_preserve_reference(old, generated, current,
													  native, 0, 0);
	s_stats.ref_s += time_diff_s(t0, time_get());
	enum chunk_entities_preserve_fail ref_fail = chunk_entities_preserve_last_fail();
	chunk_entities_preserve_test_reset_fail();
	t0 = time_get();
	nbt_node* fast = chunk_entities_preserve(old, generated, current, native,
											 0, 0);
	s_stats.fast_s += time_diff_s(t0, time_get());
	enum chunk_entities_preserve_fail fast_fail = chunk_entities_preserve_last_fail();
	if((ref == NULL) != (fast == NULL) || ref_fail != fast_fail) {
		fprintf(stderr, "MISMATCH %s: ref=%p fail=%d fast=%p fail=%d\n", label,
				(void*)ref, (int)ref_fail, (void*)fast, (int)fast_fail);
		assert(!"reference and indexed merge disagree");
	}
	size_t len = 0;
	if(ref) {
		struct buffer a = nbt_dump_binary(ref), b = nbt_dump_binary(fast);
		assert(a.data && b.data);
		if(a.len != b.len || memcmp(a.data, b.data, a.len)) {
			fprintf(stderr, "MISMATCH %s: bytes differ (%zu vs %zu)\n", label,
					a.len, b.len);
			assert(!"reference and indexed merge emit different bytes");
		}
		len = a.len;
		buffer_free(&a);
		buffer_free(&b);
		s_stats.ok_both++;
	} else
		s_stats.null_both++;
	nbt_free(ref);
	nbt_free(fast);
	s_stats.cases++;
	return len;
}

/* One randomized case of `n` records. aggression 0 = "gentle": only what a
 * healthy native world produces (legacy no-Air, departed natives, moved
 * survivors, one foreign record), so the OK path -- and the oracle's full
 * quadratic scan -- is exercised; higher values pile on the adversarial
 * mutations, which mostly end in fail-closed exits (also compared). */
static void random_case(size_t n, uint32_t aggression, const char* label) {
	struct archived_entity* ents = calloc(n ? n : 1, sizeof(*ents));
	assert(ents);
	for(size_t i = 0; i < n; i++)
		fill_entity(&ents[i], (uint32_t)i, chance(30));
	struct chunk_entities snap = {.entities = ents, .count = n};
	nbt_node* old = build_list(&snap);
	if(aggression == 0) {
		size_t departed = n / 4;
		struct chunk_entities survivors = {.entities = ents,
										   .count = n - departed};
		for(size_t i = 0; i < survivors.count; i++)
			if(chance(30))
				ents[i].pos[0] += 0.5f; /* moved */
		nbt_node* generated = build_list(&survivors);
		/* legacy no-Air on SURVIVORS (a departed mob without Air is not
		 * byte-provably deletable: the legacy shape is item-only, so that
		 * case is a legitimate fail-closed exit -- covered by the
		 * aggressive tier, kept out of the OK-path tier) */
		for(size_t i = 0; i < survivors.count; i++)
			if(ents[i].kind == ARCHIVED_ENTITY_ITEM && chance(25))
				remove_child(record_at(old, i), "Air");
		if(n > 0) {
			nbt_node* foreign = nbt_clone(record_at(old, rnd_below((uint32_t)n)));
			remove_cavewii_tags(foreign);
			set_id_string(foreign, "EquivForeign");
			append_record(old, foreign);
		}
		check_equiv(old, generated, &survivors, true, label);
		check_equiv(old, generated, NULL, true, label);
		nbt_free(old);
		nbt_free(generated);
		free(ents);
		return;
	}
	/* current: the same population, some records moved / re-identified */
	for(size_t i = 0; i < n; i++) {
		if(chance(30))
			ents[i].pos[0] += 0.5f;
		if(chance(5))
			ents[i].source_id = 0; /* zero runtime id: position identity */
		if(chance(5) && n > 1)
			ents[i].source_id = ents[rnd_below((uint32_t)n)].source_id;
	}
	nbt_node* generated = build_list(&snap);
	mutate_old(old, aggression);
	mutate_generated(generated, aggression);
	bool native = chance(70);
	const struct chunk_entities* current = chance(15) ? NULL : &snap;
	check_equiv(old, generated, current, native, label);
	/* and the same input with the other snapshot choice, for coverage */
	check_equiv(old, generated, current ? NULL : &snap, native, label);
	nbt_free(old);
	nbt_free(generated);
	free(ents);
}

static void adversarial_cases(void) {
	enum { N = 64 };
	struct archived_entity ents[N];
	for(size_t i = 0; i < N; i++)
		fill_entity(&ents[i], (uint32_t)i, i % 3 == 0);
	struct chunk_entities snap = {.entities = ents, .count = N};
	/* every record at ONE position, every runtime id zero -> every parsed
	 * old record has N owners by position: DUP_OWNER on the first one */
	for(size_t i = 0; i < N; i++) {
		ents[i].pos[0] = 3.5f;
		ents[i].pos[1] = 64.0f;
		ents[i].pos[2] = 3.5f;
		ents[i].source_id = 0;
	}
	nbt_node* old = build_list(&snap);
	nbt_node* gen = build_list(&snap);
	check_equiv(old, gen, &snap, true, "same-pos zero-rid native");
	check_equiv(old, gen, &snap, false, "same-pos zero-rid imported");
	check_equiv(old, gen, NULL, true, "same-pos zero-rid current==NULL");
	/* no Pos anywhere: nothing matches by position; rid zero -> no owner at
	 * all -> departed-native path (deletion proof fails: Pos missing) */
	for(size_t i = 0; i < N; i++) {
		remove_child(record_at(old, i), "Pos");
		remove_child(record_at(gen, i), "Pos");
	}
	check_equiv(old, gen, &snap, true, "no-pos native");
	check_equiv(old, gen, &snap, false, "no-pos imported");
	nbt_free(old);
	nbt_free(gen);
	/* unique runtime ids, but generated has half of them stripped so those
	 * fall back to position; old has two exact duplicates of one record and
	 * one foreign record */
	for(size_t i = 0; i < N; i++)
		fill_entity(&ents[i], (uint32_t)i, i % 4 == 0);
	old = build_list(&snap);
	gen = build_list(&snap);
	for(size_t i = 0; i < N; i += 2)
		remove_child(record_at(gen, i), "CaveWiiRuntimeId");
	append_record(old, nbt_clone(record_at(old, 5)));
	append_record(old, nbt_clone(record_at(old, 5)));
	nbt_node* foreign = nbt_clone(record_at(old, 7));
	remove_cavewii_tags(foreign);
	set_id_string(foreign, "EquivForeign");
	append_record(old, foreign);
	check_equiv(old, gen, &snap, true, "half-stripped rid + dup + foreign");
	check_equiv(old, gen, &snap, false, "half-stripped rid imported");
	check_equiv(old, gen, NULL, true, "half-stripped current==NULL");
	/* departed natives + legacy no-Air, generated shrunk by index-aligned
	 * current (the storm shape) */
	struct chunk_entities survivors = {.entities = ents, .count = N * 3 / 4};
	nbt_node* gen2 = build_list(&survivors);
	for(size_t i = 0; i < N / 4; i++)
		remove_child(record_at(old, i), "Air");
	check_equiv(old, gen2, &survivors, true, "storm shape native");
	check_equiv(old, gen2, &survivors, false, "storm shape imported");
	nbt_free(old);
	nbt_free(gen);
	nbt_free(gen2);
	/* empty sides */
	struct chunk_entities none = {0};
	nbt_node* empty = build_list(&none);
	nbt_node* some = build_list(&snap);
	check_equiv(empty, some, &snap, true, "empty old");
	check_equiv(some, empty, &none, true, "empty generated native");
	check_equiv(some, empty, &none, false, "empty generated imported");
	check_equiv(some, empty, NULL, true, "empty generated current==NULL");
	nbt_free(empty);
	nbt_free(some);
}

/* CHUNK_ENTITY_ARCHIVE_WRITE_MAX through the production merge only. */
static void write_max_case(void) {
	const size_t n = CHUNK_ENTITY_ARCHIVE_WRITE_MAX;
	struct archived_entity* ents = calloc(n, sizeof(*ents));
	assert(ents);
	for(size_t i = 0; i < n; i++)
		fill_entity(&ents[i], (uint32_t)i, false); /* unique positions */
	struct chunk_entities all = {.entities = ents, .count = n};
	struct chunk_entities survivors = {.entities = ents, .count = n * 3 / 4};
	nbt_node* old = build_list(&all);
	for(size_t i = 0; i < n / 4; i++)
		remove_child(record_at(old, i), "Air");
	nbt_node* foreign = nbt_clone(record_at(old, 1));
	remove_cavewii_tags(foreign);
	set_id_string(foreign, "EquivForeign");
	append_record(old, foreign);
	nbt_node* gen = build_list(&survivors);
	chunk_entities_preserve_test_reset_fail();
	ptime_t t0 = time_get();
	nbt_node* out = chunk_entities_preserve(old, gen, &survivors, true, 0, 0);
	float s = time_diff_s(t0, time_get());
	if(!out)
		fprintf(stderr, "WRITE_MAX merge failed: sub=%d\n",
				(int)chunk_entities_preserve_last_fail());
	assert(out && chunk_entities_preserve_last_fail() == CEP_NONE);
	size_t len = list_len(out);
	printf("WRITE_MAX merge: %zu old + 1 foreign vs %zu current -> %zu "
		   "records in %.3fs (production path only)\n", n, survivors.count,
		   len, s);
	assert(len == survivors.count + 1); /* departed gone, foreign kept */
	nbt_free(out);
	/* current == NULL at WRITE_MAX must fail closed just as fast */
	chunk_entities_preserve_test_reset_fail();
	t0 = time_get();
	out = chunk_entities_preserve(old, gen, NULL, true, 0, 0);
	s = time_diff_s(t0, time_get());
	assert(!out && chunk_entities_preserve_last_fail() == CEP_DUP_OWNER);
	printf("WRITE_MAX current==NULL: fail-closed in %.3fs\n", s);
	nbt_free(old);
	nbt_free(gen);
	free(ents);
}

static void run_tiers(const char* mode) {
	memset(&s_stats, 0, sizeof(s_stats));
	s_rng = 0x9E3779B97F4A7C15ull;
	printf("== hash mode: %s\n", mode);
	adversarial_cases();
	printf("adversarial: %zu cases (%zu ok, %zu fail-closed) identical\n",
		   s_stats.cases, s_stats.ok_both, s_stats.null_both);
	for(int c = 0; c < 200; c++)
		random_case(rnd_below(65), 0, "gentle small");
	for(int c = 0; c < 20; c++)
		random_case(64 + rnd_below(449), 0, "gentle medium");
	printf("gentle: %zu cases (%zu ok, %zu fail-closed) identical\n",
		   s_stats.cases, s_stats.ok_both, s_stats.null_both);
	for(int c = 0; c < 400; c++)
		random_case(rnd_below(65), 20 + rnd_below(60), "random small");
	for(int c = 0; c < 40; c++)
		random_case(64 + rnd_below(449), 20 + rnd_below(60), "random medium");
	printf("random: %zu cases total (%zu ok, %zu fail-closed) identical; "
		   "reference %.2fs vs indexed %.2fs\n", s_stats.cases,
		   s_stats.ok_both, s_stats.null_both, s_stats.ref_s, s_stats.fast_s);
	{
		float r0 = s_stats.ref_s, f0 = s_stats.fast_s;
		random_case(2048, 0, "gentle 2048");
		random_case(2048, 30, "random 2048");
		printf("2048-record cases: reference %.2fs vs indexed %.2fs\n",
			   s_stats.ref_s - r0, s_stats.fast_s - f0);
	}
	write_max_case();
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	run_tiers("FNV-1a 64");
	/* Review fix (3): every POS key hashes to one constant, so every probe
	 * walks the whole equal-range and only the byte verify decides. */
	chunk_entities_preserve_test_constant_hash(true);
	run_tiers("constant (collision path)");
	chunk_entities_preserve_test_constant_hash(false);
	/* Review fix (7): a successful merge reports CEP_NONE by itself. */
	{
		struct archived_entity e;
		fill_entity(&e, 0, false);
		struct chunk_entities one = {.entities = &e, .count = 1};
		nbt_node* gen = build_list(&one);
		chunk_entities_preserve_test_reset_fail();
		nbt_node* out = chunk_entities_preserve(NULL, gen, &one, true, 0, 0);
		assert(out && chunk_entities_preserve_last_fail() == CEP_NONE);
		nbt_free(out);
		nbt_free(gen);
	}
	assert(chunk_entities_preserve_index_fallbacks() == 0);
	puts("test_entity_preserve_equiv: PASS");
	return 0;
}

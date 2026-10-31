/* Copyright (c) 2026 CaveWii_v2 contributors */
/* docs/achievements_design.md section 9, row `test_stats_provenance` (LCE-02,
 * brief B3):
 *
 *   the full 5.2 matrix, one case per row incl. the Beta `func_27027_b`
 *   rejection of client-owned ids over Packet 200; achievement ids over
 *   Packet 200 through the parent check; columns by mode
 *
 * Design 5.2 splits AUTHORITY (who observed the fact: our server thread, our
 * client code, or a remote server's Packet 200) from PROVENANCE (which world
 * it happened in, decided by the bind-time mode). The acceptance matrix is
 * the product of the two with Beta's own client/server stat-owner flag
 * (`field_27088_g`), and anything not on it is dropped and counted by reason.
 *
 * Every row of that table is one case below, driven with a REAL event of the
 * right class and checked three ways: the counter that moved (or did not),
 * the COLUMN it moved in, and the `ach_status.rejected[]` reason when it was
 * refused. The matrix is written out here as data, so a row that changes in
 * the design has to change here too.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "support/test_fixture_dir.h"

#include "entity/monsters_object.h"
#include "game/achievements/achievements.h"
#include "game/achievements/achievements_api.h"

static char s_dir[128];
static uint64_t s_gen = 11u;
static int s_case;

static void make_dir(const char* dir) {
#ifdef _WIN32
	(void)mkdir(dir);
#else
	(void)mkdir(dir, 0777);
#endif
}

/* ---------------------------------------------------------------- helpers */

static const struct stats_profile* p0(void) {
	return achievements_debug_profile(0);
}

static struct ach_status status0(void) {
	struct ach_status st;
	memset(&st, 0, sizeof(st));
	assert(achievements_debug_status(0, &st));
	return st;
}

static void fresh(enum ach_bind_mode mode) {
	char id[CW_PROFILE_ID_MAX + 1];
	snprintf(id, sizeof(id), "pv%d", s_case++);
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, s_gen, id, mode) == ACH_BIND_OK);
}

static const struct stats_pair* general(uint32_t stat_id) {
	int i = stat_general_index(stat_id);
	assert(i >= 0);
	return &p0()->general[i];
}

/* ------------------------------------------------------------- the matrix */

/* The two stat CLASSES the matrix distinguishes, each represented by a real
 * event of that class [design 1.4 "Beta owner"]:
 *   CLIENT-owned  -> jump (2010), EntityPlayer.jump
 *   SERVER-owned  -> deaths (2022), EntityPlayer.onDeath
 * Using a real kind (not a synthetic id) is the point: it proves the matrix
 * is applied to the events lane A will actually emit. */
#define CLIENT_STAT 2010u
#define SERVER_STAT 2022u

enum cls { CLS_CLIENT = 0, CLS_SERVER = 1 };

/* The design 5.2 acceptance table, verbatim, in its own order. `column` is
 * only meaningful when accepted; `why` only when not. */
struct row {
	enum ach_bind_mode mode;
	uint8_t authority;
	enum cls cls;
	bool accept;
	enum stats_column column;
	enum ach_reject_reason why;
	const char* note;
};

static const struct row ROWS[] = {
	{ACH_MODE_LOCAL_WORLD, GE_AUTH_SERVER, CLS_SERVER, true,
	 STATS_COLUMN_LOCAL, ACH_REJECT_COUNT, "local world, our server, server-owned"},
	{ACH_MODE_LOCAL_WORLD, GE_AUTH_CLIENT, CLS_CLIENT, true,
	 STATS_COLUMN_LOCAL, ACH_REJECT_COUNT, "local world, our client, client-owned"},
	{ACH_MODE_LOCAL_WORLD, GE_AUTH_CLIENT, CLS_SERVER, false,
	 STATS_COLUMN_LOCAL, ACH_REJECT_WRONG_OBSERVER, "client cannot decide a server fact"},
	{ACH_MODE_LOCAL_WORLD, GE_AUTH_SERVER, CLS_CLIENT, false,
	 STATS_COLUMN_LOCAL, ACH_REJECT_WRONG_OBSERVER, "server cannot decide a client fact"},
	{ACH_MODE_LOCAL_WORLD, GE_AUTH_PACKET200, CLS_SERVER, false,
	 STATS_COLUMN_LOCAL, ACH_REJECT_NO_REMOTE_SERVER, "no remote server exists"},
	{ACH_MODE_LOCAL_WORLD, GE_AUTH_PACKET200, CLS_CLIENT, false,
	 STATS_COLUMN_LOCAL, ACH_REJECT_NO_REMOTE_SERVER, "no remote server exists"},
	{ACH_MODE_REMOTE_SERVER, GE_AUTH_CLIENT, CLS_CLIENT, true,
	 STATS_COLUMN_REMOTE, ACH_REJECT_COUNT, "EntityClientPlayerMP.addStat"},
	{ACH_MODE_REMOTE_SERVER, GE_AUTH_PACKET200, CLS_SERVER, true,
	 STATS_COLUMN_REMOTE, ACH_REJECT_COUNT, "func_27027_b accepts a server-owned id"},
	{ACH_MODE_REMOTE_SERVER, GE_AUTH_PACKET200, CLS_CLIENT, false,
	 STATS_COLUMN_REMOTE, ACH_REJECT_WRONG_OBSERVER, "func_27027_b rejects client-owned ids"},
	{ACH_MODE_REMOTE_SERVER, GE_AUTH_CLIENT, CLS_SERVER, false,
	 STATS_COLUMN_REMOTE, ACH_REJECT_WRONG_OBSERVER, "our client cannot decide a server fact"},
	{ACH_MODE_REMOTE_SERVER, GE_AUTH_SERVER, CLS_SERVER, false,
	 STATS_COLUMN_REMOTE, ACH_REJECT_NO_LOCAL_SERVER, "no local server runs"},
	{ACH_MODE_REMOTE_SERVER, GE_AUTH_SERVER, CLS_CLIENT, false,
	 STATS_COLUMN_REMOTE, ACH_REJECT_NO_LOCAL_SERVER, "no local server runs"},
};
#define ROW_COUNT ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

/* A PACKET200 observation is always a GE_REMOTE_STAT carrying the Beta stat
 * id; the other two authorities carry the native kind for that class. */
static void emit_row(const struct row* r) {
	struct game_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.authority = r->authority;
	ev.who.slot = 0;
	ev.who.generation = s_gen;
	if(r->authority == GE_AUTH_PACKET200) {
		ev.kind = GE_REMOTE_STAT;
		ev.u.remote.stat_id
			= (int32_t)(r->cls == CLS_CLIENT ? CLIENT_STAT : SERVER_STAT);
		ev.u.remote.amount = 1;
	} else if(r->cls == CLS_CLIENT) {
		ev.kind = GE_JUMPED;
	} else {
		ev.kind = GE_DIED;
	}
	game_event_emit(&ev);
}

static void test_acceptance_matrix(void) {
	for(int i = 0; i < ROW_COUNT; i++) {
		const struct row* r = &ROWS[i];
		uint32_t id = (r->cls == CLS_CLIENT) ? CLIENT_STAT : SERVER_STAT;
		struct ach_status st;
		const struct stats_pair* c;

		fresh(r->mode);
		emit_row(r);
		achievements_tick();

		c = general(id);
		st = status0();
		if(r->accept) {
			uint32_t want_local = (r->column == STATS_COLUMN_LOCAL) ? 1u : 0u;
			uint32_t want_remote = (r->column == STATS_COLUMN_REMOTE) ? 1u : 0u;
			if(c->local != want_local || c->remote != want_remote
			   || st.accepted != 1u) {
				fprintf(stderr,
						"FAIL row %d (%s): local %lu remote %lu accepted %lu\n",
						i, r->note, (unsigned long)c->local,
						(unsigned long)c->remote, (unsigned long)st.accepted);
				assert(0);
			}
			for(int w = 0; w < ACH_REJECT_COUNT; w++)
				assert(st.rejected[w] == 0u);
		} else {
			if(c->local != 0u || c->remote != 0u || st.accepted != 0u
			   || st.rejected[r->why] != 1u) {
				fprintf(stderr,
						"FAIL row %d (%s): local %lu remote %lu accepted %lu "
						"reason[%d] %lu\n",
						i, r->note, (unsigned long)c->local,
						(unsigned long)c->remote, (unsigned long)st.accepted,
						(int)r->why, (unsigned long)st.rejected[r->why]);
				assert(0);
			}
			/* Exactly ONE reason, so a row cannot pass by being rejected
			 * twice for two different reasons. */
			{
				uint32_t total = 0u;
				for(int w = 0; w < ACH_REJECT_COUNT; w++)
					total += st.rejected[w];
				assert(total == 1u);
			}
		}
		/* Nothing is ever dirty on a rejection, and always dirty on an
		 * acceptance that moved a counter. */
		assert(st.dirty == r->accept);
		achievements_unbind(0, s_gen);
	}

	/* The table above must BE the design table: both modes x three
	 * authorities x both classes, no row missing and none duplicated. */
	assert(ROW_COUNT == 2 * 3 * 2);
	for(int a = 0; a < ROW_COUNT; a++)
		for(int b = a + 1; b < ROW_COUNT; b++)
			assert(!(ROWS[a].mode == ROWS[b].mode
					 && ROWS[a].authority == ROWS[b].authority
					 && ROWS[a].cls == ROWS[b].cls));
	puts("PASS every row of the design 5.2 acceptance matrix");
}

/* Beta's NetClientHandler.func_27027_b hands the packet to the same addStat
 * path, so a CLIENT-owned id arriving over Packet 200 is refused -- the
 * client already counted it locally and the server has no business reporting
 * it. Checked on the one id where it is observable in the achievement half
 * too: openInventory is the only achievement with field_27088_g set. */
static void test_packet200_rejects_client_owned_ids(void) {
	struct game_event ev;
	struct ach_status st;

	fresh(ACH_MODE_REMOTE_SERVER);
	memset(&ev, 0, sizeof(ev));
	ev.kind = GE_REMOTE_STAT;
	ev.authority = GE_AUTH_PACKET200;
	ev.who.generation = s_gen;

	/* A client-owned general stat (walkOneCm). */
	ev.u.remote.stat_id = 2000;
	ev.u.remote.amount = 500;
	game_event_emit(&ev);
	/* The client-owned ACHIEVEMENT (openInventory, index 0). */
	ev.u.remote.stat_id = (int32_t)(CW_ACH_BASE + 0u);
	ev.u.remote.amount = 1;
	game_event_emit(&ev);
	achievements_tick();

	st = status0();
	assert(general(2000u)->local == 0u && general(2000u)->remote == 0u);
	assert(!ach_unlocked(p0(), 0));
	assert(st.accepted == 0u);
	assert(st.rejected[ACH_REJECT_WRONG_OBSERVER] == 2u);
	assert(!st.dirty);

	/* A server-owned one on the same connection is accepted, so the
	 * rejection above is the OWNER flag and not "Packet 200 is off". */
	ev.u.remote.stat_id = 2023; /* mobKills */
	ev.u.remote.amount = 4;
	game_event_emit(&ev);
	achievements_tick();
	assert(general(2023u)->remote == 4u && general(2023u)->local == 0u);

	achievements_unbind(0, s_gen);
	puts("PASS Packet 200 refuses client-owned ids and takes server-owned");
}

/* Design 1.2 point 4: an achievement id over Packet 200 goes through the SAME
 * parent check and the same toast as a locally decided one -- it is not a raw
 * counter write. Both directions are checked: parent locked -> dropped
 * entirely (Beta drops, it does not defer), parent unlocked -> counted, with
 * the packet's own amount, and toasted. */
static void test_packet200_achievement_goes_through_the_parent(void) {
	struct game_event ev;
	struct ach_notice n;
	struct ach_status st;

	fresh(ACH_MODE_REMOTE_SERVER);
	memset(&ev, 0, sizeof(ev));
	ev.kind = GE_REMOTE_STAT;
	ev.authority = GE_AUTH_PACKET200;
	ev.who.generation = s_gen;

	/* mineWood (index 1) needs openInventory (index 0) first. */
	ev.u.remote.stat_id = (int32_t)(CW_ACH_BASE + 1u);
	ev.u.remote.amount = 1;
	game_event_emit(&ev);
	achievements_tick();

	st = status0();
	assert(!ach_unlocked(p0(), 1));
	/* A parent-locked drop is its own status counter, not a rejection
	 * reason (R2 LOW 11). */
	assert(st.ach_dropped_parent_locked == 1u);
	assert(st.accepted == 0u);
	memset(&n, 0, sizeof(n));
	(void)achievements_debug_notice(0, &n);
	assert(n.ach_id == -1);

	/* Unlock the parent the way a remote client does -- openInventory is
	 * client-owned, so it is the CLIENT authority that may report it. */
	{
		struct game_event open;
		memset(&open, 0, sizeof(open));
		open.kind = GE_INVENTORY_OPENED;
		open.authority = GE_AUTH_CLIENT;
		open.who.generation = s_gen;
		game_event_emit(&open);
	}
	achievements_tick();
	assert(ach_unlocked(p0(), 0));
	assert(p0()->ach[0].remote == 1u && p0()->ach[0].local == 0u);
	achievements_notice_ack(0);

	/* Now the same packet lands: counted with the PACKET's amount (Beta's
	 * func_27027_b passes the value through, it is not one per packet) and
	 * toasted once. */
	ev.u.remote.stat_id = (int32_t)(CW_ACH_BASE + 1u);
	ev.u.remote.amount = 3;
	game_event_emit(&ev);
	achievements_tick();
	assert(ach_unlocked(p0(), 1));
	assert(p0()->ach[1].remote == 3u && p0()->ach[1].local == 0u);
	memset(&n, 0, sizeof(n));
	assert(achievements_debug_notice(0, &n) && n.ach_id == 1);

	achievements_unbind(0, s_gen);
	puts("PASS a Packet 200 achievement takes the parent check and the toast");
}

/* Design 6 / section 7: the same fact in the two session modes fills the two
 * columns, and the B4 rows hand the screens local, remote AND the saturating
 * 64-bit total -- which is what "the totals shown by default = local +
 * remote" means for lane C. */
static void test_columns_read_back_through_the_api(void) {
	struct game_event ev;
	struct stat_row row;
	struct stat_cell mined, crafted, used;
	char id[CW_PROFILE_ID_MAX + 1];
	int i;

	/* One local-world session: a jump and a dirt harvest. */
	snprintf(id, sizeof(id), "pv%d", s_case++);
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, s_gen, id, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	memset(&ev, 0, sizeof(ev));
	ev.kind = GE_JUMPED;
	ev.authority = GE_AUTH_CLIENT;
	ev.who.generation = s_gen;
	game_event_emit(&ev);
	memset(&ev, 0, sizeof(ev));
	ev.kind = GE_BLOCK_HARVESTED;
	ev.authority = GE_AUTH_SERVER;
	ev.who.generation = s_gen;
	ev.u.item.id = 3u;
	ev.u.item.count = 1u;
	game_event_emit(&ev);
	achievements_tick();
	achievements_flush_all();
	achievements_unbind(0, s_gen);

	/* The SAME profile rebound as a remote session: the counters continue in
	 * the other column, which is the whole point of the pair. */
	s_gen++;
	assert(achievements_bind(0, s_gen, id, ACH_MODE_REMOTE_SERVER)
		   == ACH_BIND_OK);
	memset(&ev, 0, sizeof(ev));
	ev.kind = GE_JUMPED;
	ev.authority = GE_AUTH_CLIENT;
	ev.who.generation = s_gen;
	game_event_emit(&ev);
	game_event_emit(&ev);
	memset(&ev, 0, sizeof(ev));
	ev.kind = GE_REMOTE_STAT;
	ev.authority = GE_AUTH_PACKET200;
	ev.who.generation = s_gen;
	ev.u.remote.stat_id = (int32_t)(CW_STAT_MINE_BASE + 3u);
	ev.u.remote.amount = 5;
	game_event_emit(&ev);
	achievements_tick();

	assert(general(2010u)->local == 1u && general(2010u)->remote == 2u);
	assert(p0()->mine[3].local == 1u && p0()->mine[3].remote == 5u);

	/* Read back through the B4 API: all three numbers, per owner slot. */
	for(i = 0; stats_general_iter(0, i, &row); i++)
		if(row.id == 2010u)
			break;
	assert(row.id == 2010u);
	assert(row.value.local == 1u && row.value.remote == 2u
		   && row.value.total == 3u);
	assert(stats_block_row(0, 3u, &mined, &crafted, &used));
	assert(mined.local == 1u && mined.remote == 5u && mined.total == 6u);

	achievements_unbind(0, s_gen);
	puts("PASS local / remote / total reach the screens through the B4 rows");
}

int main(void) {
	test_fixture_dir("test_stats_provenance_tmp", s_dir, sizeof(s_dir));
	make_dir(s_dir);

	test_acceptance_matrix();
	test_packet200_rejects_client_owned_ids();
	test_packet200_achievement_goes_through_the_parent();
	test_columns_read_back_through_the_api();

	achievements_shutdown();
	test_fixture_dir_cleanup(s_dir);
	puts("PASS test_stats_provenance");
	return 0;
}

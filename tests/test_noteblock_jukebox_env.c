/*
	Beta 1.7.3 note block + jukebox (noteblock/jukebox-slice) goldens, driven
	over a live superflat server_world -- the dispenser env-test harness
	pattern (tests/test_dispenser_env.c).

	Ground truth (decompile 740c583, cited again at each assertion below):
	  BlockNote.onNeighborBlockChange/.blockActivated/.onBlockClicked;
	  TileEntityNote.changePitch/.triggerNote/.readFromNBT; BlockNote.playBlock;
	  BlockJukeBox.blockActivated/.ejectRecord/.func_28038_b_/.onBlockRemoval;
	  TileEntityRecordPlayer.read/writeToNBT; ItemRecord.onItemUse;
	  TileEntity.<clinit> (the "Music"/"RecordPlayer" NBT id strings).
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "block/jukebox_logic.h"
#include "block/noteblock_logic.h"
#include "entity/entity.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h" // inventory_logic_player
#include "network/server_interface.h"
#include "network/server_jukebox.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_noteblock.h"
#include "network/server_world.h"
#include "platform/audio.h" // SOUND_NOTE_*
#include "platform/thread.h"

// item_record.c's onUseOnBlock -- declared here (not a public header) the
// same way items_object.h declares item_hoe_onUseOnBlock: an `extern` at the
// call site, matching the item-vtable signature exactly.
extern bool item_record_onUseOnBlock(struct server_local*, struct server_player*, struct item_data*,
									 struct block_info*, enum side);
#include "util.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_noteblock_jukebox_env_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor plane
#define Y 101  // block-under-test plane

static struct server_local s;
static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static bool feq(float a, float b) { return fabsf(a - b) < 1.0e-5f; }

static void set_block(int x, int y, int z, int type, int meta) {
	bool ok = server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)});
	if(!ok) // stdout is fully buffered under a pipe; name the site on stderr
		fprintf(stderr, "set_block FAILED at (%d,%d,%d) type=%d\n", x, y, z,
				type);
	assert(ok && "server_world_set_block refused the edit");
}
static int get_meta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? (b.metadata & 0x0F)
														  : -1;
}
static int get_type(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? b.type : -1;
}

// Place a note block at (x,y,z), register its TE, and return the note-value
// pointer (never NULL -- asserts on registration failure, matching every
// other fixture helper in this suite).
static int8_t* place_noteblock(int x, int y, int z) {
	set_block(x, y, z, BLOCK_NOTEBLOCK, 0);
	int idx = noteblock_te_register(&s, x, y, z);
	assert(idx >= 0);
	int8_t* note = noteblock_te_lookup(&s, x, y, z);
	assert(note);
	return note;
}

// Place a jukebox at (x,y,z), register its TE, and return the record-value
// pointer.
static int32_t* place_jukebox(int x, int y, int z) {
	set_block(x, y, z, BLOCK_JUKEBOX, 0);
	int idx = jukebox_te_register(&s, x, y, z);
	assert(idx >= 0);
	int32_t* rec = jukebox_te_lookup(&s, x, y, z);
	assert(rec);
	return rec;
}

static struct block_info info_at(int x, int y, int z, struct block_data* bd) {
	assert(server_world_get_block(&s.worlds[0], x, y, z, bd));
	return (struct block_info) {.block = bd, .x = x, .y = y, .z = z};
}

// --- RPC capture -------------------------------------------------------------

static int g_sound_count;
static struct client_rpc g_last_sound;
static int g_record_event_count;
static struct client_rpc g_last_record_event;
static int g_spawn_item_count;
static struct client_rpc g_last_spawn_item;

static void reset_capture(void) {
	g_sound_count = 0;
	g_record_event_count = 0;
	g_spawn_item_count = 0;
	memset(&g_last_sound, 0, sizeof(g_last_sound));
	memset(&g_last_record_event, 0, sizeof(g_last_record_event));
	memset(&g_last_spawn_item, 0, sizeof(g_last_spawn_item));
}

static void capture_rpc(const struct client_rpc* call, void* user) {
	(void)user;
	switch(call->type) {
		case CRPC_SOUND:
			g_sound_count++;
			g_last_sound = *call;
			break;
		case CRPC_RECORD_EVENT:
			g_record_event_count++;
			g_last_record_event = *call;
			break;
		case CRPC_SPAWN_ITEM:
			g_spawn_item_count++;
			g_last_spawn_item = *call;
			break;
		default: break;
	}
}

// ============================================================================
// 1. Pure: TileEntityNote.changePitch -- (note+1)%25, cycles 0..24 then wraps.
// [decompile 740c583]
// ============================================================================
static void test_changepitch_cycle(void) {
	uint8_t note = 0;
	for(int i = 0; i < 24; i++) {
		note = noteblock_change_pitch(note);
		assert(note == (uint8_t)(i + 1));
	}
	assert(note == 24);
	note = noteblock_change_pitch(note); // 24 -> wraps to 0
	assert(note == 0);

	printf("  changePitch: cycles 0..24 then wraps to 0 OK\n");
}

// ============================================================================
// 2. Pure: TileEntityNote.triggerNote's instrument-from-material table (all 5
// cases: rock/sand/glass/wood/else). [decompile 740c583]
// ============================================================================
static void test_instrument_table(void) {
	assert(noteblock_instrument_for_material(MATERIAL_STONE)
		   == NOTE_INSTRUMENT_BD);
	assert(noteblock_instrument_for_material(MATERIAL_SAND)
		   == NOTE_INSTRUMENT_SNARE);
	assert(noteblock_instrument_for_material(MATERIAL_GLASS)
		   == NOTE_INSTRUMENT_HAT);
	assert(noteblock_instrument_for_material(MATERIAL_WOOD)
		   == NOTE_INSTRUMENT_BASSATTACK);
	// Every other material (the decompile's implicit else, var6 stays 0) ->
	// harp. Spot-check two: wool and organic (dirt/grass in this engine).
	assert(noteblock_instrument_for_material(MATERIAL_WOOL)
		   == NOTE_INSTRUMENT_HARP);
	assert(noteblock_instrument_for_material(MATERIAL_ORGANIC)
		   == NOTE_INSTRUMENT_HARP);

	printf("  instrument table: rock->bd, sand->snare, glass->hat, "
		  "wood->bassattack, else->harp OK\n");
}

// ============================================================================
// 3. Pure: BlockNote.playBlock's pitch formula -- (float)Math.pow(2.0D,
// (note-12)/12.0D). Spot values are exact powers of two (fp32-exact, no
// tolerance needed). [decompile 740c583]
// ============================================================================
static void test_pitch_formula(void) {
	assert(noteblock_pitch(0) == 0.5F);
	assert(noteblock_pitch(12) == 1.0F);
	assert(noteblock_pitch(24) == 2.0F);
	// Non-power-of-two spot check with tolerance (fp32 pow rounding).
	assert(feq(noteblock_pitch(6), 0.7071068F)); // 2^(-0.5)

	printf("  pitch formula: note 0->0.5F, 12->1.0F, 24->2.0F exactly, "
		  "6->2^-0.5 OK\n");
}

// ============================================================================
// 4. Pure: BlockJukeBox.func_28038_b_'s eject-offset formula, 3 KNOWN draws.
// [decompile 740c583]
// ============================================================================
static void test_jukebox_eject_offset_formula(void) {
	float dx, dy, dz;
	// r0=r1=r2=0.0F: dx=0.15F, dy=0.66F, dz=0.15F (the (1-f)*k terms alone).
	jukebox_eject_offset(0.0F, 0.0F, 0.0F, &dx, &dy, &dz);
	assert(feq(dx, 0.15F) && feq(dy, 0.66F) && feq(dz, 0.15F));
	// r0=r1=r2=1.0F: dx=0.85F, dy=1.36F, dz=0.85F.
	jukebox_eject_offset(1.0F, 1.0F, 1.0F, &dx, &dy, &dz);
	assert(feq(dx, 0.85F) && feq(dy, 1.36F) && feq(dz, 0.85F));
	// Distinct per-axis draws are NOT mixed up (x uses r0, y uses r1, z r2).
	jukebox_eject_offset(1.0F, 0.0F, 0.5F, &dx, &dy, &dz);
	assert(feq(dx, 0.85F) && feq(dy, 0.66F) && feq(dz, 0.5F));

	printf("  jukebox eject-offset formula: f=0.7F offsets at r=0/1/mixed "
		  "OK\n");
}

// ============================================================================
// 5. Note block placement registers a TE with note=0.
// ============================================================================
static void test_noteblock_place_registers_te(void) {
	int x = 5, y = Y, z = 5;
	set_block(x, YF, z, BLOCK_STONE, 0);
	struct item_data held = {.id = BLOCK_NOTEBLOCK, .count = 1};
	struct block_data bd_where;
	struct block_info where = info_at(x, y, z, &bd_where);
	// where==on for a fresh-air target (the item-place convention).
	struct block_info on = where;
	bool placed
		= block_noteblock.block_item.onItemPlace(&s, &s.player, &held, &where, &on, SIDE_TOP);
	assert(placed);
	assert(get_type(x, y, z) == BLOCK_NOTEBLOCK);
	int8_t* note = noteblock_te_lookup(&s, x, y, z);
	assert(note && *note == 0);

	printf("  note block: onItemPlace registers a fresh TE (note=0) OK\n");
}

// ============================================================================
// 6. Right-click: changePitch then triggerNote, sound emitted with the
// correct instrument (stone below -> bd) and pitch. [decompile 740c583:
// BlockNote.blockActivated/TileEntityNote.triggerNote/BlockNote.playBlock]
// ============================================================================
static void test_noteblock_right_click_trigger(void) {
	int x = 10, y = Y, z = 40;
	set_block(x, y - 1, z, BLOCK_STONE, 0); // material below -> bd
	int8_t* note = place_noteblock(x, y, z);
	assert(*note == 0);

	reset_capture();
	struct block_data bd;
	struct block_info info = info_at(x, y, z, &bd);
	block_noteblock.onRightClick(&s, &s.player, NULL, &info, &info, SIDE_TOP);

	assert(*note == 1); // changePitch: 0 -> 1
	assert(g_sound_count == 1);
	assert(g_last_sound.payload.sound.sound_id == SOUND_NOTE_BD);
	assert(feq(g_last_sound.payload.sound.vol, 3.0F));
	assert(feq(g_last_sound.payload.sound.pitch, noteblock_pitch(1)));
	assert(feq(g_last_sound.payload.sound.pos[0], x + 0.5F)
		   && feq(g_last_sound.payload.sound.pos[1], y + 0.5F)
		   && feq(g_last_sound.payload.sound.pos[2], z + 0.5F));

	// A second right-click cycles again and plays again (right-click is NOT
	// edge-gated -- only the redstone path is).
	block_noteblock.onRightClick(&s, &s.player, NULL, &info, &info, SIDE_TOP);
	assert(*note == 2);
	assert(g_sound_count == 2);

	printf("  note block: right-click changePitch(0->1) + play bd @ pitch "
		  "%.4f, vol 3.0 OK\n", (double)noteblock_pitch(1));
}

// ============================================================================
// 7. Covered block: triggerNote's block-ABOVE-must-be-air gate silences the
// sound, but changePitch ALWAYS runs (Beta calls both unconditionally from
// blockActivated; only triggerNote's own internal gate is conditional).
// [decompile 740c583]
// ============================================================================
static void test_noteblock_covered_silence(void) {
	int x = 15, y = Y, z = 40;
	set_block(x, y - 1, z, BLOCK_STONE, 0);
	set_block(x, y + 1, z, BLOCK_DIRT, 0); // covered
	int8_t* note = place_noteblock(x, y, z);

	reset_capture();
	struct block_data bd;
	struct block_info info = info_at(x, y, z, &bd);
	block_noteblock.onRightClick(&s, &s.player, NULL, &info, &info, SIDE_TOP);

	assert(*note == 1); // changePitch still ran
	assert(g_sound_count == 0); // but triggerNote's gate silenced it

	printf("  note block: covered (block above != air) -> changePitch runs, "
		  "NO sound OK\n");
}

// ============================================================================
// 8. Instrument variety: sand/glass/wood/dirt(else) below -> snare/hat/
// bassattack/harp. [decompile 740c583]
// ============================================================================
static void test_noteblock_instrument_variety(void) {
	struct {
		int below_type;
		uint16_t expect_sound;
		const char* name;
	} cases[] = {
		{BLOCK_SAND, SOUND_NOTE_SNARE, "sand->snare"},
		{BLOCK_GLASS, SOUND_NOTE_HAT, "glass->hat"},
		{BLOCK_PLANKS, SOUND_NOTE_BASSATTACK, "wood->bassattack"},
		{BLOCK_DIRT, SOUND_NOTE_HARP, "dirt(else)->harp"},
	};
	for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int x = 20 + (int)i, y = Y, z = 41;
		set_block(x, y - 1, z, cases[i].below_type, 0);
		place_noteblock(x, y, z);

		reset_capture();
		struct block_data bd;
		struct block_info info = info_at(x, y, z, &bd);
		block_noteblock.onRightClick(&s, &s.player, NULL, &info, &info, SIDE_TOP);

		assert(g_sound_count == 1);
		assert(g_last_sound.payload.sound.sound_id == cases[i].expect_sound
			   && cases[i].name);
	}

	printf("  note block: instrument variety (sand/glass/wood/dirt) OK\n");
}

// ============================================================================
// 9. Redstone rising-edge: BlockNote.onNeighborBlockChange gated on the
// notifying block's canProvidePower, then fires ONLY on 0->1 -- and ONLY
// triggerNote, NEVER changePitch (the note VALUE never changes on a redstone
// trigger; only right-click changes it -- decompile's onNeighborBlockChange
// calls `var7.triggerNote(...)`, not `var7.changePitch()`). Power HELD
// (re-notified with the SAME power state) does NOT re-trigger; a genuine
// off/on cycle DOES trigger again. [decompile 740c583]
// ============================================================================
static void test_noteblock_redstone_rising_edge(void) {
	int x = 30, y = Y, z = 42;
	set_block(x, y - 1, z, BLOCK_STONE, 0);
	int8_t* note = place_noteblock(x, y, z);
	int8_t fixed_note = *note; // never changes for the rest of this test

	set_block(x + 2, y, z, BLOCK_STONE, 0); // the lever's wall support
	// Wall lever, orientation o=1 (meta&7==1): BlockLever.isIndirectlyPower-
	// ingTo's WEAK/direct table maps o==1 to powering side 5 (+X FROM the
	// lever) -- i.e. it weakly powers whatever sits at the lever's -X, which
	// is this note block. The note block's redstone check is the WEAK
	// isBlockGettingPowered query (orientation-sensitive), UNLIKE the
	// dispenser's STRONG isBlockIndirectlyGettingPowered (where an ON lever
	// powers every side regardless of orientation) -- so this fixture cannot
	// reuse the dispenser test's arbitrary meta=5 floor-lever placement.
	// Placed OFF (bit 8 unset): this placement itself notifies the note
	// block (server_world_set_block's own notify chokepoint reaches it as
	// one of the lever's 6 neighbours) but powered==false==prev is a no-op,
	// same as Beta's real placement-time notify. [decompile 740c583:
	// BlockLever.isIndirectlyPoweringTo]
	set_block(x + 1, y, z, BLOCK_LEVER, 1);
	struct block_data lb;
	struct block_info lever_info = {.block = &lb, .x = x + 1, .y = y, .z = z};
	assert(server_world_get_block(&s.worlds[0], x + 1, y, z, &lb));

	// 2026-09-03: block_lever.c's onRightClick now ALSO plays its OWN
	// random.click [740c583: BlockLever.blockActivated] on every toggle,
	// in addition to whatever this cascades into via notifyNeighbours_from
	// -- a genuine field-regression fix (the lever call used to be silent
	// here, which was itself the bug; see the field-regressions-0903 lane).
	// So every REAL block_lever.onRightClick call below now contributes
	// +1 to g_sound_count on top of the note-trigger counts this test was
	// already pinning; the manual (non-lever) onNeighbourBlockChange call
	// does not.
	reset_capture();
	block_lever.onRightClick(&s, &s.player, NULL, &lever_info, &lever_info, SIDE_TOP);
	assert((get_meta(x + 1, y, z) & 8) != 0); // lever really ON
	assert(*note == fixed_note); // triggerNote does NOT changePitch
	assert(g_sound_count == 2); // lever's own click (1) + rising-edge note trigger (1)

	// Power HELD: re-notify with the origin still power-capable while the
	// lever stays ON (the redundant-notify-mid-cascade scenario) -- must NOT
	// re-trigger, since previousRedstoneState is already true. Not a lever
	// onRightClick call, so no extra click sound here either.
	server_world_notify_origin_set(BLOCK_LEVER);
	struct block_data nb;
	struct block_info note_info = info_at(x, y, z, &nb);
	block_noteblock.onNeighbourBlockChange(&s, &note_info);
	assert(*note == fixed_note);
	assert(g_sound_count == 2); // unchanged

	// Falling edge: OFF -> state updates, but NO trigger (Beta only triggers
	// on var6==true). The lever's own click still fires on this real toggle.
	assert(server_world_get_block(&s.worlds[0], x + 1, y, z, &lb));
	struct block_info lever_info2
		= {.block = &lb, .x = x + 1, .y = y, .z = z};
	block_lever.onRightClick(&s, &s.player, NULL, &lever_info2, &lever_info2, SIDE_TOP);
	assert((get_meta(x + 1, y, z) & 8) == 0); // OFF
	assert(*note == fixed_note);
	assert(g_sound_count == 3); // +1 lever click only, no new note trigger

	// A genuine second ON transition triggers again (+1 lever click, +1 note trigger).
	assert(server_world_get_block(&s.worlds[0], x + 1, y, z, &lb));
	struct block_info lever_info3
		= {.block = &lb, .x = x + 1, .y = y, .z = z};
	block_lever.onRightClick(&s, &s.player, NULL, &lever_info3, &lever_info3, SIDE_TOP);
	assert((get_meta(x + 1, y, z) & 8) != 0); // ON again
	assert(*note == fixed_note);
	assert(g_sound_count == 5);

	printf("  note block: redstone rising-edge -- power held = one note, "
		  "off/on cycles trigger again OK\n");
}

// ============================================================================
// 10. Jukebox placement registers a TE with record=0.
// ============================================================================
static void test_jukebox_place_registers_te(void) {
	int x = 6, y = Y, z = 5;
	struct item_data held = {.id = BLOCK_JUKEBOX, .count = 1};
	struct block_data bd_where;
	struct block_info where = info_at(x, y, z, &bd_where);
	struct block_info on = where;
	bool placed
		= block_jukebox.block_item.onItemPlace(&s, &s.player, &held, &where, &on, SIDE_TOP);
	assert(placed);
	assert(get_type(x, y, z) == BLOCK_JUKEBOX);
	assert(get_meta(x, y, z) == 0);
	int32_t* rec = jukebox_te_lookup(&s, x, y, z);
	assert(rec && *rec == 0);

	printf("  jukebox: onItemPlace registers a fresh TE (record=0) OK\n");
}

// ============================================================================
// 11. Insert via ItemRecord.onItemUse: consumes the held stack, meta 1,
// Record persisted, world event 1005 data = the inserted record's item id.
// [decompile 740c583: BlockJukeBox.ejectRecord / ItemRecord.onItemUse]
// ============================================================================
static void test_jukebox_insert_via_item_use(void) {
	int x = 40, y = Y, z = 43;
	int32_t* rec = place_jukebox(x, y, z);
	assert(*rec == 0 && get_meta(x, y, z) == 0);

	struct item_data held = {.id = ITEM_RECORD_13, .durability = 0, .count = 1};
	struct block_data bd;
	struct block_info on = info_at(x, y, z, &bd);

	reset_capture();
	bool used = item_record_onUseOnBlock(&s, &s.player, &held, &on, SIDE_TOP);
	assert(used);
	assert(held.count == 0); // Beta: --stackSize
	assert(*rec == ITEM_RECORD_13);
	assert(get_meta(x, y, z) == 1);
	assert(g_record_event_count == 1);
	assert(g_last_record_event.payload.record_event.record_id
		   == ITEM_RECORD_13);

	printf("  jukebox: insert (item onUseOnBlock) consumes stack, meta 1, "
		  "Record=%d, world event 1005 data=%d OK\n",
		  ITEM_RECORD_13, ITEM_RECORD_13);
}

// ============================================================================
// 12. Insert refused when a record is already present (meta != 0) -- falls
// through unhandled, matching Beta's onItemUse `if (metadata==0) {...} else
// return false`.
// ============================================================================
static void test_jukebox_insert_blocked_when_occupied(void) {
	int x = 41, y = Y, z = 43;
	int32_t* rec = place_jukebox(x, y, z);
	*rec = ITEM_RECORD_CAT;
	set_block(x, y, z, BLOCK_JUKEBOX, 1); // meta 1: occupied

	struct item_data held = {.id = ITEM_RECORD_13, .durability = 0, .count = 1};
	struct block_data bd;
	struct block_info on = info_at(x, y, z, &bd);

	reset_capture();
	bool used = item_record_onUseOnBlock(&s, &s.player, &held, &on, SIDE_TOP);
	assert(!used);
	assert(held.count == 1); // untouched
	assert(*rec == ITEM_RECORD_CAT); // untouched
	assert(g_record_event_count == 0);

	printf("  jukebox: insert refused when occupied (meta!=0) OK\n");
}

// ============================================================================
// 13. Eject via right-click: world event 1005 data 0 (stop) BEFORE teardown,
// record cleared, meta 0, item spawned at the exact offset with the exact
// stack (id, count 1, damage 0). [decompile 740c583: BlockJukeBox.
// func_28038_b_]
// ============================================================================
static void test_jukebox_eject_right_click(void) {
	int x = 45, y = Y, z = 44;
	int32_t* rec = place_jukebox(x, y, z);
	*rec = ITEM_RECORD_CAT;
	set_block(x, y, z, BLOCK_JUKEBOX, 1);

	s.rand_src = (struct random_gen) {.seed = 12345};
	float r0 = rand_gen_flt(&s.rand_src);
	float r1 = rand_gen_flt(&s.rand_src);
	float r2 = rand_gen_flt(&s.rand_src);
	s.rand_src = (struct random_gen) {.seed = 12345}; // rewind for the real call
	float edx, edy, edz;
	jukebox_eject_offset(r0, r1, r2, &edx, &edy, &edz);

	reset_capture();
	struct block_data bd;
	struct block_info on = info_at(x, y, z, &bd);
	assert(block_jukebox.onRightClickHandled);
	bool handled
		= block_jukebox.onRightClickHandled(&s, &s.player, NULL, &on, &on, SIDE_TOP);
	assert(handled);

	assert(*rec == 0);
	assert(get_meta(x, y, z) == 0);
	assert(g_record_event_count == 1);
	assert(g_last_record_event.payload.record_event.record_id == 0); // stop

	assert(g_spawn_item_count == 1);
	assert(g_last_spawn_item.payload.spawn_item.item.id == ITEM_RECORD_CAT);
	assert(g_last_spawn_item.payload.spawn_item.item.count == 1);
	assert(g_last_spawn_item.payload.spawn_item.item.durability == 0);
	assert(feq(g_last_spawn_item.payload.spawn_item.pos[0], x + edx)
		   && feq(g_last_spawn_item.payload.spawn_item.pos[1], y + edy)
		   && feq(g_last_spawn_item.payload.spawn_item.pos[2], z + edz));

	printf("  jukebox: right-click eject -- event 1005/0, meta 0, exact "
		  "item+offset OK\n");
}

// ============================================================================
// 14. Right-click on an EMPTY jukebox (meta 0) is UNHANDLED (Beta
// blockActivated returns false without touching anything).
// ============================================================================
static void test_jukebox_eject_empty_is_unhandled(void) {
	int x = 46, y = Y, z = 44;
	place_jukebox(x, y, z);
	assert(get_meta(x, y, z) == 0);

	reset_capture();
	struct block_data bd;
	struct block_info on = info_at(x, y, z, &bd);
	bool handled
		= block_jukebox.onRightClickHandled(&s, &s.player, NULL, &on, &on, SIDE_TOP);
	assert(!handled);
	assert(g_record_event_count == 0);
	assert(g_spawn_item_count == 0);

	printf("  jukebox: right-click on empty (meta 0) is UNHANDLED OK\n");
}

// ============================================================================
// 15. onBreak ejects first (drops the record), then frees the TE. [decompile
// 740c583: BlockJukeBox.onBlockRemoval]
// ============================================================================
static void test_jukebox_break_ejects_then_frees(void) {
	int x = 50, y = Y, z = 45;
	int32_t* rec = place_jukebox(x, y, z);
	*rec = ITEM_RECORD_13;
	set_block(x, y, z, BLOCK_JUKEBOX, 1);

	reset_capture();
	struct block_data bd;
	struct block_info this_info = info_at(x, y, z, &bd);
	block_jukebox.onBreak(&s, &this_info);

	assert(g_spawn_item_count == 1);
	assert(g_last_spawn_item.payload.spawn_item.item.id == ITEM_RECORD_13);
	assert(jukebox_te_lookup(&s, x, y, z) == NULL); // TE freed

	printf("  jukebox: onBreak ejects the record then frees the TE OK\n");
}

// ============================================================================
// 16. onBreak on an EMPTY jukebox spawns nothing extra, still frees the TE.
// ============================================================================
static void test_jukebox_break_empty_no_extra_item(void) {
	int x = 51, y = Y, z = 45;
	place_jukebox(x, y, z);

	reset_capture();
	struct block_data bd;
	struct block_info this_info = info_at(x, y, z, &bd);
	block_jukebox.onBreak(&s, &this_info);

	assert(g_spawn_item_count == 0);
	assert(g_record_event_count == 0);
	assert(jukebox_te_lookup(&s, x, y, z) == NULL);

	printf("  jukebox: onBreak on an empty jukebox is a clean no-op OK\n");
}

// ============================================================================
// 17. Note block: real save/evict/reload round trip (note value + block
// survive).
// ============================================================================
static void test_noteblock_persistence_roundtrip(void) {
	int x = 60, y = Y, z = 60;
	int8_t* note = place_noteblock(x, y, z);
	*note = 9;

	w_coord_t cx = (w_coord_t)(x / 16), cz = (w_coord_t)(z / 16);
	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	assert(server_world_save_chunk(&s.worlds[0], true /* erase */, cx, cz));
	assert(noteblock_te_lookup(&s, x, y, z) == NULL); // evicted

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	int8_t* reloaded = noteblock_te_lookup(&s, x, y, z);
	assert(reloaded && *reloaded == 9);
	assert(get_type(x, y, z) == BLOCK_NOTEBLOCK);
	// The transient sidecar starts false again after a fresh chunk load
	// (Beta's previousRedstoneState is never persisted).
	bool* prev = noteblock_prev_powered_lookup(&s, x, y, z);
	assert(prev && *prev == false);

	printf("  note block: note value survives a real save/evict/reload "
		  "round trip OK\n");
}

// ============================================================================
// 18. Jukebox: real save/evict/reload round trip (record + meta survive).
// ============================================================================
static void test_jukebox_persistence_roundtrip(void) {
	int x = 65, y = Y, z = 60;
	int32_t* rec = place_jukebox(x, y, z);
	*rec = ITEM_RECORD_CAT;
	set_block(x, y, z, BLOCK_JUKEBOX, 1);

	w_coord_t cx = (w_coord_t)(x / 16), cz = (w_coord_t)(z / 16);
	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	assert(server_world_save_chunk(&s.worlds[0], true /* erase */, cx, cz));
	assert(jukebox_te_lookup(&s, x, y, z) == NULL); // evicted

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	int32_t* reloaded = jukebox_te_lookup(&s, x, y, z);
	assert(reloaded && *reloaded == ITEM_RECORD_CAT);
	assert(get_type(x, y, z) == BLOCK_JUKEBOX);
	assert(get_meta(x, y, z) == 1);

	printf("  jukebox: record + meta survive a real save/evict/reload round "
		  "trip OK\n");
}

// ============================================================================
// 19. D12 shape: cycling a note's pitch is a CONTENTS-ONLY edit (no block
// write -- the note value lives entirely in the TE, never in block_data).
// The chunk must stay !modified, yet the edit must still survive an evict/
// reload: the ONLY thing forcing that write is tile_entities_any() counting
// n_note. This is the slice's proof it obeyed the D12 predicate discipline
// (commit 8eae7a5).
// ============================================================================
static void test_noteblock_persistence_contents_only_edit(void) {
	int x = 70, y = Y, z = 61;
	place_noteblock(x, y, z); // the fresh pointer is stale after the reload
							  // below -- looked up again via `live`/`re`

	w_coord_t cx = (w_coord_t)(x / 16), cz = (w_coord_t)(z / 16);
	// Save #1 rides the block-place modified bit; the reload starts clean.
	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	assert(server_world_save_chunk(&s.worlds[0], true /* erase */, cx, cz));
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
	assert(!sc->modified && "fixture: a freshly reloaded chunk must be clean");

	int8_t* live = noteblock_te_lookup(&s, x, y, z);
	assert(live && *live == 0);

	// The contents-only edit: changePitch, NO block write. The chunk must
	// still be clean afterwards, or this test would be riding the modified
	// bit instead of the TE-kind predicate that D12 is about.
	*live = noteblock_change_pitch((uint8_t)*live);
	assert(*live == 1);
	assert(!sc->modified
		   && "fixture: a contents-only TE edit must not set c->modified");

	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	assert(server_world_save_chunk(&s.worlds[0], true /* erase */, cx, cz));
	assert(noteblock_te_lookup(&s, x, y, z) == NULL);
	assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	int8_t* re = noteblock_te_lookup(&s, x, y, z);
	assert(re);
	assert(*re == 1
		   && "D12 discipline: contents-only note edit lost on eviction");

	printf("  note block: D12 -- CONTENTS-ONLY edit (clean chunk) survives "
		  "evict/reload -- the shared TE-kind predicate covers notes\n");
}

int main(void) {
	test_fixture_dir("test_noteblock_jukebox_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	assert(svin_init());
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	// server_local_init's chest_pos/.../note_pos/record_player_pos convention
	// (all bytes 0xFF -> every int -1, "y < 0 == free"): this test builds
	// server_local by hand (like test_dispenser_env.c) rather than through
	// server_local_init, so these tables need the identical treatment or
	// noteblock_te_register/jukebox_te_register see every slot as "occupied
	// at (0,0,0)" and never find a free one.
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
					   true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 128;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	s.player.has_pos = true;
	assert(inventory_create(&s.player.inventory, &inventory_logic_player,
							&s, &s.player, INVENTORY_SIZE, 0, 0, 0));

	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	clin_test_set_rpc_observer(capture_rpc, NULL);

	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 5; cx++)
		for(int cz = 0; cz <= 5; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	printf("test_noteblock_jukebox_env:\n");
	test_changepitch_cycle();
	test_instrument_table();
	test_pitch_formula();
	test_jukebox_eject_offset_formula();
	test_noteblock_place_registers_te();
	test_noteblock_right_click_trigger();
	test_noteblock_covered_silence();
	test_noteblock_instrument_variety();
	test_noteblock_redstone_rising_edge();
	test_jukebox_place_registers_te();
	test_jukebox_insert_via_item_use();
	test_jukebox_insert_blocked_when_occupied();
	test_jukebox_eject_right_click();
	test_jukebox_eject_empty_is_unhandled();
	test_jukebox_break_ejects_then_frees();
	test_jukebox_break_empty_no_extra_item();
	test_noteblock_persistence_roundtrip();
	test_jukebox_persistence_roundtrip();
	test_noteblock_persistence_contents_only_edit();

	clin_test_set_rpc_observer(NULL, NULL);
	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_noteblock_jukebox_env\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}

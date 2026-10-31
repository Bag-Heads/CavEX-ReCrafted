/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

#include "tile_entities_nbt.h"
#include "../item/item_nbt.h"

#include <stdlib.h>
#include <string.h>

#include "../block/noteblock_logic.h" // NOTEBLOCK_NOTE_MIN/MAX (readFromNBT clamp)
#include "../cNBT/list.h" // INIT_LIST_HEAD, list_add_tail, list_for_each, list_entry

// Beta region TileEntities use 16-wide chunks and a 128-block-tall world. Keep
// this codec dependency-light: including chunk.h here creates a chunk/world
// include cycle in the devkitPPC translation unit.
#define TE_CHUNK_WIDTH 16
#define TE_WORLD_HEIGHT 128

// cNBT has no node-construction helpers, so we hand-link nbt_node + nbt_list
// structs (as region_archive.c already does). All the nodes/wrappers for one
// chunk's TileEntities live in a single malloc'd arena, freed in one shot after
// the NBT is dumped. The arena is over-sized to a safe upper bound (every chest
// assumed full) so the build can never outrun it -- no exact-count bookkeeping.

#define NODES_PER_CHEST (6 + TE_CHEST_SLOTS * 5) // compound+5 children + 54*(item compound+4)
#define WRAPS_PER_CHEST (8 + TE_CHEST_SLOTS * 6) // into-list+children-sentinel+5+Items-sentinel + 54*(into+sentinel+4)
#define NODES_PER_SIGN 9						 // compound + id,x,y,z,Text1..4
#define WRAPS_PER_SIGN 10						 // into-list + children-sentinel + 8
// furnace: compound + id,x,y,z,BurnTime,CookTime (6) + Items list (1) +
// 3*(item compound + Slot,id,Count,Damage) = 1+6+1+3*5 = 23 nodes; wraps =
// into-list + kids-sentinel + 6 field-links + Items-link + items-sentinel +
// 3*(into-items + item-kids-sentinel + 4 links) = 1+1+6+1+1+3*6 = 28.
#define NODES_PER_FURNACE (8 + TE_FURNACE_SLOTS * 5)
#define WRAPS_PER_FURNACE (10 + TE_FURNACE_SLOTS * 6)
// dispenser: compound + id,x,y,z (4) + Items list (1) + slots*(item compound +
// Slot,id,Count,Damage) = 1+4+1+slots*5 = 6+slots*5 nodes (same shape as
// chest -- no BurnTime/CookTime-equivalent scalar fields); wraps =
// into-list + kids-sentinel + 4 field-links + Items-link + items-sentinel +
// slots*6 = 8+slots*6.
#define NODES_PER_DISPENSER (6 + TE_DISPENSER_SLOTS * 5)
#define WRAPS_PER_DISPENSER (8 + TE_DISPENSER_SLOTS * 6)
// note: compound + id,x,y,z,note (5 fields) = 6 nodes (no list); wraps =
// into-list + kids-sentinel + 5 field-links = 7.
#define NODES_PER_NOTE 6
#define WRAPS_PER_NOTE 7
// record_player: compound + id,x,y,z (4) + optional Record (1) = sized for the
// record>0 case (Beta writes "Record" only when >0, so a record==0 instance
// uses fewer nodes -- the arena is a safe UPPER bound, matching every other
// kind's over-provisioning convention) = 6 nodes; wraps = into-list +
// kids-sentinel + up to 5 field-links = 7.
#define NODES_PER_RECORD_PLAYER 6
#define WRAPS_PER_RECORD_PLAYER 7
// spawner: compound + id,x,y,z,EntityId,Delay (6 fields) = 7 nodes (no list);
// wraps = into-list + kids-sentinel + 6 field-links = 8.
#define NODES_PER_SPAWNER 7
#define WRAPS_PER_SPAWNER 8
// piston: compound + id,x,y,z,blockId,blockData,facing,progress,extending = 1 + 9 = 10 nodes (no list);
// wraps = into-list + kids-sentinel + 9 field-links = 11.
#define NODES_PER_PISTON 10
#define WRAPS_PER_PISTON 11

// CR-04: scratch bytes for the 4 trimmed, NUL-terminated line strings each
// sign's Text1..Text4 nodes point at (te_sign.text is a packed 64-byte grid
// with no per-line NUL, so the writer needs real storage to hand cNBT actual
// C strings). Sized as a safe upper bound, same over-provisioning convention
// as NODES_PER_*/WRAPS_PER_* above: 4 lines, TE_SIGN_LINE_MAX chars + NUL
// each.
#define TEXT_BYTES_PER_SIGN (4 * (TE_SIGN_LINE_MAX + 1))

// One malloc holds the node array, then the wrapper array, then the sign-line
// text scratch; tile_entities_build_free frees this pointer.
struct te_arena {
	nbt_node* nodes;
	struct nbt_list* wraps;
	char* text;	   // CR-04: sign-line string scratch (see TEXT_BYTES_PER_SIGN)
	size_t ni, nw; // running cursors
	size_t nt;	   // running cursor into text
};

#ifdef CAVEWII_TILE_ENTITIES_NBT_TEST
static int te_test_allocations_before_failure = -1;

void tile_entities_nbt_test_fail_allocation_after(
	unsigned successful_allocations) {
	te_test_allocations_before_failure = (int)successful_allocations;
}

static bool te_test_allocation_should_fail(void) {
	if(te_test_allocations_before_failure < 0)
		return false;
	if(te_test_allocations_before_failure-- > 0)
		return false;
	te_test_allocations_before_failure = -1;
	return true;
}
#endif

static void* te_malloc(size_t bytes) {
#ifdef CAVEWII_TILE_ENTITIES_NBT_TEST
	if(te_test_allocation_should_fail())
		return NULL;
#endif
	return malloc(bytes);
}

static void* te_calloc(size_t count, size_t bytes) {
#ifdef CAVEWII_TILE_ENTITIES_NBT_TEST
	if(te_test_allocation_should_fail())
		return NULL;
#endif
	return calloc(count, bytes);
}

static nbt_node* node_alloc(struct te_arena* a) {
	return &a->nodes[a->ni++];
}
static struct nbt_list* wrap_alloc(struct te_arena* a) {
	return &a->wraps[a->nw++];
}
// CR-04: hand out `n` bytes of the arena's text scratch (see
// TEXT_BYTES_PER_SIGN); the caller fills and NUL-terminates it. Must come
// from the arena (not the stack) because the returned pointer is stored
// directly as an nbt_node's TAG_STRING payload and must outlive this
// function, same lifetime as the nodes/wraps arrays it sits beside.
static char* text_alloc(struct te_arena* a, size_t n) {
	char* p = &a->text[a->nt];
	a->nt += n;
	return p;
}

// Append `node` as a child of the compound/list whose sentinel is `parent`.
static void link_child(struct te_arena* a, struct nbt_list* parent,
					   nbt_node* node) {
	struct nbt_list* w = wrap_alloc(a);
	w->data = node;
	INIT_LIST_HEAD(&w->entry);
	list_add_tail(&w->entry, &parent->entry);
}

static nbt_node* make_int(struct te_arena* a, char* name, int32_t v) {
	nbt_node* n = node_alloc(a);
	n->type = TAG_INT;
	n->name = name;
	n->payload.tag_int = v;
	return n;
}
static nbt_node* make_str(struct te_arena* a, char* name, char* v) {
	nbt_node* n = node_alloc(a);
	n->type = TAG_STRING;
	n->name = name;
	n->payload.tag_string = v;
	return n;
}
static nbt_node* make_short(struct te_arena* a, char* name, int16_t v) {
	nbt_node* n = node_alloc(a);
	n->type = TAG_SHORT;
	n->name = name;
	n->payload.tag_short = v;
	return n;
}
static nbt_node* make_byte(struct te_arena* a, char* name, int8_t v) {
	nbt_node* n = node_alloc(a);
	n->type = TAG_BYTE;
	n->name = name;
	n->payload.tag_byte = v;
	return n;
}
static nbt_node* make_float(struct te_arena* a, char* name, float v) {
	nbt_node* n = node_alloc(a);
	n->type = TAG_FLOAT;
	n->name = name;
	n->payload.tag_float = v; // cNBT's DUMP_NUM byte-swaps this on a big-endian (Wii) dump
	return n;
}

// Emit one Beta {Slot:byte,id:short,Count:byte,Damage:short} ItemStack compound
// into the given Items-list sentinel. Shared by the furnace build loop (the chest
// loop predates this and stays inline).
static void emit_item_compound(struct te_arena* a, struct nbt_list* items_sentinel,
							   int slot, struct item_data it) {
	nbt_node* comp = node_alloc(a);
	if(it.nbt_record) {
		if(!item_nbt_emit(comp, &it, slot))
			*comp = (nbt_node){.type=TAG_RAW_COMPOUND,.payload.tag_byte_array.length=-1};
		link_child(a, items_sentinel, comp);
		return;
	}
	comp->type = TAG_COMPOUND;
	comp->name = NULL;
	struct nbt_list* ikids = wrap_alloc(a);
	ikids->data = NULL;
	INIT_LIST_HEAD(&ikids->entry);
	comp->payload.tag_compound = ikids;
	link_child(a, items_sentinel, comp);

	link_child(a, ikids, make_byte(a, "Slot", (int8_t)slot));
	link_child(a, ikids, make_short(a, "id", (int16_t)it.id));
	link_child(a, ikids, make_byte(a, "Count", (int8_t)it.count));
	link_child(a, ikids, make_short(a, "Damage", (int16_t)it.durability));
}

static int32_t te_chunk_from_block(int32_t coordinate) {
	return coordinate < 0
		? (coordinate + 1) / TE_CHUNK_WIDTH - 1
		: coordinate / TE_CHUNK_WIDTH;
}

static bool te_position_is_owned(int32_t x, int32_t y, int32_t z,
								 int32_t chunk_x, int32_t chunk_z) {
	return y >= 0 && y < TE_WORLD_HEIGHT && te_chunk_from_block(x) == chunk_x
		&& te_chunk_from_block(z) == chunk_z;
}

static bool te_piston_payload_is_valid(const struct te_piston_moving* pm) {
	return pm->stored_id >= 0 && pm->stored_id <= UINT8_MAX
		&& pm->stored_meta >= 0 && pm->stored_meta <= 0x0F
		&& pm->facing >= 0 && pm->facing <= 5
		&& (pm->extending == 0 || pm->extending == 1)
		// The ordered comparisons also reject NaN and infinities.
		&& pm->progress >= 0.0F && pm->progress <= 1.0F;
}

static bool te_size_add(size_t a, size_t b, size_t* out) {
	if(a > SIZE_MAX - b)
		return false;
	*out = a + b;
	return true;
}

static bool te_size_add_product(size_t* total, size_t count,
								size_t bytes_per_record) {
	if(count && bytes_per_record > SIZE_MAX / count)
		return false;
	return te_size_add(*total, count * bytes_per_record, total);
}

struct nbt_list* tile_entities_build(const struct chunk_tile_entities* te,
									 int32_t chunk_x, int32_t chunk_z,
									 void** arena_out) {
	if(!arena_out)
		return NULL;
	*arena_out = NULL;
	// Same every-kind predicate the save sites use (D12): "empty" must mean
	// the same thing to the serializer and to both writers, or a kind added to
	// one and not the other reopens the failed-build-committed-as-empty hole.
	if(!tile_entities_any(te))
		return NULL;
	/* IMP-TE-CAPS: no per-kind record cap (Beta's chunk TE map is unbounded);
	 * the total below is the one-chunk block-count bound. */
	if(te->n_chest < 0 || te->n_sign < 0 || te->n_furnace < 0
	   || te->n_dispenser < 0 || te->n_note < 0 || te->n_record_player < 0
	   || te->n_spawner < 0 || te->n_piston_moving < 0
	   || te->n_piston_moving > TE_PISTON_RECORDS_MAX
	   || (te->n_chest && !te->chests) || (te->n_sign && !te->signs)
	   || (te->n_furnace && !te->furnaces)
	   || (te->n_dispenser && !te->dispensers)
	   || (te->n_note && !te->notes)
	   || (te->n_record_player && !te->record_players)
	   || (te->n_spawner && !te->spawners)
	   || (te->n_piston_moving && !te->piston_movings))
		return NULL;
	size_t total_records = (size_t)te->n_chest + (size_t)te->n_sign
		+ (size_t)te->n_furnace + (size_t)te->n_dispenser
		+ (size_t)te->n_note + (size_t)te->n_record_player
		+ (size_t)te->n_spawner + (size_t)te->n_piston_moving;
	if(total_records > TE_RECORDS_EXAMINED_MAX)
		return NULL;
	for(int i = 0; i < te->n_chest; i++)
		if(!te_position_is_owned(te->chests[i].x, te->chests[i].y,
								 te->chests[i].z, chunk_x, chunk_z))
			return NULL;
	for(int i = 0; i < te->n_sign; i++)
		if(!te_position_is_owned(te->signs[i].x, te->signs[i].y,
								 te->signs[i].z, chunk_x, chunk_z)
		   || !memchr(te->signs[i].text, '\0', sizeof(te->signs[i].text)))
			return NULL;
	for(int i = 0; i < te->n_furnace; i++)
		if(!te_position_is_owned(te->furnaces[i].x, te->furnaces[i].y,
								 te->furnaces[i].z, chunk_x, chunk_z))
			return NULL;
	for(int i = 0; i < te->n_dispenser; i++)
		if(!te_position_is_owned(te->dispensers[i].x, te->dispensers[i].y,
								 te->dispensers[i].z, chunk_x, chunk_z))
			return NULL;
	for(int i = 0; i < te->n_note; i++)
		if(!te_position_is_owned(te->notes[i].x, te->notes[i].y,
								 te->notes[i].z, chunk_x, chunk_z))
			return NULL;
	for(int i = 0; i < te->n_record_player; i++)
		if(!te_position_is_owned(te->record_players[i].x,
								 te->record_players[i].y,
								 te->record_players[i].z, chunk_x, chunk_z))
			return NULL;
	for(int i = 0; i < te->n_spawner; i++)
		if(!te_position_is_owned(te->spawners[i].x, te->spawners[i].y,
								 te->spawners[i].z, chunk_x, chunk_z)
		   // mob_id must already be a valid NUL-terminated C string within
		   // its buffer -- the same "caller-owned data is well-formed before
		   // build" contract the sign codec's memchr check enforces.
		   || !memchr(te->spawners[i].mob_id, '\0',
					 sizeof(te->spawners[i].mob_id)))
			return NULL;
	for(int i = 0; i < te->n_piston_moving; i++)
		if(!te_position_is_owned(te->piston_movings[i].x,
								 te->piston_movings[i].y,
								 te->piston_movings[i].z, chunk_x, chunk_z)
		   || !te_piston_payload_is_valid(&te->piston_movings[i]))
			return NULL;

	size_t max_nodes = 3;
	size_t max_wraps = 1;
	if(!te_size_add_product(&max_nodes, (size_t)te->n_chest,
						NODES_PER_CHEST)
	   || !te_size_add_product(&max_nodes, (size_t)te->n_sign,
							NODES_PER_SIGN)
	   || !te_size_add_product(&max_nodes, (size_t)te->n_furnace,
							NODES_PER_FURNACE)
	   || !te_size_add_product(&max_nodes, (size_t)te->n_dispenser,
							NODES_PER_DISPENSER)
	   || !te_size_add_product(&max_nodes, (size_t)te->n_note,
							NODES_PER_NOTE)
	   || !te_size_add_product(&max_nodes, (size_t)te->n_record_player,
							NODES_PER_RECORD_PLAYER)
	   || !te_size_add_product(&max_nodes, (size_t)te->n_spawner,
							NODES_PER_SPAWNER)
	   || !te_size_add_product(&max_nodes, (size_t)te->n_piston_moving,
							NODES_PER_PISTON)
	   || !te_size_add_product(&max_wraps, (size_t)te->n_chest,
							WRAPS_PER_CHEST)
	   || !te_size_add_product(&max_wraps, (size_t)te->n_sign,
							WRAPS_PER_SIGN)
	   || !te_size_add_product(&max_wraps, (size_t)te->n_furnace,
							WRAPS_PER_FURNACE)
	   || !te_size_add_product(&max_wraps, (size_t)te->n_dispenser,
							WRAPS_PER_DISPENSER)
	   || !te_size_add_product(&max_wraps, (size_t)te->n_note,
							WRAPS_PER_NOTE)
	   || !te_size_add_product(&max_wraps, (size_t)te->n_record_player,
							WRAPS_PER_RECORD_PLAYER)
	   || !te_size_add_product(&max_wraps, (size_t)te->n_spawner,
							WRAPS_PER_SPAWNER)
	   || !te_size_add_product(&max_wraps, (size_t)te->n_piston_moving,
							WRAPS_PER_PISTON))
		return NULL;

	// CR-04: scratch bytes for each sign's 4 split-line strings (see
	// TEXT_BYTES_PER_SIGN); overflow-checked the same way as max_nodes/
	// max_wraps above.
	size_t max_text = 0;
	if(!te_size_add_product(&max_text, (size_t)te->n_sign,
						TEXT_BYTES_PER_SIGN))
		return NULL;

	// arena layout: [struct te_arena][nodes...][wraps...][sign-line text...]
	size_t bytes = sizeof(struct te_arena);
	if(!te_size_add_product(&bytes, max_nodes, sizeof(nbt_node))
	   || !te_size_add_product(&bytes, max_wraps, sizeof(struct nbt_list))
	   || !te_size_add(bytes, max_text, &bytes))
		return NULL;
	char* block = te_malloc(bytes);
	if(!block)
		return NULL;
	struct te_arena* a = (struct te_arena*)block;
	a->nodes = (nbt_node*)(block + sizeof(struct te_arena));
	a->wraps = (struct nbt_list*)((char*)a->nodes
								  + max_nodes * sizeof(nbt_node));
	a->text = (char*)a->wraps + max_wraps * sizeof(struct nbt_list);
	a->ni = 0;
	a->nw = 0;
	a->nt = 0;

	// Shared element-type node for every list sentinel (TileEntities + Items).
	nbt_node* compound_type = node_alloc(a);
	compound_type->type = TAG_COMPOUND;
	compound_type->name = NULL;
	// Beta NBTTagList.writeTagContents writes element-type byte 1 (TAG_BYTE) for any
	// EMPTY list, not the would-be element type. cNBT's empty-list dump falls back to
	// the sentinel's data->type, so an empty Items list must point at a TAG_BYTE node
	// to stay byte-faithful (a furnace/chest with no items is common). Shared, unlinked.
	nbt_node* byte_type = node_alloc(a);
	byte_type->type = TAG_BYTE;
	byte_type->name = NULL;

	struct nbt_list* te_sentinel = wrap_alloc(a);
	te_sentinel->data = compound_type;
	INIT_LIST_HEAD(&te_sentinel->entry);

	for(int c = 0; c < te->n_chest; c++) {
		const struct te_chest* ch = &te->chests[c];
		nbt_node* comp = node_alloc(a);
		comp->type = TAG_COMPOUND;
		comp->name = NULL;
		struct nbt_list* kids = wrap_alloc(a);
		kids->data = NULL;
		INIT_LIST_HEAD(&kids->entry);
		comp->payload.tag_compound = kids;
		link_child(a, te_sentinel, comp);

		link_child(a, kids, make_str(a, "id", "Chest"));
		link_child(a, kids, make_int(a, "x", ch->x));
		link_child(a, kids, make_int(a, "y", ch->y));
		link_child(a, kids, make_int(a, "z", ch->z));

		nbt_node* items = node_alloc(a);
		items->type = TAG_LIST;
		items->name = "Items";
		struct nbt_list* items_sentinel = wrap_alloc(a);
		items_sentinel->data = compound_type;
		INIT_LIST_HEAD(&items_sentinel->entry);
		items->payload.tag_list = items_sentinel;
		link_child(a, kids, items);

		int n_items = 0;
		for(int s = 0; s < TE_CHEST_SLOTS; s++) {
			if(!ch->items[s].id)
				continue;
			nbt_node* it = node_alloc(a);
			if(ch->items[s].nbt_record) {
				if(!item_nbt_emit(it, &ch->items[s], s))
					*it = (nbt_node){.type=TAG_RAW_COMPOUND,.payload.tag_byte_array.length=-1};
				link_child(a,items_sentinel,it);
				n_items++;
				continue;
			}
			it->type = TAG_COMPOUND;
			it->name = NULL;
			struct nbt_list* ikids = wrap_alloc(a);
			ikids->data = NULL;
			INIT_LIST_HEAD(&ikids->entry);
			it->payload.tag_compound = ikids;
			link_child(a, items_sentinel, it);

			nbt_node* slot = node_alloc(a);
			slot->type = TAG_BYTE;
			slot->name = "Slot";
			slot->payload.tag_byte = (int8_t)s;
			link_child(a, ikids, slot);
			nbt_node* iid = node_alloc(a);
			iid->type = TAG_SHORT;
			iid->name = "id";
			iid->payload.tag_short = (int16_t)ch->items[s].id;
			link_child(a, ikids, iid);
			nbt_node* cnt = node_alloc(a);
			cnt->type = TAG_BYTE;
			cnt->name = "Count";
			cnt->payload.tag_byte = (int8_t)ch->items[s].count;
			link_child(a, ikids, cnt);
			nbt_node* dmg = node_alloc(a);
			dmg->type = TAG_SHORT;
			dmg->name = "Damage";
			dmg->payload.tag_short = (int16_t)ch->items[s].durability;
			link_child(a, ikids, dmg);
			n_items++;
		}
		if(!n_items)
			items_sentinel->data = byte_type; // Beta: empty list element-type == 1
	}

	static const char* const tnames[4] = {"Text1", "Text2", "Text3", "Text4"};
	for(int g = 0; g < te->n_sign; g++) {
		const struct te_sign* sg = &te->signs[g];
		nbt_node* comp = node_alloc(a);
		comp->type = TAG_COMPOUND;
		comp->name = NULL;
		struct nbt_list* kids = wrap_alloc(a);
		kids->data = NULL;
		INIT_LIST_HEAD(&kids->entry);
		comp->payload.tag_compound = kids;
		link_child(a, te_sentinel, comp);

		link_child(a, kids, make_str(a, "id", "Sign"));
		link_child(a, kids, make_int(a, "x", sg->x));
		link_child(a, kids, make_int(a, "y", sg->y));
		link_child(a, kids, make_int(a, "z", sg->z));
		// CR-04: Beta TileEntitySign.writeToNBT [740c583] writes each of the 4
		// signText[] entries verbatim as Text1..Text4. This port's live sign
		// buffer packs those 4 lines into one TE_SIGN_SIZE-byte grid (see the
		// TE_SIGN_LINE_COLS/TE_SIGN_LINE_MAX comment on tile_entities_nbt.h);
		// split it back into 4 trimmed, NUL-terminated strings so the saved
		// file matches Beta's 4-tag layout instead of dumping the whole
		// buffer into Text1 with Text2-4 blank.
		for(int k = 0; k < 4; k++) {
			char* line = text_alloc(a, TE_SIGN_LINE_MAX + 1);
			// sg->text has no per-line NUL (it is a fixed-width, right-space-
			// padded grid by construction -- server_local's sign_texts /
			// block_sign.c's seed both always space-fill it); trim trailing
			// spaces the same way server_remote.c's remote_flush_sign_edit
			// does for the multiplayer wire codec's equivalent conversion.
			size_t len = TE_SIGN_LINE_COLS;
			const char* col = &sg->text[k * TE_SIGN_LINE_COLS];
			while(len > 0 && col[len - 1] == ' ')
				len--;
			// Defensive: keep Beta's 15-char cap even if the grid invariant
			// (column 15 always blank) is ever violated by a corrupt/legacy
			// buffer -- never emit a line cNBT would have to reject.
			if(len > TE_SIGN_LINE_MAX)
				len = TE_SIGN_LINE_MAX;
			memcpy(line, col, len);
			line[len] = '\0';
			link_child(a, kids, make_str(a, (char*)tnames[k], line));
		}
	}

	for(int fi = 0; fi < te->n_furnace; fi++) {
		const struct te_furnace* fr = &te->furnaces[fi];
		nbt_node* comp = node_alloc(a);
		comp->type = TAG_COMPOUND;
		comp->name = NULL;
		struct nbt_list* kids = wrap_alloc(a);
		kids->data = NULL;
		INIT_LIST_HEAD(&kids->entry);
		comp->payload.tag_compound = kids;
		link_child(a, te_sentinel, comp);

		link_child(a, kids, make_str(a, "id", "Furnace"));
		link_child(a, kids, make_int(a, "x", fr->x));
		link_child(a, kids, make_int(a, "y", fr->y));
		link_child(a, kids, make_int(a, "z", fr->z));
		link_child(a, kids, make_short(a, "BurnTime", fr->burn_time));
		link_child(a, kids, make_short(a, "CookTime", fr->cook_time));

		nbt_node* items = node_alloc(a);
		items->type = TAG_LIST;
		items->name = "Items";
		struct nbt_list* items_sentinel = wrap_alloc(a);
		items_sentinel->data = compound_type;
		INIT_LIST_HEAD(&items_sentinel->entry);
		items->payload.tag_list = items_sentinel;
		link_child(a, kids, items);

		int n_items = 0;
		for(int s = 0; s < TE_FURNACE_SLOTS; s++)
			if(fr->items[s].id) {
				emit_item_compound(a, items_sentinel, s, fr->items[s]);
				n_items++;
			}
		if(!n_items)
			items_sentinel->data = byte_type; // Beta: empty list element-type == 1
	}

	for(int di = 0; di < te->n_dispenser; di++) {
		const struct te_dispenser* dp = &te->dispensers[di];
		nbt_node* comp = node_alloc(a);
		comp->type = TAG_COMPOUND;
		comp->name = NULL;
		struct nbt_list* kids = wrap_alloc(a);
		kids->data = NULL;
		INIT_LIST_HEAD(&kids->entry);
		comp->payload.tag_compound = kids;
		link_child(a, te_sentinel, comp);

		// Beta TileEntity.writeToNBT id string for TileEntityDispenser is
		// "Trap", not "Dispenser" [decompile 740c583: TileEntity.<clinit>
		// addMapping(TileEntityDispenser.class, "Trap")].
		link_child(a, kids, make_str(a, "id", "Trap"));
		link_child(a, kids, make_int(a, "x", dp->x));
		link_child(a, kids, make_int(a, "y", dp->y));
		link_child(a, kids, make_int(a, "z", dp->z));

		nbt_node* items = node_alloc(a);
		items->type = TAG_LIST;
		items->name = "Items";
		struct nbt_list* items_sentinel = wrap_alloc(a);
		items_sentinel->data = compound_type;
		INIT_LIST_HEAD(&items_sentinel->entry);
		items->payload.tag_list = items_sentinel;
		link_child(a, kids, items);

		int n_items = 0;
		for(int s = 0; s < TE_DISPENSER_SLOTS; s++)
			if(dp->items[s].id) {
				emit_item_compound(a, items_sentinel, s, dp->items[s]);
				n_items++;
			}
		if(!n_items)
			items_sentinel->data = byte_type; // Beta: empty list element-type == 1
	}

	for(int ni = 0; ni < te->n_note; ni++) {
		const struct te_note* nt = &te->notes[ni];
		nbt_node* comp = node_alloc(a);
		comp->type = TAG_COMPOUND;
		comp->name = NULL;
		struct nbt_list* kids = wrap_alloc(a);
		kids->data = NULL;
		INIT_LIST_HEAD(&kids->entry);
		comp->payload.tag_compound = kids;
		link_child(a, te_sentinel, comp);

		// Beta TileEntity.writeToNBT id string for TileEntityNote is "Music"
		// [decompile 740c583: TileEntity.<clinit>
		// addMapping(TileEntityNote.class, "Music")].
		link_child(a, kids, make_str(a, "id", "Music"));
		link_child(a, kids, make_int(a, "x", nt->x));
		link_child(a, kids, make_int(a, "y", nt->y));
		link_child(a, kids, make_int(a, "z", nt->z));
		link_child(a, kids, make_byte(a, "note", nt->note));
	}

	for(int rp = 0; rp < te->n_record_player; rp++) {
		const struct te_record_player* rec = &te->record_players[rp];
		nbt_node* comp = node_alloc(a);
		comp->type = TAG_COMPOUND;
		comp->name = NULL;
		struct nbt_list* kids = wrap_alloc(a);
		kids->data = NULL;
		INIT_LIST_HEAD(&kids->entry);
		comp->payload.tag_compound = kids;
		link_child(a, te_sentinel, comp);

		// Beta TileEntity.writeToNBT id string for TileEntityRecordPlayer is
		// "RecordPlayer" [decompile 740c583: TileEntity.<clinit>].
		link_child(a, kids, make_str(a, "id", "RecordPlayer"));
		link_child(a, kids, make_int(a, "x", rec->x));
		link_child(a, kids, make_int(a, "y", rec->y));
		link_child(a, kids, make_int(a, "z", rec->z));
		// Beta TileEntityRecordPlayer.writeToNBT: "Record" written ONLY when
		// record > 0 -- replicate the conditional exactly for save-compat.
		if(rec->record > 0)
			link_child(a, kids, make_int(a, "Record", rec->record));
	}

	for(int si = 0; si < te->n_spawner; si++) {
		const struct te_spawner* sp = &te->spawners[si];
		nbt_node* comp = node_alloc(a);
		comp->type = TAG_COMPOUND;
		comp->name = NULL;
		struct nbt_list* kids = wrap_alloc(a);
		kids->data = NULL;
		INIT_LIST_HEAD(&kids->entry);
		comp->payload.tag_compound = kids;
		link_child(a, te_sentinel, comp);

		// Beta TileEntity.writeToNBT id string for TileEntityMobSpawner is
		// "MobSpawner" [decompile 740c583: TileEntity.<clinit>].
		link_child(a, kids, make_str(a, "id", "MobSpawner"));
		link_child(a, kids, make_int(a, "x", sp->x));
		link_child(a, kids, make_int(a, "y", sp->y));
		link_child(a, kids, make_int(a, "z", sp->z));
		// sp->mob_id is caller-owned and outlives this dump (same lifetime
		// contract entity_archive.c's add_string relies on for e.g.
		// wolf_owner) -- aliased directly, no arena text-scratch copy needed
		// (unlike te_sign's text, this field needs no transform before
		// writing).
		link_child(a, kids, make_str(a, "EntityId", (char*)sp->mob_id));
		link_child(a, kids, make_short(a, "Delay", sp->delay));
	}

	for(int pi = 0; pi < te->n_piston_moving; pi++) {
		const struct te_piston_moving* pm = &te->piston_movings[pi];
		nbt_node* comp = node_alloc(a);
		comp->type = TAG_COMPOUND;
		comp->name = NULL;
		struct nbt_list* kids = wrap_alloc(a);
		kids->data = NULL;
		INIT_LIST_HEAD(&kids->entry);
		comp->payload.tag_compound = kids;
		link_child(a, te_sentinel, comp);

		// Beta TileEntityPiston.writeToNBT field order/types.
		link_child(a, kids, make_str(a, "id", "Piston"));
		link_child(a, kids, make_int(a, "x", pm->x));
		link_child(a, kids, make_int(a, "y", pm->y));
		link_child(a, kids, make_int(a, "z", pm->z));
		link_child(a, kids, make_int(a, "blockId", pm->stored_id));
		link_child(a, kids, make_int(a, "blockData", pm->stored_meta));
		link_child(a, kids, make_int(a, "facing", pm->facing));
		link_child(a, kids, make_float(a, "progress", pm->progress));
		link_child(a, kids, make_byte(a, "extending", (int8_t)(pm->extending ? 1 : 0)));
	}

	*arena_out = block;
	return te_sentinel;
}

void tile_entities_build_free(void* arena) {
	struct te_arena* a = arena;
	if(a) for(size_t k=0;k<a->ni;k++)
		if(a->nodes[k].type == TAG_RAW_COMPOUND)
			free(a->nodes[k].payload.tag_byte_array.data);
	free(arena);
}

// --- parse ----------------------------------------------------------------

// Direct-child lookup (NOT recursive): a chest compound and its item compounds
// both carry an "id" child, so a recursive search would alias them.
static nbt_node* find_child(const nbt_node* comp, const char* name) {
	if(!comp || comp->type != TAG_COMPOUND || !comp->payload.tag_compound)
		return NULL;
	struct list_head* pos;
	list_for_each(pos, &comp->payload.tag_compound->entry) {
		struct nbt_list* e = list_entry(pos, struct nbt_list, entry);
		nbt_node* n = e->data;
		if(n && n->name && strcmp(n->name, name) == 0)
			return n;
	}
	return NULL;
}

static bool child_int(const nbt_node* comp, const char* name, int32_t* out) {
	nbt_node* n = find_child(comp, name);
	if(!n || n->type != TAG_INT)
		return false;
	*out = n->payload.tag_int;
	return true;
}

static bool child_short(const nbt_node* comp, const char* name, int16_t* out) {
	nbt_node* n = find_child(comp, name);
	if(!n || n->type != TAG_SHORT)
		return false;
	*out = n->payload.tag_short;
	return true;
}

static bool child_float(const nbt_node* comp, const char* name, float* out) {
	nbt_node* n = find_child(comp, name);
	if(!n || n->type != TAG_FLOAT)
		return false;
	*out = n->payload.tag_float;
	return true;
}

static bool child_byte(const nbt_node* comp, const char* name, int8_t* out) {
	nbt_node* n = find_child(comp, name);
	if(!n || n->type != TAG_BYTE)
		return false;
	*out = n->payload.tag_byte;
	return true;
}

static bool same_owner(const nbt_node* a, const nbt_node* b) {
	int32_t ax,ay,az,bx,by,bz;
	nbt_node* ai=find_child(a,"id"), *bi=find_child(b,"id");
	return ai && bi && ai->type==TAG_STRING && bi->type==TAG_STRING
		&& !strcmp(ai->payload.tag_string,bi->payload.tag_string)
		&& child_int(a,"x",&ax) && child_int(a,"y",&ay) && child_int(a,"z",&az)
		&& child_int(b,"x",&bx) && child_int(b,"y",&by) && child_int(b,"z",&bz)
		&& ax==bx && ay==by && az==bz;
}

static const struct item_data* owner_items(const nbt_node* n,
	const struct chunk_tile_entities* te, size_t* count) {
	int32_t x,y,z;
	if(!te || !child_int(n,"x",&x) || !child_int(n,"y",&y) || !child_int(n,"z",&z)) return NULL;
#define FIND_ITEMS(field, number, slots) \
	for(int k=0;k<te->number;k++) if(te->field[k].x==x && te->field[k].y==y && te->field[k].z==z) { \
		*count=slots; return te->field[k].items; }
	FIND_ITEMS(chests,n_chest,TE_CHEST_SLOTS)
	FIND_ITEMS(furnaces,n_furnace,TE_FURNACE_SLOTS)
	FIND_ITEMS(dispensers,n_dispenser,TE_DISPENSER_SLOTS)
#undef FIND_ITEMS
	return NULL;
}

static bool te_append_owned(struct nbt_list* list, nbt_node* node) {
	struct nbt_list* w=malloc(sizeof(*w));
	if(!node || !w) { free(w); nbt_free(node); return false; }
	w->data=node; list_add_tail(&w->entry,&list->entry); return true;
}

static bool te_native_deletion_safe(const nbt_node* old,int32_t x,int32_t z) {
	nbt_node type={.type=TAG_COMPOUND};
	struct nbt_list list={.data=&type},entry={.data=(nbt_node*)old};
	INIT_LIST_HEAD(&list.entry);list_add_tail(&entry.entry,&list.entry);
	nbt_node root={.type=TAG_LIST,.payload.tag_list=&list};
	struct chunk_tile_entities decoded={0};
	if(!tile_entities_parse(&root,x,z,&decoded)) return false;
	void* arena=NULL;
	struct nbt_list* generated=tile_entities_build(&decoded,x,z,&arena);
	nbt_node* exact=item_nbt_clone_lossless(old);
	bool same=generated && exact && !list_empty(&generated->entry)
		&& item_nbt_equal(exact,list_entry(generated->entry.flink,struct nbt_list,entry)->data);
	nbt_free(exact);tile_entities_build_free(arena);tile_entities_parse_free(&decoded);
	return same;
}

nbt_node* tile_entities_preserve(const nbt_node* original,
	const nbt_node* generated, const struct chunk_tile_entities* current, bool native, int32_t x, int32_t z) {
	if(!original) return nbt_clone((nbt_node*)generated);
	if(original->type!=TAG_LIST || generated->type!=TAG_LIST) return NULL;
	if(native && current) {
		/* Duplicate legacy native rows have no unique owner key. Regenerate
		 * only after proving the codec accounts for every byte in every row. */
		bool complete=true;
		struct list_head* p;
		list_for_each(p,&original->payload.tag_list->entry)
			if(!te_native_deletion_safe(list_entry(p,struct nbt_list,entry)->data,x,z)) {
				complete=false;break;
			}
		/* save-enc-1030: an empty list that stays empty keeps its stored bytes
		 * verbatim (an unchanged chunk is never re-encoded); only a list that
		 * changes takes the Beta empty-list type 1. */
		if(complete) return nbt_clone((nbt_node*)(list_empty(&original->payload.tag_list->entry)
			&& list_empty(&generated->payload.tag_list->entry) ? original : generated));
	}
	nbt_node* out=nbt_clone((nbt_node*)original);
	if(!out) return NULL;
	struct list_head *p,*next;
	list_for_each_safe(p,next,&out->payload.tag_list->entry) {
		struct nbt_list* w=list_entry(p,struct nbt_list,entry);
		nbt_node* candidate=NULL;
		struct list_head* q;
		list_for_each(q,&generated->payload.tag_list->entry) {
			nbt_node* n=list_entry(q,struct nbt_list,entry)->data;
			if(same_owner(w->data,n)) { if(candidate) goto fail; candidate=n; }
		}
		if(!candidate) {
			const nbt_node* id=find_child(w->data,"id");
			static const char* const known[]={"Chest","Sign","Furnace","Trap", "Music","RecordPlayer","MobSpawner","Piston"};
			bool recognized=false;
			if(id && id->type==TAG_STRING && id->payload.tag_string)
				for(size_t k=0;k<sizeof(known)/sizeof(*known);k++)
					if(!strcmp(id->payload.tag_string,known[k])) recognized=true;
			if(native && current && current->legacy_flat_complete && recognized) {
				if(!te_native_deletion_safe(w->data,x,z)) goto fail;
				list_del(p); nbt_free(w->data); free(w); continue;
			}
			/* Unrecognized imported owners are retained. An inventory owner
			 * cannot disappear without a lossless item transfer proof.
			 * IMP-TE-CAPS: an owner whose Items list is EMPTY has nothing
			 * to transfer -- e.g. a Beta Furnace record on a non-container
			 * block, which Beta's own load drops [740c583
			 * Chunk.setChunkBlockTileEntity] and the orphan sweep frees --
			 * so it is retained like any other unowned record instead of
			 * refusing the chunk save forever. */
			const nbt_node* items=find_child(w->data,"Items");
			if(items && !(items->type==TAG_LIST && items->payload.tag_list
					&& list_empty(&items->payload.tag_list->entry)))
				goto fail;
			continue;
		}
		size_t count=0;
		const struct item_data* items=owner_items(candidate,current,&count);
		list_for_each(q,&candidate->payload.tag_compound->entry) {
			nbt_node* n=list_entry(q,struct nbt_list,entry)->data;
			if(!n->name) goto fail;
			nbt_node* replacement;
			if(items && !strcmp(n->name,"Items"))
				replacement=item_nbt_write_list(find_child(w->data,"Items"),items,count,false);
			else replacement=nbt_clone(n);
			if(!item_nbt_replace(w->data,n->name,replacement)) goto fail;
		}
	}
	list_for_each(p,&generated->payload.tag_list->entry) {
		nbt_node* n=list_entry(p,struct nbt_list,entry)->data;
		bool found=false; struct list_head* q;
		list_for_each(q,&original->payload.tag_list->entry)
			if(same_owner(n,list_entry(q,struct nbt_list,entry)->data)) found=true;
		if(!found && !te_append_owned(out->payload.tag_list,nbt_clone(n))) goto fail;
	}
	/* save-enc-1030: an EMPTY list (every row departed) is written with element
	 * type 1, as Beta NBTTagList.writeTagContents does for every empty list
	 * [McBetaCpp/srcorigjava NBTTagList.java:25-31]. */
	if(!list_empty(&out->payload.tag_list->entry)) out->payload.tag_list->data->type=TAG_COMPOUND;
	else if(out->payload.tag_list->data
		&& !list_empty(&original->payload.tag_list->entry)) out->payload.tag_list->data->type=TAG_BYTE;
	return out;
fail:
	nbt_free(out); return NULL;
}

static bool child_string(const nbt_node* comp, const char* name,
						 const char** out) {
	nbt_node* n = find_child(comp, name);
	if(!n || n->type != TAG_STRING || !n->payload.tag_string)
		return false;
	*out = n->payload.tag_string;
	return true;
}

// 2026-09-01 fix: how many bytes of `s` to keep so the copy holds at most
// `max_chars` CODEPOINTS and never more than `max_bytes` bytes, WITHOUT ever
// splitting a multibyte UTF-8 (cNBT's modified-UTF-8 decode) sequence in
// half. Beta's own import clamp [740c583 TileEntitySign.readFromNBT:
// `signText[i].substring(0, 15)`] cuts by java char COUNT -- 15 CHARACTERS,
// which for any line holding a non-ASCII (2/3-byte UTF-8) character is NOT
// the same cut point as 15 BYTES. The old code clamped strlen() straight to
// TE_SIGN_LINE_MAX(15) bytes: for a line whose 15th byte lands inside a
// multibyte sequence, that left a truncated, invalid partial sequence's
// leading byte(s) in the stored/rendered text -- corrupted trailing bytes,
// not merely "one character short". This walks one whole UTF-8 sequence at
// a time (a byte is the START of a new sequence when it is NOT a
// continuation byte, i.e. (b & 0xC0) != 0x80) and stops BEFORE any sequence
// that would cross `max_bytes` or the `max_chars` codepoint count, so the
// returned length is always a whole number of codepoints. Malformed input
// (a stray continuation byte with no lead byte) degrades to treating that
// byte as its own one-byte "codepoint" -- never reads past the NUL
// terminator, never worse than the pre-fix behaviour for plain ASCII.
static size_t sign_line_clamp_len(const char* s, size_t max_bytes,
								  size_t max_chars) {
	size_t bytes = 0;
	size_t chars = 0;
	while(s[bytes] != '\0' && chars < max_chars) {
		size_t seq_start = bytes;
		bytes++; // the lead byte of this codepoint always counts
		while(((unsigned char)s[bytes] & 0xC0) == 0x80)
			bytes++; // continuation bytes belong to the same codepoint
		if(bytes > max_bytes) {
			bytes = seq_start; // this whole codepoint doesn't fit -- stop before it
			break;
		}
		chars++;
	}
	return bytes;
}

// Parse a Beta Items TAG_LIST into out_items[0..n_slots), keyed by each entry's
// "Slot" byte (entries with an out-of-range slot are skipped). The examined
// cap bounds malformed/duplicate-heavy lists independently for every TE.
static bool parse_items(const nbt_node* items, struct item_data* out_items,
						int n_slots) {
	if(items && items->type==TAG_LIST && items->payload.tag_list) {
		size_t examined=0; struct list_head* p;
		list_for_each(p,&items->payload.tag_list->entry)
			if(examined++>=TE_ITEM_RECORDS_EXAMINED_MAX) return false;
	}
	return item_nbt_read_list(items, out_items, (size_t)n_slots, false);
}

bool tile_entities_parse(const nbt_node* te_list, int32_t chunk_x,
						 int32_t chunk_z,
						 struct chunk_tile_entities* out) {
	if(!out)
		return false;
	memset(out, 0, sizeof(*out));
	if(!te_list || te_list->type != TAG_LIST || !te_list->payload.tag_list)
		return true;

	/* Pass 1: count supported records into exact-sized arrays. IMP-TE-CAPS:
	 * no per-kind cap (a real Beta chunk held 384 note blocks); the total-
	 * examined cap (one chunk's block count) bounds every list, including
	 * lists made entirely of unknown records. */
	int nc = 0, ns = 0, nf = 0, nd = 0, nn = 0, nrp = 0, nsp = 0, np = 0;
	size_t examined = 0;
	struct list_head* pos;
	list_for_each(pos, &te_list->payload.tag_list->entry) {
		if(examined++ >= TE_RECORDS_EXAMINED_MAX)
			return false;
		nbt_node* comp = list_entry(pos, struct nbt_list, entry)->data;
		nbt_node* id = find_child(comp, "id");
		if(!id || id->type != TAG_STRING || !id->payload.tag_string)
			continue;
		if(!strcmp(id->payload.tag_string, "Chest")) {
			nc++;
		} else if(!strcmp(id->payload.tag_string, "Sign")) {
			ns++;
		} else if(!strcmp(id->payload.tag_string, "Furnace")) {
			nf++;
		} else if(!strcmp(id->payload.tag_string, "Trap")) {
			// "Trap" is Beta's TileEntities id string for the dispenser
			// [decompile 740c583: TileEntity.<clinit>].
			nd++;
		} else if(!strcmp(id->payload.tag_string, "Music")) {
			// "Music" is Beta's TileEntities id string for the note block
			// [decompile 740c583: TileEntity.<clinit>].
			nn++;
		} else if(!strcmp(id->payload.tag_string, "RecordPlayer")) {
			nrp++;
		} else if(!strcmp(id->payload.tag_string, "MobSpawner")) {
			nsp++;
		} else if(!strcmp(id->payload.tag_string, "Piston")) {
			if(np >= TE_PISTON_RECORDS_MAX)
				return false;
			np++;
		}
	}

	struct chunk_tile_entities parsed = {0};
	if(nc) {
		parsed.chests = te_calloc((size_t)nc, sizeof(struct te_chest));
		if(!parsed.chests)
			goto fail;
	}
	if(ns) {
		parsed.signs = te_calloc((size_t)ns, sizeof(struct te_sign));
		if(!parsed.signs)
			goto fail;
	}
	if(nf) {
		parsed.furnaces = te_calloc((size_t)nf, sizeof(struct te_furnace));
		if(!parsed.furnaces)
			goto fail;
	}
	if(nd) {
		parsed.dispensers
			= te_calloc((size_t)nd, sizeof(struct te_dispenser));
		if(!parsed.dispensers)
			goto fail;
	}
	if(nn) {
		parsed.notes = te_calloc((size_t)nn, sizeof(struct te_note));
		if(!parsed.notes)
			goto fail;
	}
	if(nrp) {
		parsed.record_players
			= te_calloc((size_t)nrp, sizeof(struct te_record_player));
		if(!parsed.record_players)
			goto fail;
	}
	if(nsp) {
		parsed.spawners = te_calloc((size_t)nsp, sizeof(struct te_spawner));
		if(!parsed.spawners)
			goto fail;
	}
	if(np) {
		parsed.piston_movings
			= te_calloc((size_t)np, sizeof(struct te_piston_moving));
		if(!parsed.piston_movings)
			goto fail;
	}

	// Pass 2: fill.
	examined = 0;
	list_for_each(pos, &te_list->payload.tag_list->entry) {
		if(examined++ >= TE_RECORDS_EXAMINED_MAX)
			goto fail;
		nbt_node* comp = list_entry(pos, struct nbt_list, entry)->data;
		nbt_node* id = find_child(comp, "id");
		if(!id || id->type != TAG_STRING || !id->payload.tag_string)
			continue;

		if(!strcmp(id->payload.tag_string, "Chest") && parsed.chests) {
			struct te_chest* ch = &parsed.chests[parsed.n_chest++];
			nbt_node* items = find_child(comp, "Items");
			if(!child_int(comp, "x", &ch->x)
			   || !child_int(comp, "y", &ch->y)
			   || !child_int(comp, "z", &ch->z) || !items
			   || !parse_items(items, ch->items, TE_CHEST_SLOTS)
			   || !te_position_is_owned(ch->x, ch->y, ch->z, chunk_x,
									chunk_z))
				goto fail;
		} else if(!strcmp(id->payload.tag_string, "Sign") && parsed.signs) {
			struct te_sign* sg = &parsed.signs[parsed.n_sign++];
			const char* text[4];
			if(!child_int(comp, "x", &sg->x)
			   || !child_int(comp, "y", &sg->y)
			   || !child_int(comp, "z", &sg->z)
			   || !child_string(comp, "Text1", &text[0])
			   || !child_string(comp, "Text2", &text[1])
			   || !child_string(comp, "Text3", &text[2])
			   || !child_string(comp, "Text4", &text[3])
			   || !te_position_is_owned(sg->x, sg->y, sg->z, chunk_x,
									chunk_z))
				goto fail;
			// CR-04: reassemble the 4 imported Beta lines into the packed
			// grid te_sign.text holds internally (see the TE_SIGN_LINE_COLS/
			// TE_SIGN_LINE_MAX comment on tile_entities_nbt.h). Each line is
			// clamped to TE_SIGN_LINE_MAX(15) CHARACTERS, matching Beta's own
			// import clamp verbatim [740c583 TileEntitySign.readFromNBT]:
			// `if (signText[i].length() > 15) signText[i] =
			// signText[i].substring(0, 15);` -- a hard character-count cut,
			// not a trim, so this takes the first 15 CHARACTERS of whatever
			// the tag holds -- NOT the first 15 BYTES (2026-09-01 fix: see
			// sign_line_clamp_len; a raw strlen()-then-byte-clamp can split a
			// multibyte UTF-8 sequence in half for any line holding a
			// non-ASCII character, corrupting the stored trailing bytes
			// instead of merely under-counting). sg->text is te_calloc'd
			// (all zero), so bytes past TE_SIGN_SIZE stay the terminator;
			// each line's unused columns are explicitly space-filled to
			// match the live grid's own always-blank padding (block_sign.c /
			// server_local.c seed new signs the same way).
			for(int k = 0; k < 4; k++) {
				size_t len = sign_line_clamp_len(text[k], TE_SIGN_LINE_MAX,
												 TE_SIGN_LINE_MAX);
				char* col = &sg->text[k * TE_SIGN_LINE_COLS];
				memcpy(col, text[k], len);
				memset(col + len, ' ', TE_SIGN_LINE_COLS - len);
			}
		} else if(!strcmp(id->payload.tag_string, "Furnace") && parsed.furnaces) {
			struct te_furnace* fr = &parsed.furnaces[parsed.n_furnace++];
			nbt_node* items = find_child(comp, "Items");
			if(!child_int(comp, "x", &fr->x)
			   || !child_int(comp, "y", &fr->y)
			   || !child_int(comp, "z", &fr->z)
			   || !child_short(comp, "BurnTime", &fr->burn_time)
			   || !child_short(comp, "CookTime", &fr->cook_time) || !items
			   || !parse_items(items, fr->items, TE_FURNACE_SLOTS)
			   || !te_position_is_owned(fr->x, fr->y, fr->z, chunk_x,
									chunk_z))
				goto fail;
		} else if(!strcmp(id->payload.tag_string, "Trap") && parsed.dispensers) {
			struct te_dispenser* dp = &parsed.dispensers[parsed.n_dispenser++];
			nbt_node* items = find_child(comp, "Items");
			if(!child_int(comp, "x", &dp->x)
			   || !child_int(comp, "y", &dp->y)
			   || !child_int(comp, "z", &dp->z) || !items
			   || !parse_items(items, dp->items, TE_DISPENSER_SLOTS)
			   || !te_position_is_owned(dp->x, dp->y, dp->z, chunk_x,
									chunk_z))
				goto fail;
		} else if(!strcmp(id->payload.tag_string, "Music") && parsed.notes) {
			struct te_note* nt = &parsed.notes[parsed.n_note++];
			int8_t raw_note;
			if(!child_int(comp, "x", &nt->x)
			   || !child_int(comp, "y", &nt->y)
			   || !child_int(comp, "z", &nt->z)
			   || !child_byte(comp, "note", &raw_note)
			   || !te_position_is_owned(nt->x, nt->y, nt->z, chunk_x,
									chunk_z))
				goto fail;
			// Beta TileEntityNote.readFromNBT: clamp the raw signed byte to
			// [0,24] -- a corrupt/hand-edited file could carry any value.
			// [decompile 740c583]
			if(raw_note < NOTEBLOCK_NOTE_MIN)
				raw_note = NOTEBLOCK_NOTE_MIN;
			if(raw_note > NOTEBLOCK_NOTE_MAX)
				raw_note = NOTEBLOCK_NOTE_MAX;
			nt->note = raw_note;
		} else if(!strcmp(id->payload.tag_string, "RecordPlayer")
				  && parsed.record_players) {
			struct te_record_player* rec
				= &parsed.record_players[parsed.n_record_player++];
			if(!child_int(comp, "x", &rec->x)
			   || !child_int(comp, "y", &rec->y)
			   || !child_int(comp, "z", &rec->z)
			   || !te_position_is_owned(rec->x, rec->y, rec->z, chunk_x,
									chunk_z))
				goto fail;
			// Beta TileEntityRecordPlayer.readFromNBT: getInteger("Record")
			// returns 0 when the tag is absent OR the wrong type (Beta's OWN
			// forgiving default, not a CaveWii addition) -- never rejects the
			// record for a missing/malformed Record tag.
			nbt_node* rec_tag = find_child(comp, "Record");
			rec->record = (rec_tag && rec_tag->type == TAG_INT)
				? rec_tag->payload.tag_int
				: 0;
		} else if(!strcmp(id->payload.tag_string, "MobSpawner")
				  && parsed.spawners) {
			struct te_spawner* sp = &parsed.spawners[parsed.n_spawner++];
			const char* entity_id;
			int16_t delay;
			if(!child_int(comp, "x", &sp->x)
			   || !child_int(comp, "y", &sp->y)
			   || !child_int(comp, "z", &sp->z)
			   || !child_string(comp, "EntityId", &entity_id)
			   || !child_short(comp, "Delay", &delay)
			   || !te_position_is_owned(sp->x, sp->y, sp->z, chunk_x,
									chunk_z))
				goto fail;
			// Beta's own String has no length cap; this port's buffer does --
			// bound the copy defensively (a corrupt/hand-edited file could
			// carry an arbitrarily long EntityId) rather than rejecting the
			// whole parse over one oversized string, matching the sign
			// codec's own character-count clamp philosophy.
			size_t len = strlen(entity_id);
			if(len > sizeof(sp->mob_id) - 1)
				len = sizeof(sp->mob_id) - 1;
			memcpy(sp->mob_id, entity_id, len);
			sp->mob_id[len] = '\0';
			sp->delay = delay;
		} else if(!strcmp(id->payload.tag_string, "Piston")
				  && parsed.piston_movings) {
			struct te_piston_moving* pm
				= &parsed.piston_movings[parsed.n_piston_moving++];
			if(!child_int(comp, "x", &pm->x)
			   || !child_int(comp, "y", &pm->y)
			   || !child_int(comp, "z", &pm->z)
			   || !child_int(comp, "blockId", &pm->stored_id)
			   || !child_int(comp, "blockData", &pm->stored_meta)
			   || !child_int(comp, "facing", &pm->facing)
			   || !child_float(comp, "progress", &pm->progress)
			   || !child_byte(comp, "extending", &pm->extending)
			   || !te_position_is_owned(pm->x, pm->y, pm->z, chunk_x,
									chunk_z)
			   || !te_piston_payload_is_valid(pm))
				goto fail;
		}
	}
	*out = parsed;
	return true;

fail:
	tile_entities_parse_free(&parsed);
	return false;
}

void tile_entities_parse_free(struct chunk_tile_entities* out) {
	if(!out)
		return;
	free(out->chests);
	free(out->signs);
	free(out->furnaces);
	free(out->dispensers);
	free(out->notes);
	free(out->record_players);
	free(out->spawners);
	free(out->piston_movings);
	out->chests = NULL;
	out->signs = NULL;
	out->furnaces = NULL;
	out->dispensers = NULL;
	out->notes = NULL;
	out->record_players = NULL;
	out->spawners = NULL;
	out->piston_movings = NULL;
	out->n_chest = 0;
	out->n_sign = 0;
	out->n_furnace = 0;
	out->n_dispenser = 0;
	out->n_note = 0;
	out->n_record_player = 0;
	out->n_spawner = 0;
	out->n_piston_moving = 0;
	out->legacy_flat_complete = false;
}

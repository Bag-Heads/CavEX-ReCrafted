/*
	FH-14 (docs/fork_hack_audit_2026-08-21.md:166): redstone wire's digging
	hardness was the fork-inherited 1050 (a real multi-second dig time) instead
	of Beta's instant-break setHardness(0.0F)
	[740c583 Block.java:647: redstoneWire = (new BlockRedstoneWire(55,
	164)).setHardness(0.0F)...].

	This test pins the CHOSEN encoding, not just the raw field value, because
	this port has two different encodings for a Beta hardness-0.0F block (see
	block_redstone_wire.c's comment and block_locked_chest.c's identical
	note): block_repeater.c stores the literal 0, the majority (torch/
	tallgrass/sapling/flower/mushroom/tnt/fire/crops/reed/rose/locked_chest)
	store a nominal 50. source/item/tool.c's tool_dig_delay_ms collapses ANY
	hardness<=0 -- the literal-0 encoding included -- to the SAME "-1"
	sentinel block_bedrock.c/block_portal.c/block_piston.c use for genuinely
	UNBREAKABLE blocks (hardness==-1). Both of that return value's consumers
	gate on it being positive (screen_ingame.c's dig-completion check is
	`delay > 0 && elapsed >= delay`; render_block.c's crack overlay is
	`b->digging.hardness > 0`), so a literal 0 does NOT complete a hold-to-dig
	in this engine at all -- it is indistinguishable from truly unbreakable.
	The nominal-50 encoding is the one that actually reproduces Beta's
	"clears on the first tick" behaviour through this engine's real mechanism
	(50 -> tool_dig_delay_ms returns exactly 50ms, one 20-TPS tick). This test
	pins that: wire must return a small positive delay (one tick), NEVER the
	-1 "unbreakable" sentinel -- for hand AND for held tools/items alike,
	since Beta's redstoneWire is outside ItemTool.blocksEffectiveAgainst and
	not Material-gated, so every held item digs it at the identical speed
	(no per-tool branch in tool_dig_delay_ms fires: digging.tool/min are both
	TOOL_TYPE_ANY/TOOL_TIER_ANY, so the tool-match, sword, and
	wrong-tier-penalty branches all fall through unchanged).
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"
#include "item/tool.h"

extern struct block block_redstone_wire;
extern struct block block_redstone_repeater;
extern struct block block_redstone_repeater_lit;

static struct item* held(uint16_t id) {
	struct item_data d = {.id = id, .count = 1};
	struct item* it = item_get(&d);
	assert(it && "held item id not registered");
	return it;
}

int main(void) {
	blocks_init();
	items_init();

	// Bare hand (it == NULL): must be the faithful one-tick delay, never -1
	// ("cannot complete a hold-to-dig at all", block_repeater.c's outlier
	// literal-0 encoding's actual effect under this engine's tool.c).
	int hand = tool_dig_delay_ms(&block_redstone_wire, NULL);
	printf("test_redstone_wire_dig: hand delay=%dms\n", hand);
	assert(hand == 50);

	// Every held item digs it at the SAME speed -- pickaxe, sword, and even a
	// non-tool item (a stick) all fall through tool_dig_delay_ms's
	// tool-match/sword/wrong-tier branches unchanged because
	// block_redstone_wire.digging.tool/min are TOOL_TYPE_ANY/TOOL_TIER_ANY.
	uint16_t items[] = {ITEM_WOOD_PICKAXE, ITEM_IRON_PICKAXE,
						ITEM_DIAMOND_SWORD, ITEM_STICK};
	for(size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
		int d = tool_dig_delay_ms(&block_redstone_wire, held(items[i]));
		printf("test_redstone_wire_dig: item id=%u delay=%dms\n", items[i], d);
		assert(d == 50);
	}

	// Regression guard against reverting to the old fork-inherited 1050 (or
	// any other multi-tick value): the field itself, not just tool.c's
	// derived delay.
	assert(block_redstone_wire.digging.hardness == 50);

	// Never the truly-unbreakable sentinel this port also uses for
	// bedrock/portal/piston (hardness==-1 -> tool_dig_delay_ms returns -1).
	assert(hand != -1);

	// task_6ea50e06 (found by the FH-14 audit of this exact encoding): BOTH
	// repeater variants stored the literal 0, which tool_dig_delay_ms
	// collapses to the -1 UNBREAKABLE sentinel -- Beta setHardness(0.0F)
	// [740c583 Block.java:685-686] means instant break, so the repeater was
	// silently un-diggable on this port. Same 50 encoding as wire now.
	assert(block_redstone_repeater.digging.hardness == 50);
	assert(block_redstone_repeater_lit.digging.hardness == 50);
	int rep_hand = tool_dig_delay_ms(&block_redstone_repeater, NULL);
	int rep_lit = tool_dig_delay_ms(&block_redstone_repeater_lit, NULL);
	printf("test_redstone_wire_dig: repeater delay=%dms lit=%dms\n", rep_hand,
		   rep_lit);
	assert(rep_hand == 50 && rep_lit == 50);

	printf("test_redstone_wire_dig: PASS\n");
	return 0;
}

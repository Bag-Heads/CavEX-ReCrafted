/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include "loading_tips.h"

#include <limits.h>
#include <string.h>

/* TIPS-SPEC-v2 section 5; copy and item subjects from prototype.html. */
const struct loading_tip loading_tips[LOADING_TIPS_COUNT] = {
	{"S01", "Food restores health. Keep some for when you are hurt.", LOADING_TIP_SURVIVAL, 0, 0},
	{"S02", "Cook porkchops before eating: cooked pork restores more health.", LOADING_TIP_SURVIVAL, 320, 0},
	{"S03", "Cooked fish restores more health than raw fish.", LOADING_TIP_SURVIVAL, 350, 0},
	{"S04", "Light your shelter. Most monsters need darkness to spawn.", LOADING_TIP_SURVIVAL, 0, 0},
	{"S05", "Come up for air before you run out of breath underwater.", LOADING_TIP_SURVIVAL, 0, 0},
	{"S06", "You drop your inventory when you die. Take only what you need.", LOADING_TIP_SURVIVAL, 0, 0},
	{"S07", "Start with wood: one log makes four planks.", LOADING_TIP_SURVIVAL, 17, 0},
	{"S08", "Falls of more than three blocks hurt. Water breaks your fall.", LOADING_TIP_SURVIVAL, 0, 0},
	{"C01", "Four planks make a crafting table with a 3 by 3 grid.", LOADING_TIP_CRAFTING, 58, 0},
	{"C02", "Smelt logs in a furnace to make charcoal.", LOADING_TIP_CRAFTING, 263, 1},
	{"C03", "Coal or charcoal above a stick makes four torches.", LOADING_TIP_CRAFTING, 50, 0},
	{"C04", "Eight cobblestone around an empty centre make a furnace.", LOADING_TIP_CRAFTING, 61, 0},
	{"C05", "Smelt iron ore in a furnace to get iron ingots.", LOADING_TIP_CRAFTING, 265, 0},
	{"C06", "Smelt sand into glass for windows.", LOADING_TIP_CRAFTING, 20, 0},
	{"C07", "One piece of coal smelts eight items in a furnace.", LOADING_TIP_CRAFTING, 263, 0},
	{"C08", "A lava bucket keeps a furnace burning for a hundred items.", LOADING_TIP_CRAFTING, 327, 0},
	{"M01", "Use a stone, iron or diamond pickaxe to collect iron ore.", LOADING_TIP_MINING, 274, 0},
	{"M02", "Use an iron or diamond pickaxe to collect diamonds.", LOADING_TIP_MINING, 264, 0},
	{"M03", "Gold ore needs an iron or diamond pickaxe. A golden pickaxe will not harvest it.", LOADING_TIP_MINING, 14, 0},
	{"M04", "Redstone ore needs an iron or diamond pickaxe.", LOADING_TIP_MINING, 73, 0},
	{"M05", "Only a diamond pickaxe can harvest obsidian.", LOADING_TIP_MINING, 49, 0},
	{"M06", "Sand falls when unsupported. Be careful when digging beneath it.", LOADING_TIP_MINING, 12, 0},
	{"M07", "Diamonds and redstone are found only deep down, below layer sixteen.", LOADING_TIP_MINING, 264, 0},
	{"M08", "Digging gravel sometimes gives flint instead.", LOADING_TIP_MINING, 318, 0},
	{"M09", "Water flowing onto still lava makes obsidian.", LOADING_TIP_MINING, 49, 0},
	{"E01", "Zombies burn in direct daylight. Shade can protect them.", LOADING_TIP_MOBS, 0, 0},
	{"E02", "Skeletons fire arrows. Keep cover between you and them.", LOADING_TIP_MOBS, 0, 0},
	{"E03", "Back away when a creeper starts hissing. Distance can stop its fuse.", LOADING_TIP_MOBS, 0, 0},
	{"E04", "Spiders can climb walls. A tall wall alone will not keep them out.", LOADING_TIP_MOBS, 0, 0},
	{"E05", "Spiders in bright light leave you alone unless you attack them.", LOADING_TIP_MOBS, 0, 0},
	{"E06", "Offer bones to a calm wild wolf to try to tame it.", LOADING_TIP_MOBS, 0, 0},
	{"E07", "Feed pork to an injured tame wolf to restore its health.", LOADING_TIP_MOBS, 0, 0},
	{"E08", "Shears give two to four wool from a sheep without hurting it.", LOADING_TIP_MOBS, 0, 0},
	{"E09", "Put a saddle on a pig to ride it.", LOADING_TIP_MOBS, 0, 0},
	{"E10", "Use an empty bucket on a cow to collect milk.", LOADING_TIP_MOBS, 0, 0},
	{"F01", "Use a hoe on grass or dirt to prepare farmland.", LOADING_TIP_FARMING, 290, 0},
	{"F02", "Water within four blocks keeps farmland hydrated.", LOADING_TIP_FARMING, 0, 0},
	{"F03", "Crops need light level nine or higher to grow.", LOADING_TIP_FARMING, 0, 0},
	{"F04", "Wait for wheat to mature before harvesting it. Young crops drop no wheat.", LOADING_TIP_FARMING, 296, 0},
	{"F05", "Bone meal makes wheat fully grown immediately.", LOADING_TIP_FARMING, 351, 15},
	{"F06", "Plant sugar cane on grass or dirt beside water.", LOADING_TIP_FARMING, 338, 0},
	{"R01", "Redstone signals weaken along dust. Keep wire runs short.", LOADING_TIP_REDSTONE, 331, 0},
	{"R02", "Use a repeater to cycle through its four delay settings.", LOADING_TIP_REDSTONE, 356, 0},
	{"R03", "A lever switches redstone power on and off.", LOADING_TIP_REDSTONE, 69, 0},
	{"R04", "A piston can push up to twelve blocks at once.", LOADING_TIP_REDSTONE, 33, 0},
	{"R05", "Pistons cannot push obsidian.", LOADING_TIP_REDSTONE, 49, 0},
	{"N01", "Do not try to sleep in the Nether. Beds explode there.", LOADING_TIP_NETHER, 355, 0},
	{"N02", "Water from a bucket evaporates in the Nether.", LOADING_TIP_NETHER, 326, 0},
	{"N03", "Leave zombie pigmen alone. Hitting one angers others nearby.", LOADING_TIP_NETHER, 0, 0},
	{"N04", "Soul sand slows you down. Watch your footing near danger.", LOADING_TIP_NETHER, 88, 0},
	{"N05", "Fire on netherrack does not burn out by itself.", LOADING_TIP_NETHER, 87, 0},
	{"N06", "One block travelled in the Nether is eight in the Overworld.", LOADING_TIP_NETHER, 0, 0},
	{"N07", "Light the inside of an obsidian frame with flint and steel to open a portal.", LOADING_TIP_NETHER, 259, 0},
	{"W01", "After loading, {inventory} opens your inventory.", LOADING_TIP_CONTROLS, 0, 0},
	{"W02", "In play, hold {mine} to mine the block you aim at.", LOADING_TIP_CONTROLS, 0, 0},
	{"W03", "In play, {use} places a block or uses an item.", LOADING_TIP_CONTROLS, 0, 0},
	{"W04", "{previous} and {next} change your hotbar slot.", LOADING_TIP_CONTROLS, 0, 0},
	{"W05", "In play, {menu} opens the game menu.", LOADING_TIP_CONTROLS, 0, 0},
	{"P01", "In split-screen, the world keeps running while your menu is open.", LOADING_TIP_SPLIT, 0, 0},
	{"P02", "Player 2 can choose Leave game from their menu.", LOADING_TIP_SPLIT, 0, 0},
	{"P03", "The host's Save and quit ends the session for both players.", LOADING_TIP_SPLIT, 0, 0},
	{"P04", "Each player opens their own inventory with their own {inventory} button.", LOADING_TIP_SPLIT, 0, 0},
};

static const uint8_t beginner[] = {6, 8, 11, 9, 10, 3};
static const char* actions[] = {
	"inventory", "mine", "use", "previous", "next", "menu"
};
struct tip_identity {
	char profile[LOADING_TIPS_PROFILE_MAX + 1];
	struct loading_tip_history history;
	uint64_t used;
	uint32_t random;
	uint8_t control_cooldown;
};
struct tip_owner {
	enum loading_reason reason;
	bool active, primed, visible, read, blocked, have_context, eligible_last;
	uint64_t first_ms, last_ms, gate_ms, visible_ms;
	int identity, current;
	struct loading_tip_context context;
	struct loading_tip_layout layout;
};
static struct tip_identity identities[LOADING_TIPS_IDENTITIES];
static struct tip_owner owners[LOADING_TIPS_OWNERS] = {
	{.reason = UNKNOWN, .identity = -1, .current = -1},
	{.reason = UNKNOWN, .identity = -1, .current = -1}
};
static uint64_t use_serial;

static uint64_t tip_bit(unsigned i) { return UINT64_C(1) << i; }

void loading_tips_reset(void) {
	memset(identities, 0, sizeof identities);
	memset(owners, 0, sizeof owners);
	use_serial = 0;
	for(unsigned i = 0; i < LOADING_TIPS_OWNERS; i++) {
		owners[i].reason = UNKNOWN;
		owners[i].identity = owners[i].current = -1;
	}
}

enum loading_reason loading_reason_get(unsigned owner) {
	return owner < LOADING_TIPS_OWNERS && owners[owner].active
		? owners[owner].reason : UNKNOWN;
}

void loading_reason_begin(unsigned owner, enum loading_reason reason) {
	if(owner >= LOADING_TIPS_OWNERS) return;
	struct tip_owner* p = &owners[owner];
	/* Preserve only the bound local identity across episodes. */
	int identity = p->identity;
	*p = (struct tip_owner) {.reason = reason >= NEW_WORLD && reason <= UNKNOWN
		? reason : UNKNOWN, .active = true, .identity = identity, .current = -1};
}

void loading_reason_end(unsigned owner) {
	if(owner >= LOADING_TIPS_OWNERS) return;
	struct tip_owner* p = &owners[owner];
	p->active = p->visible = false;
	p->reason = UNKNOWN;
	p->current = -1;
}

const char* loading_reason_heading(enum loading_reason reason) {
	switch(reason) {
		case NEW_WORLD: return "Generating level";
		case LOAD_SAVE: case SPLIT_START: case MP_TERRAIN: return "Loading level";
		case PORTAL_IN: return "Entering the Nether";
		case PORTAL_OUT: return "Leaving the Nether";
		case RESPAWN: return "Respawning";
		default: return "Loading world";
	}
}

const char* loading_reason_stage(enum loading_reason reason) {
	(void)reason;
	return "Building terrain";
}

bool loading_tips_set_identity(unsigned owner, const char* profile) {
	if(owner >= LOADING_TIPS_OWNERS || !profile) return false;
	size_t len = strlen(profile);
	if(!len || len > LOADING_TIPS_PROFILE_MAX) return false;
	bool same = owners[owner].identity >= 0
		&& owners[owner].identity < LOADING_TIPS_IDENTITIES
		&& !strcmp(identities[owners[owner].identity].profile, profile);
	for(size_t i = 0; i < len; i++)
		if(!(profile[i] >= 'a' && profile[i] <= 'z')
		   && !(profile[i] >= '0' && profile[i] <= '9') && profile[i] != '_')
			return false;
	int found = -1, oldest = -1;
	uint64_t oldest_use = UINT64_MAX;
	for(int i = 0; i < LOADING_TIPS_IDENTITIES; i++) {
		if(!strcmp(profile, identities[i].profile)) { found = i; break; }
		bool pinned = false;
		for(unsigned k = 0; k < LOADING_TIPS_OWNERS; k++)
			if(k != owner && owners[k].active && owners[k].identity == i)
				pinned = true;
		if(!pinned && identities[i].used < oldest_use) {
			oldest = i; oldest_use = identities[i].used;
		}
	}
	if(found < 0) {
		found = oldest;
		if(found < 0) return false;
		for(unsigned k = 0; k < LOADING_TIPS_OWNERS; k++)
			if(owners[k].identity == found) {
				owners[k].identity = owners[k].current = -1;
				owners[k].visible = owners[k].eligible_last = false;
			}
		memset(&identities[found], 0, sizeof identities[found]);
		memcpy(identities[found].profile, profile, len + 1);
		/* UI-local xorshift, never a world RNG or clock seed. */
		identities[found].random = 1006;
		for(size_t i = 0; i < len; i++)
			identities[found].random = identities[found].random * 33u
				+ (unsigned char)profile[i];
		if(!identities[found].random) identities[found].random = 1006;
	}
	struct tip_owner* p = &owners[owner];
	if(!same) {
		p->current = -1; p->visible = p->read = false;
		p->gate_ms = p->visible_ms = 0; p->eligible_last = false;
	}
	p->identity = found;
	identities[found].used = ++use_serial;
	return true;
}

bool loading_tips_history(const char* profile, struct loading_tip_history* out) {
	if(!profile || !out) return false;
	for(unsigned i = 0; i < LOADING_TIPS_IDENTITIES; i++)
		if(identities[i].profile[0] && !strcmp(profile, identities[i].profile)) {
			*out = identities[i].history; return true;
		}
	return false;
}

void loading_tips_save_history(void) {
	/* TODO TIPS-2: v2 section 6 persistence; no IO on a loading frame. */
}

unsigned loading_tips_word_count(const char* text) {
	unsigned count = 0;
	if(!text) return 0;
	while(*text) {
		while(*text == ' ') text++;
		if(!*text) break;
		count += *text == '{' ? 2 : 1;
		while(*text && *text != ' ') text++;
	}
	return count;
}

uint32_t loading_tips_read_ms(unsigned words) {
	uint64_t ms = UINT64_C(2000) + (uint64_t)words * 500;
	if(ms < 6000) ms = 6000;
	return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

uint32_t loading_tips_dwell_ms(unsigned words) {
	uint64_t ms = UINT64_C(4000) + (uint64_t)words * 300;
	if(ms < 8000) ms = 8000;
	if(ms > 14000) ms = 14000;
	uint32_t read = loading_tips_read_ms(words);
	return ms > read ? (uint32_t)ms : read;
}

static int text_width(const char* text, const struct loading_tip_context* c) {
	int w = 0;
	while(*text) w += (c->font[(unsigned char)*text++] + 1) * 2;
	return w; /* gutil_font_width(...,16), without graphics dependencies */
}

bool loading_tips_layout(const struct loading_tip* tip,
	const struct loading_tip_context* c, struct loading_tip_layout* out) {
	if(!tip || !c || !out || c->width < 640) return false;
	struct loading_tip_layout l = {0};
	l.width = c->width < 854 ? 576 : 720;
	l.x = (c->width - l.width) / 2;
	l.icon = tip->item != 0 && !c->split_pane;
	l.body_x = l.x + 16 + (l.icon ? 56 : 0);
	int room = l.width - 32 - (l.icon ? 56 : 0);
	int gap = text_width(" ", c), total = 0;
	const char* at = tip->text;
	while(*at) {
		if(l.count == LOADING_TIPS_WORDS_MAX) return false;
		struct loading_tip_token* t = &l.tokens[l.count];
		t->glyph = -1;
		const char* end = strchr(at, ' ');
		if(!end) end = at + strlen(at);
		const char* text = at;
		if(*at == '{') {
			const char* close = memchr(at, '}', (size_t)(end - at));
			if(!close) return false;
			int action = -1;
			for(int k = 0; k < LOADING_TIP_ACTIONS; k++)
				if(strlen(actions[k]) == (size_t)(close - at - 1)
				   && !memcmp(actions[k], at + 1, (size_t)(close - at - 1)))
					action = k;
			if(action < 0 || c->glyph[action] < 0
			   || c->glyph_width[action] <= 0) return false;
			t->glyph = c->glyph[action]; t->width = c->glyph_width[action];
			text = close + 1;
		}
		size_t n = (size_t)(end - text);
		if(n >= sizeof t->text) return false;
		memcpy(t->text, text, n); t->text[n] = 0;
		t->width += text_width(t->text, c);
		total += t->width + (l.count ? gap : 0);
		l.count++;
		at = *end ? end + 1 : end;
	}
	if(!l.count) return false;
	unsigned cut = l.count;
	l.lines = 1; l.line_width[0] = total;
	if(total > room) {
		int first = 0, best = INT_MAX;
		cut = 0;
		/* At least TWO words on each side; no one-word last line. */
		for(unsigned k = 1; k < l.count; k++) {
			first += l.tokens[k - 1].width + (k > 1 ? gap : 0);
			int second = total - first - gap;
			int diff = first > second ? first - second : second - first;
			if(k >= 2 && l.count - k >= 2 && first <= room && second <= room
			   && diff <= best) {
				best = diff; cut = k;
				l.line_width[0] = first; l.line_width[1] = second;
			}
		}
		if(!cut) return false;
		l.lines = 2;
	}
	bool last_glyph = false;
	for(unsigned k = 0; k < l.count; k++) {
		l.tokens[k].line = k >= cut ? 1 : 0;
		if(l.tokens[k].line == l.lines - 1 && l.tokens[k].glyph >= 0)
			last_glyph = true;
	}
	l.height = 32 + (int)(l.lines - 1) * 24 + (last_glyph ? 20 : 16) + 8;
	l.y = (c->split_pane ? 224 : 448) - l.height;
	if(l.y < (c->split_pane ? 128 : 360)) return false;
	*out = l;
	return true;
}

static bool portal(enum loading_reason r) { return r == PORTAL_IN || r == PORTAL_OUT; }
static uint32_t random_draw(struct tip_identity* h) {
	uint32_t x = h->random;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	return h->random = x;
}

static bool eligible(unsigned id, const struct tip_owner* p,
	const struct loading_tip_context* c) {
	const struct loading_tip* t = &loading_tips[id];
	if(t->category == LOADING_TIP_NETHER && p->reason != PORTAL_IN) return false;
	if(t->category == LOADING_TIP_SPLIT
	   && (!c->split_active || portal(p->reason) || p->reason == SPLIT_START
		   || p->reason == MP_TERRAIN)) return false;
	if(p->reason == SPLIT_START && t->category == LOADING_TIP_CONTROLS) return false;
	/* MP peers can change death and mob rules. */
	if(p->reason == MP_TERRAIN && (t->category == LOADING_TIP_MOBS
	   || id == 3 || id == 5 || id == 48)) return false; /* S04,S06,N03 */
	if(c->creative && (t->category == LOADING_TIP_SURVIVAL
	   || t->category == LOADING_TIP_MOBS || id == 48)) return false;
	if(c->peaceful && (id == 3 || (id >= 25 && id <= 29))) return false;
	if(c->gamecube && id == 57) return false; /* W05, v2 suppression */
	struct loading_tip_layout layout;
	return loading_tips_layout(t, c, &layout);
}

static uint64_t recent_bits(const struct loading_tip_history* h) {
	uint64_t bits = 0;
	for(unsigned k = 0; k < h->read_count; k++) bits |= tip_bit(h->last_read[k]);
	return bits;
}

static int select_tip(unsigned owner, const struct loading_tip_context* c) {
	struct tip_owner* p = &owners[owner];
	struct tip_identity* h = &identities[p->identity];
	uint64_t pool = 0;
	for(unsigned k = 0; k < LOADING_TIPS_COUNT; k++) {
		bool other = false;
		for(unsigned v = 0; v < LOADING_TIPS_OWNERS; v++)
			if(v != owner && owners[v].visible && owners[v].current == (int)k)
				other = true;
		if(!other && eligible(k, p, c)) pool |= tip_bit(k);
	}
	if(!pool) return -1;
	uint64_t allowed = pool;
	if(h->control_cooldown)
		for(unsigned k = 53; k <= 57; k++) allowed &= ~tip_bit(k);
	uint64_t candidates = allowed & ~h->history.cycle_seen;
	/* Priorities are unread teaching tips, including a prior cut-short tip. */
	if(p->reason == NEW_WORLD)
		for(unsigned k = 0; k < sizeof beginner; k++)
			if(!(h->history.beginner_bits & (1u << k))
			   && (pool & tip_bit(beginner[k])) && p->current != beginner[k]
			   && ((candidates & tip_bit(beginner[k])) || p->current < 0))
				return beginner[k];
	if(p->reason == PORTAL_IN)
		for(unsigned k = 46; k <= 52; k++)
			if(!(h->history.nether_bits & (1u << (k - 46))) && (pool & tip_bit(k))
			   && p->current != (int)k
			   && ((candidates & tip_bit(k)) || p->current < 0)) return (int)k;
	if(!candidates) {
		h->history.cycle_seen = recent_bits(&h->history);
		if(p->current >= 0) h->history.cycle_seen |= tip_bit((unsigned)p->current);
		candidates = allowed & ~h->history.cycle_seen;
		if(!candidates) return p->current >= 0 && (pool & tip_bit((unsigned)p->current))
			? p->current : -1;
	}
	if(!candidates) return -1;
	/* Portal exit/general fallback uses the shortest copy first. Other entry
 * loads prefer 8-14 words for the likely-only first tip. */
	if(portal(p->reason)) {
		int shortest = -1; unsigned words = UINT_MAX;
		for(unsigned k = 0; k < LOADING_TIPS_COUNT; k++)
			if((candidates & tip_bit(k)) && loading_tips[k].category != LOADING_TIP_NETHER) {
				unsigned n = loading_tips_word_count(loading_tips[k].text);
				if(n < words) { words = n; shortest = (int)k; }
			}
		if(shortest >= 0) return shortest;
	}
	if(p->current < 0) {
		uint64_t short_pool = 0;
		for(unsigned k = 0; k < LOADING_TIPS_COUNT; k++) {
			unsigned n = loading_tips_word_count(loading_tips[k].text);
			if(n >= 8 && n <= 14) short_pool |= tip_bit(k);
		}
		if(candidates & short_pool) candidates &= short_pool;
	}
	static const unsigned weights[] = {4, 2, 3, 2, 2, 1, 1, 2, 1};
	unsigned counts[9] = {0}, sum = 0;
	for(unsigned k = 0; k < LOADING_TIPS_COUNT; k++)
		if(candidates & tip_bit(k)) counts[loading_tips[k].category]++;
	for(unsigned k = 0; k < 9; k++) if(counts[k]) sum += weights[k];
	unsigned draw = random_draw(h) % sum, cat = 0;
	for(; cat < 9; cat++) if(counts[cat]) {
		if(draw < weights[cat]) break;
		draw -= weights[cat];
	}
	draw = random_draw(h) % counts[cat];
	for(unsigned k = 0; k < LOADING_TIPS_COUNT; k++)
		if((candidates & tip_bit(k)) && loading_tips[k].category == (enum loading_tip_category)cat)
			if(draw-- == 0) return (int)k;
	return -1;
}

static void record_read(struct tip_owner* p) {
	struct loading_tip_history* h = &identities[p->identity].history;
	unsigned n = 0;
	for(unsigned k = 0; k < h->read_count; k++)
		if(h->last_read[k] != p->current) h->last_read[n++] = h->last_read[k];
	if(n == 16) { memmove(h->last_read, h->last_read + 1, 15); n = 15; }
	h->last_read[n++] = (uint8_t)p->current; h->read_count = (uint8_t)n;
	if(p->reason == NEW_WORLD)
		for(unsigned k = 0; k < sizeof beginner; k++)
			if(beginner[k] == p->current) h->beginner_bits |= (uint8_t)(1u << k);
	if(p->current >= 46 && p->current <= 52)
		h->nether_bits |= (uint8_t)(1u << (p->current - 46));
	p->read = true;
}

const struct loading_tip* loading_tips_tick(unsigned owner, uint64_t now_ms,
	const struct loading_tip_context* c) {
	if(owner >= LOADING_TIPS_OWNERS || !c) return NULL;
	struct tip_owner* p = &owners[owner];
	if(!p->active || p->identity < 0 || p->identity >= LOADING_TIPS_IDENTITIES
	   || !identities[p->identity].profile[0]) return NULL;
	uint64_t dt = p->primed && now_ms >= p->last_ms ? now_ms - p->last_ms : 0;
	if(!p->primed) p->first_ms = now_ms;
	p->primed = true; p->last_ms = now_ms;
	if(c->blocked || (p->reason == MP_TERRAIN && now_ms >= p->first_ms
	   && now_ms - p->first_ms >= 10000)) p->blocked = true;
	if(p->reason == UNKNOWN || p->reason == RESPAWN || p->blocked
	   || !c->enabled || !c->visible) {
		p->visible = p->eligible_last = false; p->gate_ms = 0; return NULL;
	}
	if(!p->eligible_last) dt = 0;
	p->eligible_last = true;
	/* Explicit field comparison avoids padding-dependent cache invalidation. */
	bool bindings_changed = p->have_context &&
		(p->context.gamecube != c->gamecube
		 || memcmp(p->context.glyph, c->glyph, sizeof c->glyph)
		 || memcmp(p->context.glyph_width, c->glyph_width, sizeof c->glyph_width));
	bool changed = !p->have_context || p->context.width != c->width
		|| p->context.split_pane != c->split_pane
		|| p->context.split_active != c->split_active
		|| p->context.creative != c->creative || p->context.peaceful != c->peaceful
		|| bindings_changed
		|| memcmp(p->context.font, c->font, sizeof c->font);
	if(changed) {
		p->context = *c; p->have_context = true;
		if(p->current >= 0) {
			if(!eligible((unsigned)p->current, p, c)) {
				p->current = -1; p->visible = false; p->gate_ms = 0; dt = 0;
			} else {
				loading_tips_layout(&loading_tips[p->current], c, &p->layout);
				if(bindings_changed) {
					p->visible_ms = 0; p->read = false; dt = 0;
				}
			}
		}
	}
	if(p->visible && p->current >= 0) {
		p->visible_ms += dt;
		unsigned words = loading_tips_word_count(loading_tips[p->current].text);
		if(!p->read && p->visible_ms >= loading_tips_read_ms(words)) record_read(p);
		if(p->visible_ms < loading_tips_dwell_ms(words)) return &loading_tips[p->current];
	} else {
		p->gate_ms += dt;
		if(p->gate_ms < (portal(p->reason) ? 4000u : 1250u)) return NULL;
		if(p->current >= 0) { p->visible = true; return &loading_tips[p->current]; }
	}
	int next = select_tip(owner, c);
	if(next < 0) { p->visible = false; return NULL; }
	if(next == p->current) return &loading_tips[next];
	p->current = next; p->visible = true; p->visible_ms = 0; p->read = false;
	identities[p->identity].history.cycle_seen |= tip_bit((unsigned)next);
	identities[p->identity].used = ++use_serial;
	if(loading_tips[next].category == LOADING_TIP_CONTROLS)
		identities[p->identity].control_cooldown = 2;
	else if(identities[p->identity].control_cooldown)
		identities[p->identity].control_cooldown--;
	loading_tips_layout(&loading_tips[next], c, &p->layout);
	return &loading_tips[next];
}

const struct loading_tip_layout* loading_tips_current_layout(unsigned owner) {
	return owner < LOADING_TIPS_OWNERS && owners[owner].visible
		&& owners[owner].current >= 0 ? &owners[owner].layout : NULL;
}

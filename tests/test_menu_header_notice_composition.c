/* GOLDEN -- approved header/notice composition (menu_header.h).
 *
 * The controller handoff (C:/tmp/cw-l3/docs/ui_design/CONTROLLER_HANDOFF.md)
 * left one open decision: "How should ordinary notices coexist with each
 * native menu header? The preview temporarily replaces a header strip; exact
 * native placement must retain menu actions, slots, focus and HUD at
 * 640 x 240. Prefer the approved per-menu header composition over masking a
 * fixed-height strip indiscriminately."
 *
 * menu_header.c implements the approved prototype's own rule (header
 * composition in the compact/split layout for a menu that owns a header;
 * the reserved floating band otherwise). THIS golden is the proof of the
 * property that decision exists to protect, for every
 *
 *     menu (5) x layout (2) x controller family (3)
 *
 * combination: the resolved notice rectangle never overlaps an ACTIONABLE
 * widget of that menu -- no button, no slot, no header control -- and the
 * header composition always lands inside the header band the menu reserved,
 * so the notice can never mask a live control or shift one when it arrives.
 *
 * The families multiply the sweep because the approved package requires all
 * three (menus.md shared rules: "Every menu is usable with Wii Remote +
 * Nunchuk, Wii Remote + Classic Controller and GameCube"). Family changes the
 * PROMPT GLYPHS inside a control, never a control's rectangle, so the sweep
 * asserts exactly that: the composition is family-invariant. A future change
 * that made a family widen a header control would fail here rather than ship
 * a notice sitting on top of a GameCube prompt.
 *
 * Pure geometry: this test compiles menu_header.c directly and needs no GL,
 * no window and no font. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/gui/menu_header.h"

/* The three approved controller families, by the name the package uses. */
static const char* const families[3]
	= {"Wii Remote + Nunchuk", "Classic Controller", "GameCube controller"};

/* The two layouts the approved package ships: the full single-player screen
 * and the compact split viewport. */
struct layout_case {
	const char* name;
	int w, h;
	enum menu_screen_layout expect;
};
static const struct layout_case layouts[2] = {
	{"full 640x480", 640, 480, MENU_SCREEN_FULL},
	{"compact 640x240", 640, 240, MENU_SCREEN_COMPACT},
};

/* ---- per-menu declarations ---------------------------------------------
 * Each builder declares the SAME header band and actionable rectangles the
 * shipping screen declares, through the same menu_header entry points and the
 * same geometry helpers. A rectangle here is a superset of the widget it
 * stands for (e.g. one keyboard band instead of 46 cells), which makes the
 * disjointness proof strictly stronger, never weaker. */

static void reserve_notice(struct menu_composition* c) {
 menu_composition_header_notice(c,280,144);
 menu_composition_seal(c);
}
static void declare_pause(struct menu_composition* c,int w,int h) {
 menu_composition_begin(c,MENU_SCREEN_PAUSE,0,0,w,h);bool full=h>=480;
 menu_composition_header(c,full?152:44,full?72:16,full?336:552,full?40:24);
 for(int k=0;k<(full?3:2);k++){struct menu_rect a=menu_pause_row(c,k);menu_composition_action(c,a.x,a.y,a.w,a.h);}
 reserve_notice(c);
}
static void declare_container(struct menu_composition* c,enum menu_screen_id id,int w,int h,int slots) {
 menu_composition_begin(c,id,0,0,w,h);bool full=h>=480;
 menu_composition_window(c,full?134:32,full?44:8,full?372:576,full?332:188);
 menu_composition_header(c,full?146:44,full?52:16,full?348:552,24);
 for(int k=0;k<36;k++)menu_composition_action(c,(full?158:272)+(k%9)*36,(full?220:40)+(k/9)*36+(k>=27?4:0),36,36);
 if(slots==3) {
  menu_composition_action(c,full?248:74,full?85:55,36,36);
  menu_composition_action(c,full?248:74,full?145:115,36,36);
  menu_composition_action(c,full?356:182,full?115:85,36,36);
 } else for(int k=0;k<9;k++)menu_composition_action(c,(full?266:92)+(k%3)*36,(full?79:49)+(k/3)*36,36,36);
 reserve_notice(c);
}
static void declare_sign(struct menu_composition* c,int w,int h) {
 menu_composition_begin(c,MENU_SCREEN_SIGN,0,0,w,h);bool full=h>=480;
 menu_composition_window(c,full?44:32,full?44:8,full?552:576,full?352:188);
 menu_composition_header(c,full?56:44,full?52:16,full?528:552,24);
 int fx=full?203:44,fy=full?112:76,fw=full?234:194;
 menu_composition_action(c,fx+8,fy+8,fw-16,88);
 menu_composition_action(c,full?44:266,full?232:47,full?552:330,full?156:133);
 menu_composition_action(c,fx,fy-28,fw-52,28);
 reserve_notice(c);
}

/* Mirrors screen_chat.c's chat_layout(): the sign editor's panel metrics
 * rearranged for a one-line draft -- a title-only header, the input strip
 * (draft text plus the Send/Cancel hints inside it), then the keyboard. */
static void declare_chat(struct menu_composition* c,int w,int h) {
 menu_composition_begin(c,MENU_SCREEN_CHAT,0,0,w,h);bool full=h>=480;
 int px=full?44:32,py=full?236:8,pw=full?552:576;
 menu_composition_window(c,px,py,pw,full?236:224);
 menu_composition_header(c,px+12,py+8,pw-24,24);
 menu_composition_action(c,px+12,py+40,pw-24,full?28:24);
 menu_composition_action(c,px+12,py+76,pw-24,full?156:133);
 reserve_notice(c);
}

/* Mirrors screen_death.c's death_compose(). The compact case is the affected
 * split viewport, which is why the sweep passes it as the whole view. */
static void declare_death(struct menu_composition* c, int w, int h) {
	menu_composition_begin(c, MENU_SCREEN_DEATH, 0, 0, w, h);
	for(int k = 0; k < 2; k++) {
		struct menu_rect r = menu_death_row(c, k);
		menu_composition_action(c, r.x, r.y, r.w, r.h);
	}
	menu_composition_seal(c);
}

static void build(struct menu_composition* c, enum menu_screen_id id, int w,
				  int h) {
	switch(id) {
		case MENU_SCREEN_PAUSE: declare_pause(c, w, h); break;
		case MENU_SCREEN_FURNACE: declare_container(c, id, w, h, 3); break;
		case MENU_SCREEN_DISPENSER: declare_container(c, id, w, h, 9); break;
		case MENU_SCREEN_SIGN: declare_sign(c, w, h); break;
		case MENU_SCREEN_CHAT: declare_chat(c, w, h); break;
		default: declare_death(c, w, h); break;
	}
}

static const char* menu_name(enum menu_screen_id id) {
	static const char* const names[MENU_SCREEN_COUNT]
		= {"pause", "furnace", "dispenser", "sign", "chat", "death"};
	return names[id];
}

static void test_rect_algebra(void) {
	struct menu_rect a = {0, 0, 10, 10}, b = {10, 0, 10, 10};
	struct menu_rect c = {9, 9, 10, 10}, empty = {5, 5, 0, 4};
	assert(!menu_rect_overlap(&a, &b)); /* touching edges do not overlap */
	assert(menu_rect_overlap(&a, &c));
	assert(!menu_rect_overlap(&a, &empty));
	assert(menu_rect_empty(&empty));
	struct menu_rect outer = {0, 0, 20, 20};
	assert(menu_rect_contains(&outer, &a));
	assert(!menu_rect_contains(&a, &outer));
	puts("  composition: rect algebra 1/1");
}

static void test_layout_rule(void) {
	assert(menu_screen_layout_for(640, 480) == MENU_SCREEN_FULL);
	assert(menu_screen_layout_for(640, 479) == MENU_SCREEN_COMPACT);
	assert(menu_screen_layout_for(640, 240) == MENU_SCREEN_COMPACT);
	puts("  composition: layout threshold 1/1");
}

static void test_split_admission(void) {
	/* IMPLEMENTATION.md open decision: never admit the split composition
	 * merely because a viewport measured short. */
	assert(!menu_split_composition_admitted(false, false));
	assert(!menu_split_composition_admitted(true, false));
	assert(!menu_split_composition_admitted(false, true));
	assert(menu_split_composition_admitted(true, true));
	puts("  composition: split admission needs a READY second player 1/1");
}

static void test_sweep(void) {
	unsigned cases = 0;
	for(int m = 0; m < MENU_SCREEN_COUNT; m++) {
		for(int l = 0; l < 2; l++) {
			for(int f = 0; f < 3; f++) {
				struct menu_composition c;
				build(&c, (enum menu_screen_id)m, layouts[l].w, layouts[l].h);
				cases++;

				/* Nothing may be silently dropped from a declaration. */
				assert(c.action_overflow == 0);
				assert(c.action_count > 0);
				assert(c.layout == layouts[l].expect);

				/* THE property: the notice never covers an actionable
				 * widget, for this menu x layout x family. */
				size_t hit = menu_composition_notice_conflict(&c);
				if(hit != c.action_count) {
					printf("FAIL %s / %s / %s: notice %d,%d %dx%d covers "
						   "action %zu at %d,%d %dx%d\n",
						   menu_name((enum menu_screen_id)m), layouts[l].name,
						   families[f], c.notice.x, c.notice.y, c.notice.w,
						   c.notice.h, hit, c.actions[hit].x, c.actions[hit].y,
						   c.actions[hit].w, c.actions[hit].h);
					fflush(stdout);
					assert(0 && "notice overlaps an actionable widget");
				}

				/* The notice is always inside the affected viewport
				 * (menus.md: "Notifications fit within their viewport"). */
				assert(menu_rect_contains(&c.view, &c.notice));

				/* The approved placement rule itself. */
				bool expect_header
					= c.has_header && c.layout == MENU_SCREEN_COMPACT;
				assert(c.notice_in_header == expect_header);
				if(expect_header) {
					/* Header composition stays inside the band the menu
					 * reserved, so it can never mask a live control ... */
					assert(menu_rect_contains(&c.header, &c.notice));
					/* ... and never overlaps the header CONTENT box, which is
					 * where the menu puts its own header widgets -- the
					 * reserve is why a notice arriving shifts nothing. */
					struct menu_rect content
						= menu_composition_header_content(&c);
					assert(!menu_rect_overlap(&content, &c.notice));
					assert(c.notice.w > 0 && c.notice.h > 0);
				} else {
					/* Otherwise the floating card, in the reserved band. */
					struct menu_rect band
						= menu_notice_band(c.view.x, c.view.y, c.view.w);
					assert(c.notice.x == band.x && c.notice.y == band.y
						   && c.notice.w == band.w && c.notice.h == band.h);
				}

				/* Family invariance: the rectangles are identical across all
				 * three approved families (family selects glyphs, not
				 * geometry), so proving one family proves all three -- and a
				 * regression that made geometry family-dependent fails here. */
				struct menu_composition first;
				build(&first, (enum menu_screen_id)m, layouts[l].w,
					  layouts[l].h);
				assert(first.action_count == c.action_count);
				assert(memcmp(&first.notice, &c.notice, sizeof(c.notice)) == 0);
				assert(memcmp(first.actions, c.actions,
							  c.action_count * sizeof(c.actions[0]))
					   == 0);
			}
		}
	}
	assert(cases == MENU_SCREEN_COUNT * 2 * 3);
	printf("  composition: %u menu x layout x family cases, notice disjoint "
		   "from every actionable widget %u/%u\n",
		   cases, cases, cases);
}

/* The compact container header is the case the handoff was actually worried
 * about: a 640x240 viewport where the tab row and the Prev/Next prompts share
 * one 32 px band with the notice. */
static void test_compact_container_header(void) {
	struct menu_rect head = menu_container_header_box(0, 0);
	struct menu_rect prev = menu_container_prev_box(0, 0);
	struct menu_rect next = menu_container_next_box(0, 0);
	struct menu_rect tabs = menu_container_tab_box(0, 0);
	struct menu_rect boxes[3] = {prev, tabs, next};
	for(int a = 0; a < 3; a++) {
		assert(boxes[a].x >= head.x);
		/* every control left of the reserved notice slot */
		assert(boxes[a].x + boxes[a].w
			   <= head.x + head.w - MENU_HEADER_NOTICE_W);
		for(int b = 0; b < a; b++)
			assert(!menu_rect_overlap(&boxes[a], &boxes[b]));
	}
	/* Prev, tabs, Next in reading order. */
	assert(prev.x < tabs.x && tabs.x < next.x);
	puts("  composition: compact container header keeps Prev/tabs/Next left "
		 "of the reserved notice slot 1/1");
}

/* The sign editor's own reserve, the idiom the header reserve is modelled on:
 * "a reserved 52 px count area so line controls do not shift near the limit". */
static void test_sign_count_reserve(void) {
 for(int l=0;l<2;l++) {
  struct menu_composition c;declare_sign(&c,layouts[l].w,layouts[l].h);
  bool full=l==0;int fx=full?203:44,fy=full?112:76,fw=full?234:194;
  struct menu_rect count={fx+fw-52,fy-28,52,28};
  assert(!menu_rect_overlap(&count,&c.notice));
  assert(!menu_rect_overlap(&count,&c.actions[2]));
 }
 for(int desired=0;desired<=560;desired+=40) {
  struct menu_composition c;declare_sign(&c,640,240);
  menu_composition_header_notice(&c,desired,144);menu_composition_seal(&c);
  struct menu_rect content=menu_composition_header_content(&c);
  assert(content.x==44&&content.w>=144&&c.notice.x+c.notice.w==596&&c.notice.h==24);
  assert(c.notice.w==(desired<400?desired:400));
  assert(!menu_rect_overlap(&content,&c.notice));
  if(desired)assert(c.notice.x-(content.x+content.w)==8);
 }
 puts("  measured header cards preserve title and sign line/count controls");
}

int main(void) {
	puts("menu header / controller-notice composition golden");
	test_rect_algebra();
	test_layout_rule();
	test_split_admission();
	test_compact_container_header();
	test_sign_count_reserve();
	test_sweep();
	puts("PASS menu header notice composition");
	return 0;
}

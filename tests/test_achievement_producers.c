/* ACH-1: real producer paths, not injected stand-ins for the takes.
 * Beta RetroMCP: SlotCrafting.java:17-35, ContainerWorkbench.java:86,
 * ContainerPlayer.java:91; SlotFurnace.java:15-25, ContainerFurnace.java:96;
 * GuiInventory.java:13; EntityPlayerSP.java:176-190; EntityItem.java:110-119.
 * RED before wiring: expect_event's `assert(event_count == 1)` fails on
 * the first workbench take. No build/run is authorized by this brief.
 */
#include "support/owner_swap_harness.h"
#include "game/achievements/achievements.h"
#include "game/gui/local_menu_host.h"
#include "item/inventory_click.h"
#include "network/inventory_crafting.h"
#include "network/inventory_logic.h"

static unsigned event_count;
static struct game_event last_event;
static char stats_dir[128];
static unsigned pickaxe_toasts;
static bool take_right, take_quick;

static void notice_hook(uint8_t slot, int16_t achievement) {
	if(slot == 0 && achievement == 3) pickaxe_toasts++;
}

void __real_game_event_emit(const struct game_event*);
void __wrap_game_event_emit(const struct game_event* ev) {
	if(ev->kind == GE_CRAFT_TAKEN || ev->kind == GE_FURNACE_TAKEN
	   || ev->kind == GE_INVENTORY_OPENED || ev->kind == GE_ITEM_PICKED_UP) {
		event_count++;
		last_event = *ev;
	}
	__real_game_event_emit(ev);
}
/* The native pointer backend is doubled; screen reset/menu opening,
 * inventory logic, identity resolution and the consumer are production.
 * inventory_close_send below also injects a transport refusal case. */
void __wrap_input_pointer_enable(bool enabled) { (void)enabled; }

static void expect_event_case(const char* name, unsigned line,
	struct server_local* s, unsigned owner, uint8_t kind,
	uint16_t id, uint16_t count) {
	printf("CASE %s:%u owner=%u kind=%u id=%u count=%u right=%u quick=%u event_count=%u\n",
		name, line, owner, kind, id, count, take_right, take_quick, event_count);
	fflush(stdout);
	assert(event_count == 1); /* RED: pre-ACH-1 producers emit zero. */
	assert(last_event.kind == kind);
	assert(last_event.authority == (kind == GE_INVENTORY_OPENED
		? GE_AUTH_CLIENT : GE_AUTH_SERVER));
	assert(last_event.who.slot == owner);
	assert(last_event.who.generation == server_local_achievement_epoch(
		s, owner ? s->guest_actor : &s->player));
	if(kind != GE_INVENTORY_OPENED) {
		assert(last_event.u.item.id == id);
		assert(last_event.u.item.count == count);
	}
}
#define expect_event(s, owner, kind, id, count) \
	expect_event_case(__func__, __LINE__, s, owner, kind, id, count)

static void clear_items(struct inventory* inv) {
	memset(inv->items, 0, inv->capacity * sizeof(*inv->items));
	inv->picked_item = (struct item_data){0};
}

static void take(struct inventory* inv, size_t slot, bool right, bool quick) {
	take_right = right;
	take_quick = quick;
	set_inv_slot_t changes;
	set_inv_slot_init(changes);
	if(quick) (void)inventory_quick_move(inv, inv->owner, slot, changes);
	else (void)inventory_action(inv, inv->owner, slot, right, changes);
	set_inv_slot_clear(changes);
}

static void test_crafts(struct server_local* s) {
	/* Every achievement result, both buttons and Quick Move; also the 2x2
	 * workbench recipe through ContainerPlayer's actual matcher. */
	static const uint16_t ids[] = {58,270,61,290,297,354,274,268};
	for(unsigned owner = 0; owner < 2; owner++) {
		struct server_player* p = owner ? s->guest_actor : &s->player;
		struct inventory inv;
		assert(inventory_init(&inv, &inventory_logic_crafting, s, p,
			CRAFTING_SIZE, 0, 0, 0));
		for(size_t i = 0; i < sizeof(ids)/sizeof(*ids); i++)
			for(unsigned mode = 0; mode < 3; mode++) {
				clear_items(&inv);
				/* Test the result-slot decision independently of recipe shape. */
				inv.items[0] = (struct item_data){.id=ids[i],.count=1};
				event_count = 0;
				take(&inv, 0, mode == 1, mode == 2);
				expect_event(s, owner, GE_CRAFT_TAKEN, ids[i], 1);
				assert(inv.picked_item.id == ids[i] || mode == 2);
				achievements_tick();
			}
		for(unsigned quick = 0; quick < 2; quick++) {
			clear_items(&p->inventory);
			for(unsigned k = 1; k <= 4; k++)
				p->inventory.items[k] = (struct item_data){.id=5,.count=1};
			inventory_click_post(&p->inventory, WINDOW_TYPE_INVENTORY, 1, NULL);
			assert(p->inventory.items[0].id == 58);
			event_count = 0; take(&p->inventory, 0, false, quick);
			expect_event(s, owner, GE_CRAFT_TAKEN, 58, 1);
		}
		/* Non-achievement results and failed takes never emit. */
		for(unsigned quick=0;quick<2;quick++) {
			clear_items(&inv); inv.items[0]=(struct item_data){.id=5,.count=4};
			event_count=0; take(&inv,0,false,quick); assert(event_count==0);
		}
		clear_items(&inv); inv.items[0]=(struct item_data){.id=58,.count=2};
		event_count=0; take(&inv,0,false,false);
		expect_event(s,owner,GE_CRAFT_TAKEN,58,2);
		clear_items(&inv); inv.items[0]=(struct item_data){.id=58,.count=1};
		inv.picked_item=(struct item_data){.id=4,.count=1};
		event_count=0; take(&inv,0,false,false); assert(event_count==0);
		clear_items(&inv); inv.items[0]=(struct item_data){.id=58,.count=1};
		for(size_t k=CRAFTING_SLOT_MAIN;k<inv.capacity;k++)
			inv.items[k]=(struct item_data){.id=4,.count=64};
		event_count=0; take(&inv,0,false,true); assert(event_count==0);
		/* NULL-server prediction must remain silent even with an owned test
		 * inventory; production client mirrors also have no logic/user/owner. */
		for(unsigned quick=0;quick<2;quick++) {
			clear_items(&inv); inv.items[0]=(struct item_data){.id=58,.count=1};
			event_count=0;
			if(quick) (void)inventory_transfer(&inv,WINDOW_TYPE_WORKBENCH,0,NULL,NULL);
			else (void)inventory_click_pre(&inv,WINDOW_TYPE_WORKBENCH,0,false,NULL,NULL);
			assert(event_count==0);
		}
		/* The real MP path is the logic-less client mirror and NULL server.
		 * Keep it silent even when the session is explicitly remote. */
		gstate.remote_session=true;
		for(unsigned quick=0;quick<2;quick++) {
			struct item_data data[CRAFTING_SIZE]={0};
			struct inventory mirror={.items=data,.capacity=CRAFTING_SIZE};
			data[0]=(struct item_data){.id=58,.count=1};
			event_count=0;
			(void)inventory_click_predict(&mirror,WINDOW_TYPE_WORKBENCH,0,false,quick);
			assert(event_count==0);
		}
		gstate.remote_session=false;
		inventory_destroy(&inv,p);
	}
	puts("PASS eight craft ids, click/right/shift, personal 2x2, refusals and prediction");
}

static void test_furnace(struct server_local* s) {
	const uint16_t ids[]={265,350};
	for(unsigned owner=0;owner<2;owner++) {
		struct server_player* p=owner?s->guest_actor:&s->player;
		/* Unbound, hand-built window: exercises shipping furnace pre/post
		 * callbacks without a backing TE (same supported fixture mode). */
		struct inventory inv;
		assert(inventory_init(&inv,NULL,s,p,FURNACE_SIZE,0,0,0));
		inv.logic=&inventory_logic_furnace; /* bypass backing-TE on_create only */
		/* Beta RetroMCP Item.java:102 / ItemFood.java:11: cooked fish
		 * stacks to ONE. Container.java:134-143 cannot merge it onto an
		 * occupied cursor; only iron has a successful occupied-cursor take. */
		for(size_t i=0;i<2;i++) for(unsigned mode=0;mode<(i==0?4:3);mode++) {
			uint16_t count=i==0?4:1;
			clear_items(&inv); inv.items[2]=(struct item_data){.id=ids[i],.count=count};
			if(mode==3) inv.picked_item=(struct item_data){.id=ids[i],.count=2};
			event_count=0; take(&inv,2,mode==1,mode==2);
			expect_event(s,owner,GE_FURNACE_TAKEN,ids[i],mode==1?(count+1)/2:count);
			assert(inv.items[2].count==(mode==1?count/2:0));
			if(mode!=2) {
				assert(inv.picked_item.id==ids[i]);
				assert(inv.picked_item.count==(mode==3?6:mode==1?(count+1)/2:count));
			}
		}
		for(unsigned right=0;right<2;right++) {
			clear_items(&inv); inv.items[2]=(struct item_data){.id=350,.count=1};
			inv.picked_item=(struct item_data){.id=350,.count=1};
			event_count=0; take(&inv,2,right,false);
			assert(event_count==0);
			assert(inv.items[2].id==350 && inv.items[2].count==1);
			assert(inv.picked_item.id==350 && inv.picked_item.count==1);
		}
		for(unsigned quick=0;quick<2;quick++) {
			clear_items(&inv); inv.items[2]=(struct item_data){.id=266,.count=1};
			event_count=0; take(&inv,2,false,quick); assert(event_count==0);
		}
		clear_items(&inv); inv.items[2]=(struct item_data){.id=265,.count=1};
		inv.picked_item=(struct item_data){.id=265,.count=64};
		event_count=0; take(&inv,2,false,false); assert(event_count==0);
		clear_items(&inv); inv.items[2]=(struct item_data){.id=265,.count=4};
		for(size_t k=FURNACE_SLOT_MAIN;k<inv.capacity;k++)
			inv.items[k]=(struct item_data){.id=4,.count=64};
		event_count=0; take(&inv,2,false,true); assert(event_count==0);
		inv.items[inv.capacity-1]=(struct item_data){.id=265,.count=63};
		event_count=0; take(&inv,2,false,true);
		expect_event(s,owner,GE_FURNACE_TAKEN,265,1);
		assert(inv.items[2].count==3); /* retry moved nothing: still one event */
		for(unsigned quick=0;quick<2;quick++) {
			struct item_data data[FURNACE_SIZE]={0};
			struct inventory mirror={.items=data,.capacity=FURNACE_SIZE};
			data[2]=(struct item_data){.id=265,.count=4};
			event_count=0;
			gstate.remote_session=true;
			(void)inventory_click_predict(&mirror,WINDOW_TYPE_FURNACE,2,false,quick);
			assert(event_count==0);
			gstate.remote_session=false;
		}
		inventory_destroy(&inv,p);
	}
	puts("PASS iron/fish furnace empty/occupied cursor, right/shift, refusal and prediction");
}

static void test_open(struct server_local* s) {
	for(unsigned owner=0;owner<2;owner++) {
		struct client_session* c=&gstate.players[owner];
		assert(c->achievement_epoch==server_local_achievement_epoch(s,owner?s->guest_actor:&s->player));
		event_count=0;
		assert(local_menu_host_open(c,owner,WINDOWC_INVENTORY,640,240));
		expect_event(s,owner,GE_INVENTORY_OPENED,0,0);
		local_menu_host_inventory_closed(c);
		local_menu_host_reset(c);
	}
	/* These are separate logical opens, rather than presentation resets. */
	for(unsigned height=240;height<=480;height+=240) {
		gstate.players[0].current_screen=&screen_inventory;
		event_count=0; screen_inventory.reset(&screen_inventory,640,height);
		expect_event(s,0,GE_INVENTORY_OPENED,0,0);
		screen_set(&screen_ingame);
	}
	static const enum window_type types[]={WINDOW_TYPE_CHEST,WINDOW_TYPE_WORKBENCH,WINDOW_TYPE_FURNACE};
	static const size_t sizes[]={CHEST_SIZE,CRAFTING_SIZE,FURNACE_SIZE};
	for(unsigned i=0;i<3;i++) {
		struct window_container wc;
		assert(windowc_create(&wc,types[i],sizes[i]));
		gstate.players[0].windows[1]=&wc;
		event_count=0; assert(local_menu_host_open(&gstate.players[0],0,1,640,240));
		assert(event_count==0); local_menu_host_reset(&gstate.players[0]);
		gstate.players[0].windows[1]=NULL; windowc_destroy(&wc);
	}
	/* MP decision (b): leave even inventory-open remote behavior unchanged. */
	gstate.remote_session=true;
	event_count=0; screen_inventory.reset(&screen_inventory,640,480);
	assert(event_count==0);
	for(unsigned owner=0;owner<2;owner++) {
		assert(local_menu_host_open(&gstate.players[owner],owner,WINDOWC_INVENTORY,640,240));
		assert(event_count==0); local_menu_host_reset(&gstate.players[owner]);
	}
	gstate.remote_session=false;
	gstate.config_creative=true;
	event_count=0; screen_creative_picker.reset(&screen_creative_picker,640,480);
	assert(event_count==0);
	screen_inventory.reset(&screen_inventory,640,480); assert(event_count==0);
	for(unsigned owner=0;owner<2;owner++) {
		assert(local_menu_host_open(&gstate.players[owner],owner,WINDOWC_INVENTORY,640,240));
		assert(event_count==0); local_menu_host_reset(&gstate.players[owner]);
	}
	gstate.config_creative=false;
	puts("PASS per-owner inventory GUI, compact/full, containers, creative and MP exclusion");
}

static void test_inventory_presentation(struct server_local* s) {
	/* Beta RetroMCP GuiInventory.java:10-14: the constructor awards once;
	 * initGui (presentation setup) does not award. Drive the real conversion
	 * entry points used by main.c, including its compact-to-full reset. */
	struct client_session* c=&gstate.players[0];
	screen_set(&screen_ingame); local_menu_host_reset(c);
	event_count=0;
	assert(clin_show_window(WINDOWC_INVENTORY));
	expect_event(s,0,GE_INVENTORY_OPENED,0,0);
	assert(screen_split_container_base(WINDOWC_INVENTORY,640,240));
	assert(event_count==1); /* RED ACH-1b: compact conversion emits event #2. */
	local_menu_host_reset(c);
	assert(clin_show_window(WINDOWC_INVENTORY));
	assert(event_count==1); /* RED ACH-1b: full conversion emits event #3. */
	screen_set(&screen_ingame);
	assert(clin_show_window(WINDOWC_INVENTORY));
	assert(event_count==2);
	assert(last_event.who.slot==0);
	screen_set(&screen_ingame); local_menu_host_reset(c);
	puts("PASS inventory open -> compact -> full once, close -> reopen twice");
}

struct inventory_close_sender {
	struct server_rpc_source source;
	bool accept;
};
static bool inventory_close_send(void* user, struct server_rpc* rpc) {
	struct inventory_close_sender* sender=user;
	assert(rpc->type==SRPC_WINDOW_CLOSE);
	assert(rpc->payload.window_close.window==WINDOWC_INVENTORY);
	/* Refusal is injected at the sender boundary; accepted closes use the
	 * real per-owner transport. */
	return sender->accept && svin_rpc_send_from(sender->source,rpc);
}

static void test_inventory_owner_close(struct server_local* s) {
	struct container_input close={.close=true};
	struct container_glyphs glyphs={0};
	for(unsigned owner=0;owner<2;owner++) {
		struct client_session* c=&gstate.players[owner];
		local_menu_host_inventory_closed(c); local_menu_host_reset(c);
		clear_items(windowc_get_latest(c->windows[WINDOWC_INVENTORY]));
	}
	event_count=0;
	assert(local_menu_host_open(&gstate.players[0],0,WINDOWC_INVENTORY,640,240));
	expect_event(s,0,GE_INVENTORY_OPENED,0,0);
	assert(local_menu_host_open(&gstate.players[1],1,WINDOWC_INVENTORY,640,240));
	assert(event_count==2 && last_event.who.slot==1);
	for(unsigned owner=0;owner<2;owner++) {
		struct client_session* c=&gstate.players[owner];
		struct inventory_close_sender sender={.source=owner?guest_source(s)
			:(struct server_rpc_source){0}};
		assert(!local_menu_host_update(c,&close,&glyphs,0,inventory_close_send,&sender));
		assert(local_menu_host_open(c,owner,WINDOWC_INVENTORY,640,240));
		assert(event_count==2+owner); /* refused close keeps the logical open */
		sender.accept=true;
		assert(local_menu_host_update(c,&close,&glyphs,0,inventory_close_send,&sender));
		drain_server(s);
		assert(local_menu_host_open(c,owner,WINDOWC_INVENTORY,640,240));
		assert(event_count==3+owner && last_event.who.slot==owner);
		unsigned other=1-owner;
		assert(local_menu_host_open(&gstate.players[other],other,WINDOWC_INVENTORY,640,240));
		assert(event_count==3+owner); /* closing one owner never retires the other */
	}
	/* A server-initiated guest return resets its compact presentation. The
	 * next GUI frame retires the logical open before a new inventory request. */
	struct client_session* guest=&gstate.players[1];
	assert(client_session_local_reply(guest,guest->local_generation,
		&(struct client_rpc){.type=CRPC_RETURN_INGAME}));
	struct input_player_frame neutral={.connected=true};
	struct inventory_close_sender sender={.source=guest_source(s),.accept=true};
	assert(!local_menu_host_frame(guest,1,&neutral,0,0,640,240,0,inventory_close_send,&sender));
	assert(local_menu_host_open(guest,1,WINDOWC_INVENTORY,640,240));
	assert(event_count==5 && last_event.who.slot==1);
	for(unsigned owner=0;owner<2;owner++) {
		local_menu_host_inventory_closed(&gstate.players[owner]);
		local_menu_host_reset(&gstate.players[owner]);
	}
	puts("PASS independent P1/P2 opens, refused/accepted close and guest server return");
}

static void test_inventory_teardown(struct server_local* s) {
	/* Exercise the production session/window teardown used by world reset
	 * without retiring either admitted fixture owner needed by later tests. */
	struct client_session c={.visible_window=-1,
		.achievement_epoch=gstate.players[1].achievement_epoch};
	for(unsigned reopen=0;reopen<2;reopen++) {
		struct window_container* wc=malloc(sizeof(*wc));
		assert(wc && windowc_create(wc,WINDOW_TYPE_INVENTORY,INVENTORY_SIZE));
		c.windows[WINDOWC_INVENTORY]=wc;
		event_count=0;
		assert(local_menu_host_open(&c,1,WINDOWC_INVENTORY,640,240));
		expect_event(s,1,GE_INVENTORY_OPENED,0,0);
		client_session_clear_windows(&c);
		assert(!c.menu_host.inventory_open);
	}
	puts("PASS session/world window teardown clears the logical inventory open");
}

static void test_guided(struct server_local* s) {
	uint16_t index=RECIPE_CRAFT_INDEX_NONE;
	for(size_t k=0;k<array_recipe_size(recipes_crafting);k++)
		if(array_recipe_cget(recipes_crafting,k)->result.id==58) {index=(uint16_t)k;break;}
	assert(index!=RECIPE_CRAFT_INDEX_NONE);
	for(unsigned owner=0;owner<2;owner++) {
		struct inventory* inv=&(owner?s->guest_actor:&s->player)->inventory;
		clear_items(inv); inv->items[INVENTORY_SLOT_HOTBAR]=(struct item_data){.id=5,.count=4};
		event_count=0;
		assert(inventory_crafting_one_batch_preflight(inv,WINDOW_TYPE_INVENTORY,index)==RECIPE_CRAFT_OK);
		assert(event_count==0);
		assert(inventory_crafting_one_batch(inv,WINDOW_TYPE_INVENTORY,index,s,NULL)==RECIPE_CRAFT_OK);
		expect_event(s,owner,GE_CRAFT_TAKEN,58,1);
		clear_items(inv); inv->items[INVENTORY_SLOT_HOTBAR]=(struct item_data){.id=5,.count=4};
		event_count=0;
		assert(inventory_crafting_one_batch(inv,WINDOW_TYPE_INVENTORY,index,NULL,NULL)==RECIPE_CRAFT_OK);
		assert(event_count==0);
	}
	puts("PASS guided committed batch once; preflight/client batch silent");
}

static void test_pickups(struct server_local* s) {
	/* All eight collecting handlers. Sign's handler always returns false,
	 * so it has no pickup to award. EntityItem.java:110-119 requires success. */
	struct inventory_logic* logic[]={&inventory_logic_player,&inventory_logic_crafting,
		&inventory_logic_furnace,&inventory_logic_chest,&inventory_logic_double_chest,
		&inventory_logic_dispenser,&inventory_logic_iron_chest,&inventory_logic_minecart_chest};
	const size_t capacity[]={INVENTORY_SIZE,CRAFTING_SIZE,FURNACE_SIZE,CHEST_SIZE,
		90,DISPENSER_SIZE,IRON_CHEST_SIZE,CHEST_SIZE};
	const uint16_t ids[]={17,334};
	for(unsigned owner=0;owner<2;owner++) for(unsigned n=0;n<8;n++) {
		struct server_player* p=owner?s->guest_actor:&s->player;
		/* Direct handler fixture: no backing/container admission claim. */
		struct item_data data[90]={0};
		struct inventory inv={.items=data,.capacity=capacity[n],.logic=logic[n],.user=s,.owner=p};
		for(unsigned i=0;i<2;i++) {
			clear_items(&inv);
			struct item_data item={.id=ids[i],.count=4};
			event_count=0;
			assert(logic[n]->on_collect(&inv,p,&item));
			assert(item.count==0);
			expect_event(s,owner,GE_ITEM_PICKED_UP,ids[i],4);
			/* Only one slot has room for one item; collection partially mutates
			 * the stack but returns false, just as Beta's final stalled loop. */
			for(size_t k=0;k<inv.capacity;k++) data[k]=(struct item_data){.id=4,.count=64};
			data[inv.capacity-1]=(struct item_data){.id=ids[i],.count=63};
			item=(struct item_data){.id=ids[i],.count=4};
			event_count=0;
			assert(!logic[n]->on_collect(&inv,p,&item));
			assert(item.count==3 && event_count==0);
		}
		clear_items(&inv);
		struct item_data item={.id=262,.count=1};
		event_count=0; assert(logic[n]->on_collect(&inv,p,&item));
		assert(event_count==0); /* arrow recovery is not an achievement pickup */
	}
	puts("PASS wood/leather pre-drain capture, eight handlers, both owners, partial failure silent");
}

static void test_parent(struct server_local* s) {
	/* Reset only the consumer to fresh bound records, keeping real actors and
	 * their admitted epochs. Beta EntityPlayerSP.java:176-190 drops children. */
	assert(achievements_shutdown()); assert(achievements_init(stats_dir));
	for(unsigned owner=0;owner<2;owner++) {
		struct server_player* p=owner?s->guest_actor:&s->player;
		assert(achievements_bind(owner,server_local_achievement_epoch(s,p),
			owner?"parent2":"parent1",ACH_MODE_LOCAL_WORLD)==ACH_BIND_OK);
		clear_items(&p->inventory); p->inventory.items[0]=(struct item_data){.id=270,.count=1};
		event_count=0; take(&p->inventory,0,false,false);
		expect_event(s,owner,GE_CRAFT_TAKEN,270,1); achievements_tick();
		assert(!ach_unlocked(achievements_debug_profile(owner),3));
	}
	/* Host opening awards only host; guest must keep its own locked record. */
	event_count=0; screen_inventory.reset(&screen_inventory,640,480);
	achievements_tick();
	assert(ach_unlocked(achievements_debug_profile(0),0));
	assert(!ach_unlocked(achievements_debug_profile(1),0));
	struct inventory* inv=&s->player.inventory;
	clear_items(inv);
	struct item_data wood={.id=17,.count=1};
	assert(inv->logic->on_collect(inv,&s->player,&wood)); achievements_tick();
	assert(ach_unlocked(achievements_debug_profile(0),1));
	assert(!ach_unlocked(achievements_debug_profile(0),2));
	clear_items(inv); inv->items[0]=(struct item_data){.id=270,.count=1};
	take(inv,0,false,false); achievements_tick();
	assert(!ach_unlocked(achievements_debug_profile(0),3)); /* Benchmarking alone is missing */
	clear_items(inv); inv->items[0]=(struct item_data){.id=58,.count=1};
	take(inv,0,false,false); achievements_tick();
	assert(ach_unlocked(achievements_debug_profile(0),2));
	achievements_debug_notice_hook(notice_hook);
	for(unsigned repeat=0;repeat<2;repeat++) {
		clear_items(inv); inv->items[0]=(struct item_data){.id=270,.count=1};
		event_count=0; take(inv,0,false,false);
		expect_event(s,0,GE_CRAFT_TAKEN,270,1); achievements_tick();
	}
	assert(ach_unlocked(achievements_debug_profile(0),3));
	assert(stats_counter_total(&achievements_debug_profile(0)->ach[3])==2);
	assert(!ach_unlocked(achievements_debug_profile(1),3));
	assert(pickaxe_toasts==1); achievements_debug_notice_hook(NULL);
	puts("PASS parent gate through module and separate P1/P2 records");
}

int main(void) {
	test_fixture_dir("test_achievement_producers",stats_dir,sizeof(stats_dir));
#ifdef _WIN32
	assert(_mkdir(stats_dir)==0);
#else
	assert(mkdir(stats_dir,0755)==0);
#endif
	assert(achievements_init(stats_dir));
	assert(achievements_set_profiles("producer1","producer2"));
	blocks_init(); items_init(); recipe_init();
	assert(clin_init()&&svin_init());
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type=CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed,NULL);
	struct server_local* s=admitted_fixture(); achievements_tick();
	client_session_achievements_sync(&gstate.players[0],0);
	client_session_achievements_sync(&gstate.players[1],1);
	assert(achievements_debug_profile(0)&&achievements_debug_profile(1));
	test_crafts(s); test_furnace(s); test_open(s); test_inventory_presentation(s);
	test_inventory_owner_close(s); test_inventory_teardown(s);
	test_guided(s); test_pickups(s); test_parent(s);
	assert(achievements_shutdown()); teardown(s);
	test_fixture_dir_cleanup(stats_dir);
	puts("PASS test_achievement_producers");
	return 0;
}

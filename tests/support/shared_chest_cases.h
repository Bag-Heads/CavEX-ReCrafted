/* A1b: fixture rows and hand-built windows name their world explicitly (the
 * slot-0 fallbacks for unstamped rows / unbound windows are gone). */
#define A1B_STAMP(s, kind, k, w) \
	((s)->te_owners.kind##_slot[k] = (w)->world_index, \
	 (s)->te_owners.kind##_gen[k] = (w)->generation)
#define A1B_BIND(player, w) ((player)->window_world = server_world_handle_of(w))
/* Included by the live server/world lifecycle harness. Exercises the production
 * actor-bound entry point, real chest callbacks/backing rows, and client sessions. */
struct chest_client_capture {
	struct client_session session;
	unsigned accepts, rejects, replies;
	uint16_t expected_action;
};

static void chest_client_reply(const struct client_rpc* call, void* user) {
	struct chest_client_capture* capture = user;
	capture->replies++;
	if(call->type == CRPC_OPEN_WINDOW || call->type == CRPC_REOPEN_WINDOW) {
		bool reopen = call->type == CRPC_REOPEN_WINDOW;
		uint8_t id = reopen ? call->payload.window_reopen.window : call->payload.window_open.window;
		enum window_type type = reopen ? call->payload.window_reopen.type : call->payload.window_open.type;
		size_t count = reopen ? call->payload.window_reopen.slot_count : call->payload.window_open.slot_count;
		struct window_container** wc = &capture->session.windows[id];
		if(*wc) { windowc_destroy(*wc); free(*wc); }
		*wc = malloc(sizeof(**wc));
		assert(*wc && windowc_create(*wc, type, count));
		return;
	}
	if(call->type == CRPC_WINDOW_TRANSACTION) {
		assert(call->payload.window_transaction.action_id == capture->expected_action);
		if(call->payload.window_transaction.accepted) capture->accepts++;
		else capture->rejects++;
	}
	assert(client_session_window_reply(&capture->session, call));
}

struct chest_response_target {
	struct server_local* server;
	struct server_player* actor;
};
static void chest_transaction_response(struct server_rpc* rpc, void* user) {
	struct chest_response_target* target = user;
	assert(rpc->type == SRPC_WINDOW_TRANSACTION);
	server_local_process_window_for(target->server, target->actor, rpc);
}

static void chest_actor_click(struct server_local* server,
	struct server_player* actor, uint8_t window, uint8_t slot, bool quick,
	bool right, struct item_data expected) {
	struct inventory* inv = actor->active_inventory;
	struct chest_client_capture* client = inv->reply_user;
	struct window_container* wc = client ? client->session.windows[window] : NULL;
	uint16_t id = 42;
	if(inv->owner == actor && wc && wc->slot_count == inv->capacity
	   && slot < inv->capacity && !inv->transaction_pending) {
		/* Author real prediction before dispatch; success no longer echoes it. */
		assert(quick ? windowc_new_quick_action(wc, &id, slot)
			: windowc_new_action(wc, &id, right, slot));
	}
	if(client && inv->owner == actor) client->expected_action = id;
	struct server_rpc rpc = {.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {.window = window, .slot = slot,
			.quick_move = quick, .right_click = right, .expected = expected, .action_id = id}};
	server_local_process_window_for(server, actor, &rpc);
	if(inv->owner == actor && inv->transaction_pending) {
		/* Rejection must finish its matching response before the next click.
		 * The wire/gate golden separately tests clicks while this is pending. */
		struct chest_response_target target = {server, actor};
		svin_process_messages(chest_transaction_response, &target, false);
		assert(!inv->transaction_pending);
	}
}

static void chest_actor_close(struct server_local* server,
	struct server_player* actor, uint8_t window) {
	struct server_rpc rpc = {.type = SRPC_WINDOW_CLOSE, .payload.window_close.window = window};
	server_local_process_window_for(server, actor, &rpc);
	assert(actor->active_inventory == &actor->inventory);
}

static void test_shared_chest_actors(void) {
	struct server_local server = {0};
	struct server_player guest = {0};
	struct server_player* actors[] = {&server.player, &guest};
	struct chest_client_capture clients[2] = {0};
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	server_world_create(&server.world, level, WORLD_DIM_OVERWORLD, 31337, true);
	server.worlds[0].session = &server; /* A1a review M4: the owning session (callbacks, services) */
	if(!server_local_te_tables_init(&server)) abort(); /* IMP-TE-CAPS */
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&server.world, 0, 0, &chunk) && chunk);
	dict_entity_init(server.entities);
	server.rand_src.seed = 123;
	for(int p = 0; p < 2; p++) {
		struct server_player* actor = actors[p];
		assert(inventory_create(&actor->inventory, &inventory_logic_player,
			&server, actor, INVENTORY_SIZE, 0, 0, 0));
		actor->inventory.reply = chest_client_reply;
		actor->inventory.reply_user = &clients[p];
		actor->active_inventory = &actor->inventory;
		actor->has_pos = actor->finished_loading = true;
		actor->health = MAX_PLAYER_HEALTH;
		actor->x = 4 + p * 4; actor->y = 72; actor->z = 4;
		actor->rx = p * 180;
		clients[p].session.windows[0] = malloc(sizeof(struct window_container));
		assert(clients[p].session.windows[0]);
		assert(windowc_create(clients[p].session.windows[0], WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
	}
	/* An unbound guest must never fall back to Player 1's reply queue. */
	guest.inventory.reply = NULL;
	assert(!server_local_open_inventory_for(&server,&guest,&inventory_logic_chest,
		CHEST_SIZE,4,70,4,WINDOWC_CHEST,WINDOW_TYPE_CHEST));
	guest.inventory.reply = chest_client_reply;
	guest.active_inventory = &server.player.inventory;
	unsigned foreign_before = clients[0].replies + clients[1].replies;
	chest_actor_click(&server,&guest,WINDOWC_INVENTORY,9,false,false,(struct item_data){0});
	assert(clients[0].replies + clients[1].replies == foreign_before);
	guest.active_inventory = &guest.inventory;
	for(int large = 0; large < 2; large++) for(int order = 0; order < 2; order++) {
		for(int k = 0; k < 2; k++) {
			if(!large && !order)
				assert(server_world_set_block(&server.worlds[0], 4+k, 70, 4, (struct block_data){.type=BLOCK_CHEST}));
			server.chest_pos[k] = (struct complex_block_pos){4+k,70,4};
			A1B_STAMP(&server, chest, k, &server.worlds[0]);
			memset(server.chest_items[k], 0, sizeof(server.chest_items[k]));
			inventory_clear(&actors[k]->inventory);
			actors[k]->inventory.items[INVENTORY_SLOT_ARMOR] = (struct item_data){.id=306+k,.count=1};
		}
		server.chest_items[0][0] = (struct item_data){.id=ITEM_DIAMOND,.count=12};
		guest.inventory.items[INVENTORY_SLOT_MAIN] = (struct item_data){.id=BLOCK_COBBLESTONE,.count=5};
		uint8_t window = large ? WINDOWC_IRON_CHEST : WINDOWC_CHEST;
		int main = large ? IRON_CHEST_SLOT_MAIN : CHEST_SLOT_MAIN;
		for(int p = 0; p < 2; p++) {
			bool opened = large ? server_local_open_linked_inventory_for(&server, actors[p],
				&inventory_logic_double_chest, IRON_CHEST_SIZE, 4,70,4,5,70,4,window,WINDOW_TYPE_CHEST)
				: server_local_open_inventory_for(&server, actors[p], &inventory_logic_chest,
					CHEST_SIZE,4,70,4,window,WINDOW_TYPE_CHEST);
			assert(opened);
		}
		struct inventory* a = actors[0]->active_inventory;
		struct inventory* b = actors[1]->active_inventory;
		struct item_data stale = a->items[0];
		unsigned rejects = clients[1].rejects, accepts = clients[1].accepts;
		chest_actor_click(&server, actors[0],window,0,true,false,stale);
		assert(!server.chest_items[0][0].id && !b->items[0].id);
		assert(!windowc_get_latest(clients[1].session.windows[window])->items[0].id);
		assert(clients[1].accepts == accepts); /* peer update is not our ACK */
		chest_actor_click(&server, actors[1],window,0,true,false,stale);
		assert(clients[1].rejects == rejects+1);
		assert(a->items[main+35].count == 12 && !b->items[main+35].id);
		chest_actor_click(&server, actors[1],window,(uint8_t)main,true,false,b->items[main]);
		assert(a->items[0].count == 5 && server.chest_items[0][0].count == 5);
		chest_actor_click(&server, actors[0],window,0,false,true,a->items[0]);
		chest_actor_click(&server, actors[1],window,0,false,false,b->items[0]);
		assert(a->picked_item.count == 3 && b->picked_item.count == 2);
		assert(windowc_get_latest(clients[0].session.windows[window])->picked_item.count == 3);
		assert(windowc_get_latest(clients[1].session.windows[window])->picked_item.count == 2);
		chest_actor_click(&server, actors[0],window,1,false,false,(struct item_data){0});
		int drops_before = count_item_entities(&server, BLOCK_COBBLESTONE, 1);
		uint16_t drop_id;
		assert(windowc_new_drop_action(clients[1].session.windows[window], &drop_id, true));
		clients[1].expected_action = drop_id;
		struct server_rpc drop = {.type=SRPC_WINDOW_DROP,
			.payload.window_drop={.window=window,.right=true,.action_id=drop_id}};
		server_local_process_window_for(&server, &guest, &drop);
		assert(b->picked_item.count == 1 && count_item_entities(&server, BLOCK_COBBLESTONE, 1)==drops_before+1);
		struct entity** dropped = dict_entity_get(server.entities, server.next_entity_id);
		assert(dropped && (*dropped)->pos[0] == guest.x && (*dropped)->vel[2] < 0);
		assert((*dropped)->data.item.pickup_delay == 40);
		/* Same window/action numbers, wrong window, bad slot, and handoff gates. */
		rejects = clients[1].rejects;
		chest_actor_click(&server,&guest,window,255,true,false,stale);
		chest_actor_click(&server,&guest,WINDOWC_INVENTORY,0,true,false,stale);
		assert(clients[1].rejects == rejects+1); /* wrong window is ignored */
		unsigned replies = clients[0].replies;
		server.input_ack_pending = 9;
		chest_actor_click(&server,actors[0],window,1,true,false,a->items[1]);
		assert(clients[0].replies == replies && a->items[1].count == 3);
		server.input_ack_pending = 0;
		/* A failed cursor spawn keeps the guest's window and reopens only that
		 * client's snapshot, including its carried item, before a successful retry. */
		server_local_test_fail_next_entity_alloc();
		struct server_rpc close = {.type=SRPC_WINDOW_CLOSE,.payload.window_close.window=window};
		server_local_process_window_for(&server,&guest,&close);
		assert(guest.active_inventory == b && b->picked_item.count == 1);
		assert(windowc_get_latest(clients[1].session.windows[window])->picked_item.count == 1);
		assert(clients[0].replies == replies);
		chest_actor_close(&server, actors[order],window);
		chest_actor_close(&server, actors[1-order],window);
		assert(!server.chest_views && server.chest_items[0][1].count == 3);
		assert(count_item_entities(&server, BLOCK_COBBLESTONE, 1) == drops_before+2);
		assert(actors[0]->inventory.items[INVENTORY_SLOT_HOTBAR+8].count == 12);
		assert(!actors[1]->inventory.items[INVENTORY_SLOT_MAIN].id);
		for(int p=0;p<2;p++) assert(actors[p]->inventory.items[INVENTORY_SLOT_ARMOR].id == 306+p);
	}
	puts("PASS shared chest actors: single/large, stale Quick Move, private cursors/ACKs, either close order, conserved items");
	/* Views may map the same block to different slot offsets/window IDs. */
	memset(server.chest_items[0],0,sizeof(server.chest_items[0]));
	memset(server.chest_items[1],0,sizeof(server.chest_items[1]));
	server.chest_items[0][0]=(struct item_data){.id=BLOCK_LOG,.count=3};
	server.chest_items[1][0]=(struct item_data){.id=ITEM_COAL,.count=6};
	assert(server_local_open_linked_inventory_for(&server,actors[0],&inventory_logic_double_chest,
		IRON_CHEST_SIZE,4,70,4,5,70,4,WINDOWC_IRON_CHEST,WINDOW_TYPE_CHEST));
	assert(server_local_open_inventory_for(&server,&guest,&inventory_logic_chest,
		CHEST_SIZE,5,70,4,WINDOWC_CHEST,WINDOW_TYPE_CHEST));
	chest_actor_click(&server,&guest,WINDOWC_CHEST,0,true,false,guest.active_inventory->items[0]);
	assert(actors[0]->active_inventory->items[0].count==3);
	assert(!actors[0]->active_inventory->items[27].id && !server.chest_items[1][0].id);
	assert(!windowc_get_latest(clients[0].session.windows[WINDOWC_IRON_CHEST])->items[27].id);
	chest_actor_close(&server,&guest,WINDOWC_CHEST);
	chest_actor_close(&server,actors[0],WINDOWC_IRON_CHEST);
	puts("PASS shared chest keys: a single view maps into the other actor's second large-chest half");

	/* Actual queue dispatch: identical IDs belong to different actor cursors.
	 * No ambient player swap and no direct render-thread server call. */
	struct server_rpc_source guest_source;
	assert(server_local_bind_guest(&server, &guest, &guest_source));
	assert(!server_local_bind_guest(&server, &guest, &guest_source));
	for(int p=0;p<2;p++) {
		inventory_clear(&actors[p]->inventory);
		actors[p]->inventory.items[INVENTORY_SLOT_MAIN] = (struct item_data){.id=ITEM_COAL+p,.count=8+p};
	}
	struct server_rpc queued_click={.type=SRPC_WINDOW_CLICK,
		.payload.window_click={.window=0,.slot=INVENTORY_SLOT_MAIN,.action_id=42}};
	clients[0].expected_action = clients[1].expected_action = 42;
	queued_click.payload.window_click.expected = server.player.inventory.items[INVENTORY_SLOT_MAIN];
	assert(svin_rpc_send(&queued_click));
	queued_click.payload.window_click.expected = guest.inventory.items[INVENTORY_SLOT_MAIN];
	assert(svin_rpc_send_from(guest_source,&queued_click));
	server_local_test_process_messages(&server);
	assert(server.player.inventory.picked_item.id==ITEM_COAL && server.player.inventory.picked_item.count==8);
	assert(guest.inventory.picked_item.id==ITEM_COAL+1 && guest.inventory.picked_item.count==9);
	assert(!server_local_unbind_guest(&server,&guest_source)); /* cursor still owned */
	queued_click.payload.window_click.expected = (struct item_data){0};
	assert(svin_rpc_send(&queued_click));
	assert(svin_rpc_send_from(guest_source,&queued_click));
	server_local_test_process_messages(&server); /* put both back */
	assert(!guest.inventory.picked_item.id && !server.player.inventory.picked_item.id);
	assert(server_local_open_inventory_for(&server,&guest,&inventory_logic_chest,
		CHEST_SIZE,4,70,4,WINDOWC_CHEST,WINDOW_TYPE_CHEST));
	assert(!server_local_unbind_guest(&server,&guest_source)); /* active view still owned */
	chest_actor_close(&server,&guest,WINDOWC_CHEST);
	/* Stale request waits in the pending tier across detach/rejoin. */
	struct server_rpc filler={.type=SRPC_WINDOW_DROP,.payload.window_drop.window=255};
	for(int k=0;k<16;k++) assert(svin_rpc_send(&filler));
	assert(svin_rpc_send_from(guest_source,&queued_click));
	assert(svin_pending_count()==1);
	assert(server_local_unbind_guest(&server,&guest_source));
	struct server_rpc_source new_source;
	assert(server_local_bind_guest(&server,&guest,&new_source));
	assert(new_source.generation!=guest_source.generation);
	assert(!server_local_unbind_guest(&server,&guest_source));
	unsigned replies_before=clients[1].replies;
	server_local_test_process_messages(&server);
	svin_rpc_flush_pending();
	server_local_test_process_messages(&server);
	assert(clients[1].replies==replies_before && !guest.inventory.picked_item.id);
	queued_click.payload.window_click.expected = guest.inventory.items[INVENTORY_SLOT_MAIN];
	assert(svin_rpc_send_from(new_source,&queued_click));
	server_local_test_process_messages(&server);
	assert(guest.inventory.picked_item.count==9 && !server.player.inventory.picked_item.id);
	queued_click.payload.window_click.expected = (struct item_data){0};
	assert(svin_rpc_send_from(new_source,&queued_click));
	server_local_test_process_messages(&server);
	assert(server_local_unbind_guest(&server,&new_source));
	puts("PASS guest ownership: queued actors isolated; stale rejoin generation ignored; live cursor/view prevents detach");

	/* Break the second half with two open views. Even when the batch allocation
	 * fails, exactly one view retains ownership of the orphaned four-item stack. */
	for(int oom=0;oom<2;oom++) {
		if(oom) assert(server_world_set_block(&server.worlds[0],5,70,4,(struct block_data){.type=BLOCK_CHEST}));
		server.chest_pos[1]=(struct complex_block_pos){5,70,4}; A1B_STAMP(&server, chest, 1, &server.worlds[0]);
		memset(server.chest_items[0],0,sizeof(server.chest_items[0]));
		memset(server.chest_items[1],0,sizeof(server.chest_items[1]));
		server.chest_items[0][0]=(struct item_data){.id=ITEM_COAL,.count=7};
		server.chest_items[1][0]=(struct item_data){.id=BLOCK_GRAVEL,.count=4};
		for(int p=0;p<2;p++) assert(server_local_open_linked_inventory_for(&server,actors[p],
			&inventory_logic_double_chest,IRON_CHEST_SIZE,4,70,4,5,70,4,WINDOWC_IRON_CHEST,WINDOW_TYPE_CHEST));
		int before=count_item_entities(&server,BLOCK_GRAVEL,4);
		if(oom) server_local_test_fail_next_container_drop();
		struct block_data chest={.type=BLOCK_CHEST};
		struct block_info info={.x=5,.y=70,.z=4,.block=&chest};
		block_chest.onBreak(&server,&info);
		assert(server_world_set_block(&server.worlds[0],5,70,4,(struct block_data){.type=BLOCK_AIR}));
		assert(actors[0]->active_inventory->invalidated && actors[1]->active_inventory->invalidated);
		assert(count_item_entities(&server,BLOCK_GRAVEL,4)==before+(oom?0:1));
		unsigned rejects=clients[1].rejects;
		chest_actor_click(&server,&guest,WINDOWC_IRON_CHEST,0,true,false,guest.active_inventory->items[0]);
		assert(clients[1].rejects==rejects+1);
		/* Close the non-owner first to expose accidental duplicated spills. */
		chest_actor_close(&server,actors[0],WINDOWC_IRON_CHEST);
		if(oom) {
			assert(server_world_set_block(&server.worlds[0],5,70,4,(struct block_data){.type=BLOCK_CHEST}));
			server.chest_pos[1]=(struct complex_block_pos){5,70,4}; A1B_STAMP(&server, chest, 1, &server.worlds[0]);
			server.chest_items[1][0]=(struct item_data){.id=ITEM_GOLD,.count=20};
			assert(!server_local_open_inventory_for(&server,actors[0],&inventory_logic_chest,
				CHEST_SIZE,5,70,4,WINDOWC_CHEST,WINDOW_TYPE_CHEST));
		}
		chest_actor_close(&server,actors[1],WINDOWC_IRON_CHEST);
		assert(!server.chest_views && count_item_entities(&server,BLOCK_GRAVEL,4)==before+1);
		assert(server.chest_items[0][0].count==7);
		if(oom) assert(server.chest_items[1][0].id==ITEM_GOLD && server.chest_items[1][0].count==20);
	}
	puts("PASS shared chest removal: surviving half preserved; failed spill retries once, no duplicated orphan stack");
	for(int p=0;p<2;p++) {
		client_session_clear_windows(&clients[p].session);
		inventory_destroy(&actors[p]->inventory,actors[p]);
	}
	dict_entity_it_t it;
	for(dict_entity_it(it,server.entities);!dict_entity_end_p(it);dict_entity_next(it)) free(dict_entity_ref(it)->value);
	dict_entity_clear(server.entities);
	server_world_discard(&server.world);
	string_clear(level);
}

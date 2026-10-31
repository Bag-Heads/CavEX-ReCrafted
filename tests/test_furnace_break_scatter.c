/* Beta BlockFurnace.onBlockRemoval, src_original lines 142-171: three
 * nextFloat offsets per occupied slot, then nextInt(21)+10 and three
 * nextGaussian motion draws for every piece. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <m-lib/m-string.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_furnace.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

static struct server_local s;
static volatile int drain_run = 1;
static struct client_rpc drops[24];
static int drop_count;
static void* drain(void* arg) {
 (void)arg;
 while(drain_run) { clin_drain_discard(); thread_msleep(1); }
 clin_drain_discard();
 return NULL;
}
static void capture(const struct client_rpc* rpc, void* user) {
 (void)user;
 if(rpc->type == CRPC_SPAWN_ITEM) {
  assert(drop_count < 24);
  drops[drop_count++] = *rpc;
 }
}
static void verify(void) {
 struct random_gen expected = {.seed = 4242};
 int total_coal = 0, total_ore = 0, i = 0;
 const struct item_data slots[3] = {
  {.id = BLOCK_IRON_ORE, .count = 10},
  {.id = ITEM_COAL, .count = 64}, {0}
 };
 for(int slot = 0; slot < 3; slot++) {
  int remain = slots[slot].count;
  if(!remain) continue;
  float ox = rand_gen_flt(&expected)*0.8F+0.1F;
  float oy = rand_gen_flt(&expected)*0.8F+0.1F;
  float oz = rand_gen_flt(&expected)*0.8F+0.1F;
  float x = (float)8 + ox;
  float y = (float)70 + oy;
  float z = (float)8 + oz;
  while(remain) {
   int take = rand_gen_int(&expected, 21)+10;
   if(take > remain) take = remain;
   remain -= take;
   float vx = rand_gen_gaussian(&expected)*0.05F;
   float vy = rand_gen_gaussian(&expected)*0.05F+0.2F;
   float vz = rand_gen_gaussian(&expected)*0.05F;
   assert(i < drop_count);
   const struct client_rpc* rpc = &drops[i++];
   assert(rpc->payload.spawn_item.item.id == slots[slot].id);
   assert(rpc->payload.spawn_item.item.count == take);
   assert(take <= 30);
   if(rpc->payload.spawn_item.pos[0] != x || rpc->payload.spawn_item.pos[1] != y || rpc->payload.spawn_item.pos[2] != z)
    fprintf(stderr,"piece %d pos got %.9g %.9g %.9g expected %.9g %.9g %.9g\n",i-1,rpc->payload.spawn_item.pos[0],rpc->payload.spawn_item.pos[1],rpc->payload.spawn_item.pos[2],x,y,z);
   assert(rpc->payload.spawn_item.pos[0] == x);
   assert(rpc->payload.spawn_item.pos[1] == y);
   assert(rpc->payload.spawn_item.pos[2] == z);
   assert(rpc->payload.spawn_item.vel[0] == vx);
   assert(rpc->payload.spawn_item.vel[1] == vy);
   assert(rpc->payload.spawn_item.vel[2] == vz);
   if(slot == 0) total_ore += take; else total_coal += take;
  }
 }
 assert(i == drop_count && drop_count > 2);
 assert(total_ore == 10 && total_coal == 64);
}
static void run(bool open) {
 assert(server_world_set_block(&s.worlds[0], 8, 70, 8,
  (struct block_data){.type=BLOCK_FURNACE,.metadata=2}));
 assert(furnace_te_register(&s, 8, 70, 8) >= 0);
 struct furnace_te* f = furnace_te_lookup(&s, 8, 70, 8);
 f->items[0] = (struct item_data){.id=BLOCK_IRON_ORE,.count=10};
 f->items[1] = (struct item_data){.id=ITEM_COAL,.count=64};
 s.player.active_inventory = &s.player.inventory;
 if(open) assert(server_local_open_inventory(&s, &inventory_logic_furnace,
  FURNACE_SIZE, 8, 70, 8, WINDOWC_FURNACE, WINDOW_TYPE_FURNACE));
 s.container_drop_rng = (struct random_gen){.seed=4242};
 drop_count = 0;
 struct block_data block = {.type=BLOCK_FURNACE,.metadata=2};
 struct block_info info = {.block=&block,.x=8,.y=70,.z=8};
 block_furnaceoff.onBreak(&s, &info);
 assert(furnace_te_lookup(&s, 8, 70, 8) == NULL);
 verify();
 if(open) {
  struct inventory* inv = s.player.active_inventory;
  assert(inv->invalidated);
  for(int j=0;j<3;j++) assert(inv->items[j].count == 0);
  inventory_destroy_owned(inv, &s.player);
  s.player.active_inventory = &s.player.inventory;
 }
 assert(server_world_set_block(&s.worlds[0], 8, 70, 8,
  (struct block_data){.type=BLOCK_AIR}));
}
int main(void) {
 char dir[128];
 test_fixture_dir("test_furnace_break_scatter_tmp",dir,sizeof dir);
 assert(clin_init());
 blocks_init(); items_init();
 MKDIR(dir); MKDIR(test_fixture_join(dir,"/region"));
 string_t level; string_init_set_str(level,dir);
 memset(&s,0,sizeof s);
 /* IMP-TE-CAPS: growable tile slabs (all rows free) */
 if(!server_local_te_tables_init(&s))
 	abort();
 // This fixture has one explicit Overworld context.
 server_world_create(&s.worlds[0],level,WORLD_DIM_OVERWORLD,4242,true);
 s.worlds[0].session=&s; s.worlds[0].local=&s;
 dict_entity_init(s.entities);
 s.player.has_pos=true;
 assert(inventory_create(&s.player.inventory,&inventory_logic_player,
  &s,&s.player,INVENTORY_SIZE,0,0,0));
 struct thread thread;
 assert(thread_create(&thread,drain,NULL,64));
 clin_test_set_rpc_observer(capture,NULL);
 struct server_chunk* chunk=NULL;
 assert(server_world_load_chunk(&s.worlds[0],0,0,&chunk));
 run(false); run(true);
 clin_test_set_rpc_observer(NULL,NULL);
 drain_run=0; thread_join(&thread);
 dict_entity_it_t it; dict_entity_it(it,s.entities);
 while(!dict_entity_end_p(it)) { free(dict_entity_ref(it)->value); dict_entity_next(it); }
 dict_entity_clear(s.entities);
 server_world_discard(&s.worlds[0]);
 inventory_destroy(&s.player.inventory,&s.player);
 string_clear(level);
 test_fixture_dir_cleanup(dir);
 puts("PASS furnace break scatter: closed and open windows, exact seeded pieces");
 return 0;
}

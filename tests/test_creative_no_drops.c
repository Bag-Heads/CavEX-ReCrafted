/* ci-1101/08: sanctioned creative addition, maintainer 2026-11-01.
 * Beta references: RetroMCP b1.7.3 PlayerControllerSP.sendBlockRemoved;
 * BlockChest:129-156, BlockFurnace:142-171, BlockDispenser:175-202
 * spill contents in onBlockRemoval, independently of harvestBlock.
 * Explosion.java:158 retains dropBlockAsItemWithChance(..., 0.3F).
 * --survival records deterministic item fields for before/after comparison.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m-lib/m-string.h>
#include "support/test_fixture_dir.h"
#include "support/drain_quiesce.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "item/items.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_furnace.h"
#include "network/server_dispenser.h"
#include "network/server_world.h"
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

static struct server_local s;
static atomic_bool running = true;
static void* drain(void* unused) {
    (void)unused;
    while(atomic_load(&running)) { test_drain_pass(); thread_msleep(1); }
    clin_drain_discard();
    return NULL;
}
static void set_block(int x, int y, int z, int id) {
    struct block_data old;
    assert(server_world_get_block(&s.worlds[0],x,y,z,&old));
    if(old.type==id) return;
    assert(server_world_set_block(&s.worlds[0], x,y,z,
        (struct block_data){.type=id}));
}
static int type_at(int x, int y, int z) {
    struct block_data b;
    assert(server_world_get_block(&s.worlds[0],x,y,z,&b));
    return b.type;
}
static void clear_items(void) {
    dict_entity_it_t it;
    dict_entity_it(it,s.entities);
    while(!dict_entity_end_p(it)) {
        free(dict_entity_ref(it)->value);
        dict_entity_next(it);
    }
    dict_entity_reset(s.entities);
    s.next_entity_id=1;
    s.rand_src=(struct random_gen){.seed=4242};
    s.container_drop_rng=(struct random_gen){.seed=4242};
    jrand_set_seed(&s.worlds[0].rand,4242);
    server_local_test_seed_math_random(4242);
}
/* Serialize meaningful fields, never pointers/padding, in entity-ID order. */
static uint64_t snapshot_hash;
static void hash_bytes(const void* data, size_t size) {
    const unsigned char* bytes=data;
    for(size_t i=0;i<size;i++) {
        snapshot_hash^=bytes[i];
        snapshot_hash*=UINT64_C(1099511628211);
    }
}
static int snapshot(int wanted_id, int* total) {
    snapshot_hash=UINT64_C(14695981039346656037);
    int n=0;
    *total=0;
    for(uint32_t id=0; id<=s.next_entity_id; ++id) {
        struct entity** pp=dict_entity_get(s.entities,id);
        if(!pp) continue;
        struct entity* e=*pp;
        assert(e->type==ENTITY_ITEM);
        if(wanted_id) assert(e->data.item.item.id==wanted_id);
        *total+=e->data.item.item.count;
        ++n;
        hash_bytes(&e->data.item.item.id,sizeof e->data.item.item.id);
        hash_bytes(&e->data.item.item.count,sizeof e->data.item.item.count);
        hash_bytes(&e->data.item.item.durability,sizeof e->data.item.item.durability);
        hash_bytes(e->pos,sizeof e->pos);
        hash_bytes(e->vel,sizeof e->vel);
        hash_bytes(&e->data.item.pickup_delay,sizeof e->data.item.pickup_delay);
        printf("item %u %u %u %d pos %a %a %a vel %a %a %a delay %d\n",
            id,e->data.item.item.id,e->data.item.item.count,
            e->data.item.item.durability,
            (double)e->pos[0],(double)e->pos[1],(double)e->pos[2],
            (double)e->vel[0],(double)e->vel[1],(double)e->vel[2],
            e->data.item.pickup_delay);
    }
    return n;
}
static void dig(int block_id, int tool) {
    set_block(8,70,8,block_id);
    inventory_set_hotbar(&s.player.inventory,0);
    inventory_clear_slot(&s.player.inventory,INVENTORY_SLOT_HOTBAR);
    if(tool) inventory_set_slot(&s.player.inventory,INVENTORY_SLOT_HOTBAR,
        (struct item_data){.id=tool,.count=1});
    struct server_rpc r={.type=SRPC_BLOCK_DIG,
        .payload.block_dig={.x=8,.y=70,.z=8,.finished=true}};
    server_local_test_process_rpc(&s,&r);
}
static void check_break(int block_id,int tool,int drop_id) {
    clear_items();
    dig(block_id,tool);
    assert(type_at(8,70,8)==BLOCK_AIR);
    int total;
    int n=snapshot(drop_id,&total);
    assert(n==(s.creative_mode?0:1));
    assert(total==(s.creative_mode?0:1));
}
static void check_container(int id) {
    clear_items();
    set_block(8,70,8,id);
    struct item_data* contents=NULL;
    if(id==BLOCK_CHEST) {
        struct te_store st=server_world_tile_store(&s.worlds[0]);
        int row=te_store_claim(&st,TE_KIND_CHEST,8,70,8,NULL);
        assert(row>=0);
        contents=s.chest_items[row];
    } else if(id==BLOCK_FURNACE) {
        assert(furnace_te_register(&s,8,70,8)>=0);
        contents=furnace_te_lookup(&s,8,70,8)->items;
    } else {
        assert(dispenser_te_register(&s,8,70,8)>=0);
        contents=dispenser_te_lookup(&s,8,70,8)->items;
    }
    contents[0]=(struct item_data){.id=ITEM_DIAMOND,.count=64};
    dig(id,ITEM_IRON_PICKAXE);
    assert(type_at(8,70,8)==BLOCK_AIR);
    int total;
    int n=snapshot(s.creative_mode?ITEM_DIAMOND:0,&total);
    /* Existing chest path emits whole stacks; furnace/dispenser scatter.
     * This addition preserves contents, rather than changing their scatter. */
    assert(n>0 && total==(s.creative_mode?64:65));
}
static void check_explosion(float power) {
    static uint64_t survival_hash[2], survival_rng[2];
    int index=power==4.0F?0:1;
    clear_items();
    for(int x=5;x<=11;x++) for(int y=67;y<=73;y++)
        for(int z=5;z<=11;z++) set_block(x,y,z,BLOCK_DIRT);
    server_world_explode(&s.worlds[0],(vec3){8.5F,70.5F,8.5F},power);
    assert(type_at(8,70,8)==BLOCK_AIR);
    int total;
    int n=snapshot(BLOCK_DIRT,&total);
    assert(n>0 && total==n);
    if(s.creative_mode) {
        assert(snapshot_hash==survival_hash[index]);
        assert(s.worlds[0].rand.seed==survival_rng[index]);
    } else {
        survival_hash[index]=snapshot_hash;
        survival_rng[index]=s.worlds[0].rand.seed;
    }
    printf("blast %.0f drops %d hash %llx\n",(double)power,n,
        (unsigned long long)snapshot_hash);
    for(int x=5;x<=11;x++) for(int y=67;y<=73;y++)
        for(int z=5;z<=11;z++) set_block(x,y,z,BLOCK_AIR);
}
int main(int argc,char** argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    bool survival_only=argc==2 && strcmp(argv[1],"--survival")==0;
    char dir[128];
    test_fixture_dir("test_creative_no_drops_tmp",dir,sizeof dir);
    assert(clin_init()); blocks_init(); items_init();
    assert(server_local_te_tables_init(&s));
    MKDIR(dir); MKDIR(test_fixture_join(dir,"/region"));
    string_t level; string_init_set_str(level,dir);
    server_world_create(&s.worlds[0],level,WORLD_DIM_OVERWORLD,4242,true);
    s.worlds[0].session=&s; s.worlds[0].local=&s;
    dict_entity_init(s.entities);
    assert(inventory_create(&s.player.inventory,&inventory_logic_player,
        &s,&s.player,INVENTORY_SIZE,0,0,0));
    s.player.active_inventory=&s.player.inventory;
    s.player.has_pos=true; s.player.finished_loading=true;
    s.player.health=MAX_PLAYER_HEALTH;
    s.player.x=100; s.player.y=70; s.player.z=100;
    struct thread worker; assert(thread_create(&worker,drain,NULL,64));
    struct server_chunk* chunk=NULL;
    assert(server_world_load_chunk(&s.worlds[0],0,0,&chunk));
    for(int creative=0;creative<=(survival_only?0:1);creative++) {
        s.creative_mode=creative;
        printf("mode %d\n",creative);
        check_break(BLOCK_DIRT,0,BLOCK_DIRT);
        check_break(BLOCK_STONE,ITEM_IRON_PICKAXE,BLOCK_COBBLESTONE);
        check_break(BLOCK_LEAVES,ITEM_SHEARS,BLOCK_LEAVES);
        check_break(BLOCK_TNT,0,BLOCK_TNT);
        check_container(BLOCK_CHEST);
        check_container(BLOCK_FURNACE);
        check_container(BLOCK_DISPENSER);
        /* Ice's non-drop harvest side effect must remain unchanged. */
        clear_items(); set_block(8,69,8,BLOCK_STONE);
        dig(BLOCK_ICE,ITEM_IRON_PICKAXE);
        assert(type_at(8,70,8)==BLOCK_WATER_FLOW);
        set_block(8,70,8,BLOCK_AIR); set_block(8,69,8,BLOCK_AIR);
        /* Production blast path used by TNT (4) and ordinary creepers (3). */
        check_explosion(4.0F); check_explosion(3.0F);
    }
    clear_items();
    server_world_discard(&s.worlds[0]);
    inventory_destroy(&s.player.inventory,&s.player);
    dict_entity_clear(s.entities);
    test_drain_quiesce(); atomic_store(&running,false); thread_join(&worker);
    te_grow_ctx_free(&s.te_grow);
    string_clear(level); test_fixture_dir_cleanup(dir);
    puts("PASS creative no harvest drops; contents and explosions retained");
    return 0;
}

/* Run only against 3a682653, before the persistence slice. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
time_t time(time_t* out) { time_t t=1700000000; if(out) *out=t; return t; }
static void copy_file(const char* src,const char* dst) {
 FILE* a=fopen(src,"rb");FILE* b=fopen(dst,"wb");assert(a&&b);
 char buf[4096];size_t n;while((n=fread(buf,1,sizeof(buf),a)))assert(fwrite(buf,1,n,b)==n);
 assert(!ferror(a));assert(!fclose(a));assert(!fclose(b));
}
#include "network/level_archive.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "network/entity_archive.h"
void inventory_clear(struct inventory* inv) {
 memset(inv->items,0,inv->capacity*sizeof(*inv->items));
 inv->picked_item=(struct item_data){0}; inv->hotbar_slot=0;
}
int main(int argc,char** argv) {
 assert(argc==2 || argc==3); mkdir(argv[1]);
 string_t world; string_init_set_str(world,argv[1]);
 struct level_archive la; assert(level_archive_create_new(&la,world,42,(vec3){0,70,0}));
 struct item_data items[INVENTORY_SIZE]={0};
 struct inventory inv={.items=items,.capacity=INVENTORY_SIZE};
 items[INVENTORY_SLOT_HOTBAR]=(struct item_data){.id=5,.count=8,.durability=2};
 assert(level_archive_write_inventory(&la,&inv)); assert(level_archive_save(&la)); level_archive_destroy(&la);
 struct region_archive ra; assert(region_archive_create_new(&ra,world,0,0,WORLD_DIM_OVERWORLD));
 struct server_chunk sc={0}; sc.populated=true;
 sc.ids=calloc(32768,1); sc.metadata=calloc(16384,1); sc.lighting_sky=calloc(16384,1);
 sc.lighting_torch=calloc(16384,1); sc.heightmap=calloc(256,1);
 struct te_chest chest={.x=0,.y=64,.z=0}; chest.items[0]=items[INVENTORY_SLOT_HOTBAR];
 struct chunk_tile_entities te={.legacy_flat_complete=true,.chests=&chest,.n_chest=1};
 struct chunk_entities entities={0};
 assert(region_archive_set_chunk(&ra,0,0,&sc,&te,&entities));
 assert(region_archive_sync(&ra)); assert(region_archive_destroy(&ra));
 char path[512],copy[512];
 snprintf(path,sizeof(path),"%s/level.dat",argv[1]);snprintf(copy,sizeof(copy),"%s/level.before",argv[1]);copy_file(path,copy);
 snprintf(path,sizeof(path),"%s/region/r.0.0.mcr",argv[1]);snprintf(copy,sizeof(copy),"%s/region.before",argv[1]);copy_file(path,copy);
 assert(level_archive_create(&la,world));items[INVENTORY_SLOT_HOTBAR].count=7;
 assert(level_archive_write_inventory(&la,&inv));assert(level_archive_save(&la));level_archive_destroy(&la);
 assert(region_archive_create(&ra,world,0,0,WORLD_DIM_OVERWORLD));chest.items[0].count=7;
 assert(region_archive_set_chunk(&ra,0,0,&sc,&te,&entities));assert(region_archive_sync(&ra));assert(region_archive_destroy(&ra));
 /* Optional multi-region fixture: two edited regions, a third untouched,
  * with a second untouched chunk in every region. Same pre-slice writer. */
 if(argc==3) for(int r=1;r<=3;r++) {
  assert(region_archive_create_new(&ra,world,r,0,WORLD_DIM_OVERWORLD));
  for(int c=0;c<2;c++) {
   chest.x=(r*32+c)*16;chest.items[0].count=8;
   assert(region_archive_set_chunk(&ra,r*32+c,0,&sc,&te,&entities));
  }
  assert(region_archive_sync(&ra));assert(region_archive_destroy(&ra));
  snprintf(path,sizeof(path),"%s/region/r.%d.0.mcr",argv[1],r);
  snprintf(copy,sizeof(copy),"%s/r.%d.0.before",argv[1],r);copy_file(path,copy);
  if(r<3) {
   assert(region_archive_create(&ra,world,r,0,WORLD_DIM_OVERWORLD));
   chest.x=r*32*16;chest.items[0].count=7;
   assert(region_archive_set_chunk(&ra,r*32,0,&sc,&te,&entities));
   assert(region_archive_sync(&ra));assert(region_archive_destroy(&ra));
  }
 }
 free(sc.ids);free(sc.metadata);free(sc.lighting_sky);free(sc.lighting_torch);free(sc.heightmap);
 string_clear(world); return 0;
}

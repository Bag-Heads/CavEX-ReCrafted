/* Beta 1.7.3 World.func_28105_a:704-851 and Block.collisionRayTrace:357-420.
 * The callback is a synthetic cell fixture; expected hits are hand-derived
 * from the Java plane order, not from the production tracer. */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/block_ray.h"

struct fixture {
	int x, y, z;
	bool liquid, source;
	struct block_ray_cell cell;
};

static bool cell_at(int x, int y, int z, bool stop_on_liquid,
						struct block_ray_cell* out, void* ctx) {
	struct fixture* f = ctx;
	*out = (struct block_ray_cell) {0};
	if(x != f->x || y != f->y || z != f->z)
		return true;
	*out = f->cell;
	out->present = true;
	if(f->liquid)
		out->collidable = stop_on_liquid && f->source;
	return true;
}

static struct fixture cube(int x, int y, int z) {
	return (struct fixture) {.x=x,.y=y,.z=z,
		.cell={.present=true,.collidable=true,.has_collision_box=true,
		.has_ray_box=true,.x1=0,.y1=0,.z1=0,.x2=1,.y2=1,.z2=1}};
}

static void expect_hit(struct fixture* f, const double from[3],
					   const double to[3], int side, const double hit[3]) {
	struct block_ray_hit got;
	assert(block_ray_trace(from,to,false,true,cell_at,f,&got,NULL));
	assert(got.x==f->x && got.y==f->y && got.z==f->z && got.side==side);
	for(int i=0;i<3;i++) assert(fabs(got.hit[i]-hit[i])<1e-12);
}

int main(void) {
	struct block_ray_hit hit;
	struct block_ray_stats stats;
	struct fixture f=cube(0,0,0);
	/* Start cell is tested before the DDA; entering x=0 chooses side 4. */
	expect_hit(&f,(double[]){-.5,.5,.5},(double[]){.5,.5,.5},4,
		(double[]){0,.5,.5});
	/* Start inside returns the nearest exit face, not a zero-distance hit. */
	expect_hit(&f,(double[]){.5,.5,.5},(double[]){1.5,.5,.5},5,
		(double[]){1,.5,.5});
	/* Block.collisionRayTrace is inclusive at the end face; World DDA
	 * enters that end cell with a zero-length remainder, so World misses it.
	 * [Block.java:357-420; World.java:813-840]. */
	assert(block_ray_collision_trace(&f.cell,(double[]){-1,.5,.5},
		(double[]){0,.5,.5},&hit));
	assert(hit.side==4 && hit.hit[0]==0);
	assert(!block_ray_trace((double[]){-1,.5,.5},(double[]){0,.5,.5},
		false,true,cell_at,&f,&hit,NULL));
	/* Block.collisionRayTrace keeps the earlier X face on an exact X/Y tie. */
	expect_hit(&f,(double[]){-1,-1,.5},(double[]){1,1,.5},4,
		(double[]){0,0,.5});
	/* Vec3D.getIntermediateWithXValue rejects dx^2 < (double)1e-7F. */
	assert(!block_ray_collision_trace(&f.cell,
		(double[]){-1e-4,.5,.5},(double[]){1e-4,.5,.5},&hit));
	/* Negative travel at an exact grid boundary assigns the far-side cell. */
	f=cube(-1,0,0);
	expect_hit(&f,(double[]){.5,.5,.5},(double[]){-1.5,.5,.5},5,
		(double[]){0,.5,.5});
	/* World.java:724-727 sets 200 then tests while(var11-- >= 0): 201 steps. */
	f=cube(201,0,0);
	assert(block_ray_trace((double[]){.5,.5,.5},(double[]){250.5,.5,.5},
		false,true,cell_at,&f,&hit,&stats));
	assert(hit.x==201 && stats.cells==202 && stats.box_tests==1);
	f.x=202;
	assert(!block_ray_trace((double[]){.5,.5,.5},(double[]){250.5,.5,.5},
		false,true,cell_at,&f,&hit,NULL));
	/* Equal X/Y DDA planes choose Y, but flooring the shared corner places
	 * both positive coordinates in cell (1,1); Block chooses minX first. */
	f=cube(1,1,0);
	expect_hit(&f,(double[]){.5,.5,.5},(double[]){1.5,1.5,.5},4,
		(double[]){1,1,.5});
	/* Equal negative X/Y planes also choose Y. The negative-face correction
	 * visits (1,0), and Block's minX beats maxY at the shared corner. */
	f=cube(1,0,0);
	expect_hit(&f,(double[]){1.5,1.5,.5},(double[]){-.5,-.5,.5},4,
		(double[]){1,1,.5});
	/* Fluid source metadata0 collides only with stopOnLiquid, and a null
	 * collision getter is still skipped independently (BlockFluid:56,70). */
	f=cube(1,0,0); f.liquid=true; f.source=true;
	f.cell.has_collision_box=false; /* BlockFluid.getCollisionBoundingBox is null */
	for(int stop=0;stop<2;stop++) for(int skip=0;skip<2;skip++) {
		bool got=block_ray_trace((double[]){.5,.5,.5},(double[]){1.5,.5,.5},
			stop,skip,cell_at,&f,&hit,NULL);
		assert(got==(stop && !skip));
	}
	f.source=false;
	assert(!block_ray_trace((double[]){.5,.5,.5},(double[]){1.5,.5,.5},
		true,false,cell_at,&f,&hit,NULL));
	/* A torch-like null collision getter is skipped only when asked. */
	f=cube(1,0,0); f.cell.has_collision_box=false;
	assert(!block_ray_trace((double[]){.5,.5,.5},(double[]){1.5,.5,.5},
		false,true,cell_at,&f,&hit,NULL));
	assert(block_ray_trace((double[]){.5,.5,.5},(double[]){1.5,.5,.5},
		false,false,cell_at,&f,&hit,NULL));
	/* NaN is rejected by World before any callback. */
	assert(!block_ray_trace((double[]){NAN,0,0},(double[]){1,0,0},
		false,true,cell_at,&f,&hit,NULL));
	printf("PASS test_block_ray\n");
	return 0;
}

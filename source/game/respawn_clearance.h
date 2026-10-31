#ifndef RESPAWN_CLEARANCE_H
#define RESPAWN_CLEARANCE_H

#include <limits.h>
#include <math.h>

#include "../chunk_defs.h"
#include "../block/blocks.h"
#include "../entity/combat.h"
#include "../network/piston_moving_transport.h"

typedef bool (*respawn_clearance_get_block_fn)(void*,w_coord_t,w_coord_t,
	w_coord_t,struct block_data*);
typedef bool (*respawn_clearance_get_moving_fn)(void*,w_coord_t,w_coord_t,
	w_coord_t,struct piston_moving_state*);

static inline bool respawn_clearance_body(double eye_x,double eye_y,double eye_z,
	struct AABB* body) {
	if(!body||!isfinite(eye_x)||!isfinite(eye_y)||!isfinite(eye_z))return false;
	double raw=eye_y-(double)PLAYER_EYE_HEIGHT;
	double nearest=round(raw);
	float feet=(float)raw;
	if(nearest>=0.0&&nearest<=(double)WORLD_HEIGHT
	   &&eye_y==(double)((float)nearest+PLAYER_EYE_HEIGHT))feet=(float)nearest;
	if(!isfinite(feet)||feet<0.0F
	   ||feet+PLAYER_BODY_HEIGHT>(float)WORLD_HEIGHT)return false;
	body->x1=(float)eye_x-.3F;body->x2=(float)eye_x+.3F;
	body->y1=feet;body->y2=feet+PLAYER_BODY_HEIGHT;
	body->z1=(float)eye_z-.3F;body->z2=(float)eye_z+.3F;
	/* Validate the derived FLOAT box edges and the walker's extra-cell
	 * margins before any float-to-w_coord_t conversion. Near INT32_MAX the
	 * float representation rounds an otherwise in-range double to 2^31. */
	double min_x=floor((double)body->x1);
	double max_x=ceil((double)body->x2)+1.0;
	double min_z=floor((double)body->z1);
	double max_z=ceil((double)body->z2)+1.0;
	if(min_x<(double)INT32_MIN||max_x>(double)INT32_MAX
	   ||min_z<(double)INT32_MIN||max_z>(double)INT32_MAX)return false;
	return true;
}

static inline bool respawn_clearance_blocks_clear(void* ctx,
	respawn_clearance_get_block_fn get_block,
	respawn_clearance_get_moving_fn get_moving,const struct AABB* body) {
	if(!get_block||!body)return false;
	w_coord_t min_x=(w_coord_t)floorf(body->x1);
	w_coord_t min_y=(w_coord_t)floorf(body->y1)-1;
	w_coord_t min_z=(w_coord_t)floorf(body->z1);
	w_coord_t max_x=(w_coord_t)ceilf(body->x2)+1;
	w_coord_t max_y=(w_coord_t)ceilf(body->y2)+1;
	w_coord_t max_z=(w_coord_t)ceilf(body->z2)+1;
	if(min_y<0)min_y=0;
	if(max_y>WORLD_HEIGHT)max_y=WORLD_HEIGHT;
	for(w_coord_t x=min_x;x<max_x;x++)for(w_coord_t z=min_z;z<max_z;z++)
		for(w_coord_t y=min_y;y<max_y;y++) {
			struct block_data blk;
			if(!get_block(ctx,x,y,z,&blk))return false;
			if(!blocks[blk.type])continue;
			struct block_info info={.block=&blk,.x=x,.y=y,.z=z,
				.get_piston_moving=get_moving,
				.get_piston_moving_ctx=ctx};
			size_t count=blocks[blk.type]->getBoundingBox(&info,true,NULL);
			if(!count)continue;
			struct AABB boxes[count];
			blocks[blk.type]->getBoundingBox(&info,true,boxes);
			for(size_t k=0;k<count;k++) {
				aabb_translate(&boxes[k],x,y,z);
				struct AABB candidate=*body;
				if(aabb_intersection_strict(&candidate,&boxes[k]))return false;
			}
		}
	return true;
}

#endif

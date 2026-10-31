/* [Beta 1.7.3 Render.renderEntityOnFire, Render.java:30-88]
 * Expected values below are evaluated from Java's float expressions by hand;
 * they are deliberately independent of the C builder. */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "graphics/render_entity_fire.h"

static void near(float got, float want) {
	if(fabsf(got - want) >= 0.000002F)
		fprintf(stderr, "near: got %.9g want %.9g\n", got, want);
	assert(fabsf(got - want) < 0.000002F);
}

static bool facing_case(int degrees) {
	struct entity_fire_quad q[32];
	float yaw = (float)degrees * 3.14159265358979323846F / 180.0F;
	float fx = sinf(yaw), fz = cosf(yaw);
	/* camera.c's level forward is (sin(rx),0,cos(rx)); the view's
	 * screen-right axis is (-cos(rx),0,sin(rx)). With glFrontFace(GL_CW)
	 * and MODE_BACK, a visible quad has negative screen signed area.
	 * Its CW front normal is (v2-v0) x (v1-v0), toward -forward. */
	float right_x = -fz, right_z = fx;
	size_t n = render_entity_fire_build(true, 0.6F, 1.8F, 0.0F,
		yaw, q, 32);
	assert(n == 5);
	bool normals = true, winding = true;
	for(size_t i = 0; i < n; i++) {
		const struct entity_fire_vertex *a = &q[i].v[0],
			*b = &q[i].v[1], *c = &q[i].v[2];
		float x1 = b->x - a->x, y1 = b->y - a->y,
			z1 = b->z - a->z;
		float x2 = c->x - a->x, y2 = c->y - a->y,
			z2 = c->z - a->z;
		float nx = y2 * z1 - z2 * y1;
		float nz = x2 * y1 - y2 * x1;
		normals &= nx * fx + nz * fz < -0.01F;
		float sx1 = x1 * right_x + z1 * right_z;
		float sx2 = x2 * right_x + z2 * right_z;
		winding &= sx1 * y2 - y1 * sx2 < -0.01F;
	}
	/* The first layer's centre has zero horizontal displacement in Beta's
	 * local quad. Its depth offset must be on the camera side: -forward. */
	float cx = (q[0].v[0].x + q[0].v[2].x) * 0.5F;
	float cz = (q[0].v[0].z + q[0].v[2].z) * 0.5F;
	bool depth = cx * fx + cz * fz < -0.01F;
	printf("facing %d: normal=%s depth=%s CW=%s\n", degrees,
		normals ? "PASS" : "FAIL", depth ? "PASS" : "FAIL",
		winding ? "PASS" : "FAIL");
	return normals && depth && winding;
}

static void size_case(float width, float height, size_t expected,
					  float scale, float initial_z,
					  const float* bottoms, const float* depths) {
	struct entity_fire_quad q[32];
	size_t n = render_entity_fire_build(true, width, height, 0.0F,
										  0.0F, q, 32);
	assert(n == expected);
	if(!n) return;
	for(size_t i = 0; i < n; i++) {
		near(q[i].v[0].y, bottoms[i]);
		near(q[i].v[3].y, bottoms[i]);
		near(q[i].v[0].z, depths[i]);
		near(q[i].v[2].y, bottoms[i] + 1.4F * scale);
	}
	near(q[0].v[0].x, 0.5F * scale);
	near(q[0].v[0].y, 0.0F);
	near(q[0].v[0].z, initial_z * scale);
	near(q[0].v[2].y, 1.4F * scale);
	/* Block.fire.blockIndexInTexture=31: x=240, y=16. */
	near(q[0].v[0].u, 240.0F / 256.0F);
	near(q[0].v[3].u, 255.99F / 256.0F);
	near(q[0].v[1].u, q[0].v[0].u);
	near(q[0].v[1].v, 16.0F / 256.0F);
	near(q[0].v[0].v, 31.99F / 256.0F);
	near(q[0].v[2].v, 16.0F / 256.0F);
	if(n > 1) {
		near(q[1].v[0].z, (initial_z + 0.03F) * scale);
		near(q[1].v[0].y, 0.45F * scale);
		near(q[1].v[0].x, 0.45F * scale);
		near(q[1].v[0].v, 47.99F / 256.0F);
		near(q[1].v[2].v, 32.0F / 256.0F);
	}
	if(n > 2) {
		near(q[2].v[0].u, 255.99F / 256.0F);
		near(q[2].v[3].u, 240.0F / 256.0F);
		near(q[2].v[0].y, 0.90F * scale);
		near(q[2].v[0].z, (initial_z + 0.06F) * scale);
	}
	if(n == 5) {
		/* The fifth iteration begins after four 0.45F subtractions;
		 * its bottom is 1.8*scale, depth is 4*0.03F. */
		near(q[4].v[0].y, 1.8F * scale);
		near(q[4].v[0].z, (initial_z + 0.12F) * scale);
		near(q[4].v[0].v, 31.99F / 256.0F);
	}
}

int main(void) {
	static const int yaws[] = {0, 45, 90, 180, 270};
	bool facing = true;
	for(size_t i = 0; i < sizeof(yaws) / sizeof(yaws[0]); i++)
		facing &= facing_case(yaws[i]);
	fflush(stdout);
	assert(facing);
	/* scale=width*1.4F; while(height/scale>0) subtracts 0.45F.
	 * Initial Z is -0.3F+(int)(height/scale)*0.02F; each layer adds
	 * 0.03F before multiplication by scale. Bottoms add 0.45F*scale.
	 * These expected literals are evaluated independently of the builder. */
	static const float mob_y[] = {0.0F, 0.378F, 0.756F, 1.134F, 1.512F};
	static const float mob_z[] = {-0.2184F, -0.1932F, -0.168F,
		-0.1428F, -0.1176F};
	static const float spider_y[] = {0.0F, 0.882F};
	static const float spider_z[] = {-0.588F, -0.5292F};
	static const float ghast_y[] = {0.0F, 2.52F};
	static const float ghast_z[] = {-1.68F, -1.512F};
	static const float item_y[] = {0.0F, 0.1575F};
	static const float item_z[] = {-0.105F, -0.0945F};
	size_case(0.6F, 1.8F, 5, 0.84F, -0.26F, mob_y, mob_z);
	size_case(1.4F, 0.9F, 2, 1.96F, -0.30F, spider_y, spider_z);
	size_case(4.0F, 4.0F, 2, 5.6F, -0.30F, ghast_y, ghast_z);
	size_case(0.25F, 0.25F, 2, 0.35F, -0.30F, item_y, item_z);
	size_case(0.6F, 0.0F, 0, 0.84F, -0.30F, NULL, NULL);
	struct entity_fire_quad q[32];
	assert(render_entity_fire_build(false, 0.6F, 1.8F, 0.0F,
										0.0F, q, 32) == 0);
	/* posY-minY=0.125: the first bottom drops by 0.125*scale. */
	assert(render_entity_fire_build(true, 0.6F, 1.8F, 0.125F,
										0.0F, q, 32) == 5);
	near(q[0].v[0].y, -0.125F * 0.84F);
	/* Beta's -playerViewY is +camera.rx: the first depth at rx=90
	 * moves toward -X, while local +X rotates toward -Z. */
	assert(render_entity_fire_build(true, 0.6F, 1.8F, 0.0F,
										1.5707963267948966F, q, 32) == 5);
	near(q[0].v[0].x, -0.26F * 0.84F);
	near(q[0].v[0].z, -0.5F * 0.84F);
	puts("test_entity_fire_billboard_golden: PASS");
	return 0;
}

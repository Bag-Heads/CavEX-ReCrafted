/* Independent double-precision OpenGL postmultiply transcription:
 * RetroMCP b1.7.3 src_original ItemRenderer.java:154-220.
 * Weak declaration permits a runnable RED before the new map path exists. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "graphics/render_item.h"

extern void render_item_map_transforms(float pitch, float swing, float equip,
	mat4 surface, mat4 hands[2]) __attribute__((weak));
typedef double matrix[4][4];
static void identity(matrix a) {
	memset(a, 0, sizeof(matrix));
	for(int i = 0; i < 4; i++) a[i][i] = 1;
}
static void mul(matrix a, matrix b) {
	matrix out = {{0}};
	for(int c = 0; c < 4; c++) for(int r = 0; r < 4; r++)
		for(int k = 0; k < 4; k++) out[c][r] += a[k][r]*b[c][k];
	memcpy(a, out, sizeof(out));
}
static void translate(matrix a, double x, double y, double z) {
	matrix b; identity(b); b[3][0]=x; b[3][1]=y; b[3][2]=z; mul(a,b);
}
static void scale(matrix a, double x, double y, double z) {
	matrix b; identity(b); b[0][0]=x; b[1][1]=y; b[2][2]=z; mul(a,b);
}
static void rotate(matrix a, double deg, int axis) {
	double angle=deg*3.14159265358979323846/180, c=cos(angle), s=sin(angle);
	int u=(axis+1)%3, v=(axis+2)%3;
	matrix b; identity(b); b[u][u]=b[v][v]=c; b[u][v]=s; b[v][u]=-s; mul(a,b);
}
static void equal(mat4 actual, matrix expected) {
	for(int c=0;c<4;c++) for(int r=0;r<4;r++) {
		if(fabs(actual[c][r]-expected[c][r]) > 0.00002) {
			fprintf(stderr,"matrix[%d][%d] actual %.9g Beta %.9g\n",c,r,actual[c][r],expected[c][r]);
			assert(0);
		}
	}
}
static void check(float pitch, float swing, float equip) {
	const double pi=3.14159265358979323846;
	double root=sin(sqrt(swing)*pi), tilt=fmax(0,fmin(1,1-pitch/45.0+0.1));
	tilt=-cos(tilt*pi)*0.5+0.5;
	matrix base, expected; identity(base);
	translate(base,-root*0.4,sin(sqrt(swing)*pi*2)*0.2,-sin(swing*pi)*0.2);
	translate(base,0,-(1-equip)*1.2-tilt*0.5+0.04,-0.9*0.8);
	rotate(base,90,1); rotate(base,-85*tilt,2);
	mat4 surface,hands[2];
	render_item_map_transforms(pitch,swing,equip,surface,hands);
	for(int i=0;i<2;i++) {
		int sign=2*i-1;
		memcpy(expected,base,sizeof(base));
		translate(expected,0,-0.6,1.1*sign);
		rotate(expected,-45*sign,0); rotate(expected,-90,2);
		rotate(expected,59,2); rotate(expected,-65*sign,1);
		/* RenderPlayer.drawFirstPersonHand -> ModelBiped right arm, age=0:
		 * pivot(-5,2,0), roll=.1 rad; engine box axes (X,-Y,-Z). */
		scale(expected,1.0/16,1.0/16,1.0/16);
		translate(expected,-5,2,0); rotate(expected,0.1*180/pi,2);
		translate(expected,-3,10,2); rotate(expected,180,0);
		equal(hands[i],expected);
		/* render_model_box {4,4,12} emits extents (4,12,4).
		 * Verify every corner, including the long axis of the forearm. */
		for(unsigned mask=0;mask<8;mask++) {
			vec4 corner={(mask&1)?4:0,(mask&2)?12:0,(mask&4)?4:0,1}, got;
			glm_mat4_mulv(hands[i],corner,got);
			for(int r=0;r<3;r++) {
				double want=0;
				for(int c=0;c<4;c++) want+=expected[c][r]*corner[c];
				assert(fabs(got[r]-want)<0.00002);
			}
		}
	}
	memcpy(expected,base,sizeof(base));
	rotate(expected,-sin(swing*swing*pi)*20,1);
	rotate(expected,-root*20,2); rotate(expected,-root*80,0);
	scale(expected,0.38,0.38,0.38); rotate(expected,90,1); rotate(expected,180,2);
	translate(expected,-1,-1,0); scale(expected,1.0/64,1.0/64,1.0/64);
	/* Existing display-list vertices use twice the map pixels, Y inverted,
	 * and backend /256. Convert that representation to Beta map pixels. */
	translate(expected,0,128,0); scale(expected,128,-128,128);
	equal(surface,expected);
}
int main(void) {
	assert(render_item_map_transforms && "missing Beta held-map replacement path");
	const float pitches[]={-90,0,4.5F,27,49.5F,90};
	const float swings[]={0,0.125F,0.5F,0.875F};
	const float equips[]={0,0.3F,1};
	for(unsigned p=0;p<6;p++) for(unsigned s=0;s<4;s++) for(unsigned e=0;e<3;e++)
		check(pitches[p],swings[s],equips[e]);
	puts("PASS 72 Beta held-map transforms and 144 hand transforms");
}

/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 * Uses the real Wii adapter and its existing driver fixture. Compile/link with
 * test_input_wii_adapter's sources/definitions, replacing its test source with
 * this file. No new production seam or build profile is required.
 */
#define main input_wii_adapter_suite_main
#include "test_input_wii_adapter.c"
#undef main

static float script_x;
static bool scripted_cursor(float* x, float* y, float* angle, bool* available) {
	*x = script_x; *y = 240; *angle = 0;
	*available = true;
	return true;
}

static void ir_sample(float x, float y, float angle, bool valid, bool owned) {
	WPADData data = report(WPAD_EXP_NUNCHUK, 0);
	data.data_present |= WPAD_DATA_IR;
	data.ir.valid = valid;
	data.ir.x = x; data.ir.y = y; data.ir.angle = angle;
	if(owned) {
		enqueue(0, data);
		tick(16);
	} else {
		wpad[0].retained = data;
	}
}

static void cursor(float ex, float ey, float ea) {
	/* The pre-existing owned path normalizes then remaps in float. Retain
	 * that rounding in the golden; the filter must not alter larger moves. */
	const struct input_player_frame* frame = input_player_frame(0);
	if(frame && frame->pointer_normalized) {
		ex = (ex / 640.0F) * 640.0F;
		ey = (ey / 480.0F) * 480.0F;
	}
	float x, y, angle;
	assert(input_pointer(&x, &y, &angle));
	if(x != ex || y != ey || angle != ea)
		fprintf(stderr,"cursor (%g,%g,%g), expected (%g,%g,%g)\n",x,y,angle,ex,ey,ea);
	assert(x == ex && y == ey && angle == ea);
}

static void trace(bool owned) {
	reset();
	if(owned) claim_wpad(0, WPAD_EXP_NUNCHUK);
	input_pointer_enable(true);
	ir_sample(320, 240, 0, true, owned);
	cursor(320, 240, 0);
	/* Stationary hand: repeated sub-pixel drift never accumulates at output. */
	const float still[][2] = {{320.25F,240.25F}, {319.5F,240},
		{320,239.5F}, {320.5F,240.25F}, {319.75F,239.75F}};
	for(unsigned n = 0; n < 100; n++) {
		unsigned k = n % (sizeof(still) / sizeof(still[0]));
		ir_sample(still[k][0], still[k][1], 1, true, owned);
		input_pointer_enable(true); /* Some menu paths refresh this every frame. */
		cursor(320, 240, 1); /* Angle remains raw. */
		cursor(320, 240, 1); /* Repeated reads do not advance a filter. */
	}
	puts(owned ? "PASS owned still-hand trace: 100 samples -> (320,240)"
		: "PASS fallback still-hand trace: 100 samples -> (320,240)");
	/* Boundary and slow intentional motion compare against the held anchor,
	 * not the previous raw sample, so small steps eventually move the cursor. */
	ir_sample(320.75F, 240, 2, true, owned); cursor(320,240,2);
	ir_sample(321, 240, 3, true, owned); cursor(321,240,3);
	ir_sample(321.5F, 240.5F, 4, true, owned); cursor(321,240,4);
	ir_sample(321.75F, 240.75F, 5, true, owned); cursor(321.75F,240.75F,5);
	const float fast[][2] = {{600,400}, {10,10}, {320,240}};
	for(unsigned k = 0; k < sizeof(fast) / sizeof(fast[0]); k++) {
		ir_sample(fast[k][0], fast[k][1], 6, true, owned);
		cursor(fast[k][0],fast[k][1],6);
	}
	puts(owned ? "PASS owned fast trace: every output equals current raw sample"
		: "PASS fallback fast trace: every output equals current raw sample");
	/* Lost IR and menu reopen must seed immediately even within the deadzone. */
	ir_sample(0,0,0,false,owned);
	float x,y,a;
	assert(!input_pointer(&x,&y,&a));
	ir_sample(320.25F,240.25F,7,true,owned); cursor(320.25F,240.25F,7);
	input_pointer_enable(false); input_pointer_enable(true);
	ir_sample(320.5F,240.5F,8,true,owned); cursor(320.5F,240.5F,8);
	/* Script output bypasses the physical filter and clears its old anchor. */
	input_pointer_set_script(test_pointer_script);
	script_owns_pointer = true;
	assert(!input_pointer(&x,&y,&a));
	script_owns_pointer = false;
	input_pointer_set_script(NULL);
	ir_sample(320.75F,240.75F,9,true,owned); cursor(320.75F,240.75F,9);
	input_pointer_set_script(scripted_cursor);
	script_x = 320; assert(input_pointer(&x,&y,&a) && x == 320);
	script_x = 320.25F; assert(input_pointer(&x,&y,&a) && x == 320.25F);
	input_pointer_set_script(NULL);
	const float invalid[][3] = {{NAN,240,0},{320,INFINITY,0},{320,240,NAN}};
	for(unsigned k = 0; k < sizeof(invalid) / sizeof(invalid[0]); k++) {
		ir_sample(invalid[k][0],invalid[k][1],invalid[k][2],true,owned);
		assert(!input_pointer(&x,&y,&a));
		ir_sample(320.75F,240.75F,9,true,owned); cursor(320.75F,240.75F,9);
	}
	if(owned) {
		/* Filtering the menu never writes back into the raw owner frame. */
		ir_sample(321,241,10,true,true); cursor(320.75F,240.75F,10);
		const struct input_player_frame* f = input_player_frame(0);
		assert(f->pointer_x == 321.0F / 640 && f->pointer_y == 241.0F / 480);
		/* Transfer directly to a different channel without a missing query. */
		input_player_unbind(0);
		connect_wpad(1, WPAD_EXP_NUNCHUK);
		WPADData data = report(WPAD_EXP_NUNCHUK, 0);
		data.data_present |= WPAD_DATA_IR;
		data.ir.valid = true;
		data.ir.x = 321; data.ir.y = 241; data.ir.angle = 11;
		enqueue(1, data); tick(16);
		assert(input_player_bind(0, (struct input_device){INPUT_DEVICE_WPAD,1}));
		tick(16);
		cursor(321,241,11);
		puts("PASS raw owner coordinates preserved; channel transfer reseeds");
	}
	puts("PASS invalid/nonfinite/reopen/script reset; scripts bypass the deadzone");
}

int main(void) {
	assert(config_create(&gstate.config_user,"config_wii.json"));
	trace(false);
	trace(true);
	config_destroy(&gstate.config_user);
	return 0;
}

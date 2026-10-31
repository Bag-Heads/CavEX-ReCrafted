/* Frozen pre-owner renderer. Do not update to follow production changes. */
void hud_survival_row_reference(int hud_left, int heart_row_y) {
	gfx_bind_texture(&texture_icons);

	// Batch J heart blink + low-health jitter, state from hud_hurt.h. The
	// jitter rand is reseeded EVERY FRAME from the tick counter
	// [740c583 GuiIngame:66], so within one tick the offsets hold still;
	// jrand is the bit-exact java.util.Random (one nextInt(2) per heart, in
	// heart order -- Beta draws nothing else from it in this loop).
	bool hurt_blink = hud_hurt_blink(&gstate.players[0].camera.hurt);
	struct java_random hud_rand;
	jrand_set_seed(&hud_rand, hud_hurt_jitter_seed(&gstate.players[0].camera.hurt));
	// Beta jitter gate: health <= 4 half-hearts == 2 hearts
	// [740c583 GuiIngame:96-98], in the engine's 16-per-heart scale.
	bool heart_jitter
		= gstate.players[0].local_player->health <= 2 * HEALTH_PER_HEART;

	for(int k = 0; k < MAX_PLAYER_HEALTH / HEALTH_PER_HEART; k++) {
		int hx = hud_left + k * 8 * GFX_GUI_SCALE;
		int hy = heart_row_y;
		if(heart_jitter)
			hy += jrand_next_int_bound(&hud_rand, 2) * GFX_GUI_SCALE;
		// container first (white variant on blink frames, u = 16 + 9
		// [740c583 GuiIngame:99-101]), then overlays in Beta order:
		// prev-health ghost on blink frames [:102-109], current on top.
		gutil_texquad(hx, hy, hurt_blink ? 25 : 16, 0, 9, 9,
					  9 * GFX_GUI_SCALE, 9 * GFX_GUI_SCALE);
		if(hurt_blink) {
			int prev_hp
				= gstate.players[0].camera.hurt.prev_health - k * HEALTH_PER_HEART;
			if(prev_hp >= HEALTH_PER_HEART)
				gutil_texquad(hx, hy, 70, 0, 9, 9, 9 * GFX_GUI_SCALE,
							  9 * GFX_GUI_SCALE);
			else if(prev_hp >= HEALTH_PER_HEART / 2)
				gutil_texquad(hx, hy, 79, 0, 9, 9, 9 * GFX_GUI_SCALE,
							  9 * GFX_GUI_SCALE);
		}
		int heart_hp = gstate.players[0].local_player->health - k * HEALTH_PER_HEART;
		if(heart_hp >= HEALTH_PER_HEART)
			gutil_texquad(hx, hy, 52, 0, 9, 9, 9 * GFX_GUI_SCALE,
						  9 * GFX_GUI_SCALE);
		else if(heart_hp >= HEALTH_PER_HEART / 2)
			gutil_texquad(hx, hy, 61, 0, 9, 9, 9 * GFX_GUI_SCALE,
						  9 * GFX_GUI_SCALE);
	}

	// Batch J armor row [740c583 GuiIngame:75-93]: right-aligned at the
	// hearts' UNJITTERED y (Beta adds the jitter after the armor draw), only
	// while any armor value is present; icon per slot k compares k*2+1
	// against getTotalArmorValue's Beta-native units (armor.h port). Beta x =
	// w/2 + 91 - k*8 - 9, mirrored here off the hotbar's right edge.
	{
		int armor = armor_get_total_value(
			windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]));
		if(armor > 0) {
			for(int k = 0; k < 10; k++) {
				int ax = hud_left + 182 * GFX_GUI_SCALE
					- (9 + k * 8) * GFX_GUI_SCALE;
				int au = (k * 2 + 1 < armor) ? 34
					: ((k * 2 + 1 == armor) ? 25 : 16);
				gutil_texquad(ax, heart_row_y, au, 9, 9, 9,
							  9 * GFX_GUI_SCALE, 9 * GFX_GUI_SCALE);
			}
		}
	}

	// draw oxygen bubbles while submerged [740c583 GuiIngame:120-130]: the
	// air row sits 9px above the health row (Beta's var7 - 32 - 9), full
	// bubbles (16,18) for ceil((air-2)*10/300), the remainder up to
	// ceil(air*10/300) as the popping sprite (25,18).
	if(gstate.players[0].in_water && gstate.players[0].oxygen >= OXYGEN_THRESHOLD) {
		// Beta bubble counts -> a full 10-bubble bar for air 273..300
		// depleting to 0 (oxygen runs 300..-20 here); the integer form
		// (x*10 + 299)/300 is ceil for positive x, guarded to 0 otherwise.
		// The prior (oxygen-OXYGEN_THRESHOLD)/32 collapsed the top 10-bubble
		// band to a single tick once MAX_OXYGEN became 300 (320 == 10*32).
		int full
			= gstate.players[0].oxygen > 2 ? ((gstate.players[0].oxygen - 2) * 10 + 299) / 300 : 0;
		int total
			= gstate.players[0].oxygen > 0 ? (gstate.players[0].oxygen * 10 + 299) / 300 : 0;
		for(int k = 0; k < total; k++) {
			gutil_texquad(hud_left + k * 8 * GFX_GUI_SCALE,
						  heart_row_y - 9 * GFX_GUI_SCALE, (k < full) ? 16 : 25,
						  18, 9, 9, 9 * GFX_GUI_SCALE, 9 * GFX_GUI_SCALE);
		}
	}
}

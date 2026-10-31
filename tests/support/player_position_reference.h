/* Frozen pre-ownership client position algorithm, GPL-3.0-or-later.
 * Extracted from the saved 2026-09-05 client_interface.c, SHA256:
 * 701d668987c8d32e15d1bc1cd4cddf5f47f34e09941be2e8d6179286419629f9
 * Only owner storage/names are parameterized. Never update to match production. */
static void reference_position_only(struct entity* player,
										struct camera* camera,
										const vec3 position, uint8_t flags) {
	assert(player && camera && position);
	vec3 delta = {
		position[0] - player->pos[0],
		position[1] - player->pos[1],
		position[2] - player->pos[2],
	};
	for(int axis = 0; axis < 3; axis++) {
		player->pos[axis] += delta[axis];
		player->pos_old[axis] += delta[axis];
		player->network_pos[axis] += delta[axis];
	}
	camera->x = position[0];
	camera->y = position[1];
	camera->z = position[2];
	if(flags & PLAYER_CORRECTION_ZERO_VEL_X)
		player->vel[0] = 0.0F;
	if(flags & PLAYER_CORRECTION_ZERO_VEL_Y)
		player->vel[1] = 0.0F;
	if(flags & PLAYER_CORRECTION_ZERO_VEL_Z)
		player->vel[2] = 0.0F;
	if(flags & PLAYER_CORRECTION_SET_ON_GROUND)
		player->on_ground = (flags & PLAYER_CORRECTION_ON_GROUND) != 0;
}

static void reference_player_position(struct client_session* session,
	const struct client_rpc* call) {
	session->player_sync.correction_seq = call->payload.player_pos.correction_seq;
	session->player_sync.authoritative_pos_seen = true;
	if(call->payload.player_pos.correction_flags & PLAYER_CORRECTION_POSITION_ONLY) {
		reference_position_only(session->local_player, &session->camera,
			call->payload.player_pos.position, call->payload.player_pos.correction_flags);
				if(call->payload.player_pos.correction_flags
				   & PLAYER_CORRECTION_SET_ROTATION) {
					session->camera.rx = glm_rad(
						-call->payload.player_pos.rotation[0]);
					session->camera.ry = glm_rad(glm_clamp(
						call->payload.player_pos.rotation[1] + 90.0F,
						0.0F, 180.0F));
					session->local_player->orient[0] = session->camera.rx;
					session->local_player->orient[1] = session->camera.ry;
					glm_vec2_copy(session->local_player->orient,
						session->local_player->orient_old);
				}
	} else {
			session->camera.x = call->payload.player_pos.position[0];
			session->camera.y = call->payload.player_pos.position[1];
			session->camera.z = call->payload.player_pos.position[2];
			session->camera.rx = glm_rad(-call->payload.player_pos.rotation[0]);
			session->camera.ry = glm_rad(glm_clamp(
				call->payload.player_pos.rotation[1] + 90.0F, 0.0F, 180.0F));
			/* Main can run up to three catch-up physics ticks before camera_attach.
			 * Install both entity orientation samples now so the first fresh stick
			 * acceleration cannot use the source/default yaw. */
			session->local_player->orient[0] = session->camera.rx;
			session->local_player->orient[1] = session->camera.ry;
			glm_vec2_copy(
				session->local_player->orient, session->local_player->orient_old);
			if(session->local_player)
				session->local_player->teleport(
					session->local_player,
					(vec3) {call->payload.player_pos.position[0],
							call->payload.player_pos.position[1],
							call->payload.player_pos.position[2]});
	}
}

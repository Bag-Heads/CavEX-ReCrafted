/*
	Copyright (c) 2026 CaveWii_v2 contributors
	This file is part of CaveWii_v2.

	menus_dwell hardware profile: the CONTAINER DEMO layout shared by the server
	(server_local.c server_local_menu_dwell_container_demo places the blocks)
	and the pure walker script (menu_autopilot_script.c aims at them), so the
	console can photograph the real furnace / chest / double chest / dispenser
	screens with no human at the TV (2026-09-11).

	Frame of reference: the player stands at the fresh superflat spawn; the
	server's player.y is the EYE (server_local.c: hy + 0.1 + PLAYER_EYE_HEIGHT),
	so with px/py/pz = (int)player.x/y/z the eye is ~(px+0.5, py+0.62, pz+0.5).
	Every target sits ON the ground at y = py-1 (cell py-1..py), MENU_DWELL_DEMO_DZ
	blocks ahead (+Z) and dx blocks aside (+X), nothing in between.

	Aim = the camera's own convention (camera.c: yaw 0 faces +Z, positive yaw
	turns toward +X; pitch 90 is level, larger looks down), whole degrees:
	  yaw   = atan2(dx, dz)
	  pitch = 90 + atan2(eye_y - centre_y, horizontal distance),  eye_y - centre_y = 1.12
	Ray checks (eye (0.5,5.62,0.5) -> block cells) were done by hand for every
	entry below; each ray enters its target's front or side face.

	The wall sign (server_local_menu_dwell_sign_demo) sits at (px, py, pz+4),
	i.e. at EYE level: the profile's original fixed pitch of 108 descended
	into the demo floor stone in front of it, which is why no menus_dwell run
	ever reached the sign editor before this header existed.
*/
#ifndef GAME_MENU_DWELL_DEMO_H
#define GAME_MENU_DWELL_DEMO_H

#define MENU_DWELL_DEMO_DZ 3

#define MENU_DWELL_DEMO_FURNACE_DX 0
#define MENU_DWELL_AIM_FURNACE_YAW 0
#define MENU_DWELL_AIM_FURNACE_PITCH 110 /* atan2(1.12, 3.0) = 20.5 deg */

#define MENU_DWELL_DEMO_CHEST_DX (-2)
#define MENU_DWELL_AIM_CHEST_YAW (-34) /* atan2(-2, 3) */
#define MENU_DWELL_AIM_CHEST_PITCH 107 /* atan2(1.12, 3.61) = 17.3 deg */

#define MENU_DWELL_DEMO_LARGE_DX 2 /* second half at MENU_DWELL_DEMO_LARGE_DX + 1 */
#define MENU_DWELL_AIM_LARGE_YAW 34
#define MENU_DWELL_AIM_LARGE_PITCH 107

#define MENU_DWELL_DEMO_DISPENSER_DX (-3) /* -4 sat 5.1 blocks out, past the 4.5 pick ray */
#define MENU_DWELL_AIM_DISPENSER_YAW (-45) /* atan2(-3, 3) */
#define MENU_DWELL_AIM_DISPENSER_PITCH 105 /* atan2(1.12, 4.24) = 14.8 deg */

#define MENU_DWELL_AIM_SIGN_YAW 0
#define MENU_DWELL_AIM_SIGN_PITCH 91 /* sign centre 0.12 below the eye, 4 ahead (z+4) */

#endif

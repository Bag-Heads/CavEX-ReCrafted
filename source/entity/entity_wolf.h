/* Beta 1.7.3 EntityWolf adaptation helpers. */
#ifndef ENTITY_WOLF_H
#define ENTITY_WOLF_H

#include "entity_monster.h"

/* Durable guest Owner tag; host keeps WOLF_LOCAL_PLAYER_NAME unchanged. */
#define WOLF_LOCAL_GUEST_NAME "Local Guest"

struct server_local;
struct java_random;

enum wolf_interact_result {
	WOLF_INTERACT_NONE = 0,
	WOLF_INTERACT_HANDLED,
	WOLF_INTERACT_SMOKE,
	WOLF_INTERACT_HEARTS,
};

bool entity_is_wolf(const struct entity* e);
bool entity_wolf_owner_is_local(const struct entity* e);
struct server_player;
/* Built-in durable identities: Player / CaveWiiGuest records, independent of
 * the guest admission generation. Imported, unknown owner names stay absent. */
struct server_player* entity_wolf_owner_actor(const struct entity* e,
	struct server_local* s);
bool entity_wolf_owner_is_actor(const struct entity* e, struct server_local* s,
	const struct server_player* actor);
uint8_t wolf_restored_flags(bool angry, bool sitting, const char* owner);
void wolf_update_automatic_sit(uint8_t* flags, bool owner_present,
	bool in_water, bool has_retaliation_target);
void wolf_update_current_target_latch(struct entity* wolf,
	struct java_random* rand_src, bool wolf_has_path, bool player_has_pos,
	float player_dist_sq);
bool wolf_looks_with_interest(bool has_current_target, bool wolf_has_path,
	bool angry, bool held_favorite_item);
enum wolf_interact_result entity_wolf_interact(
	struct entity* wolf, struct server_local* s, struct item_data* held);
/* The production caller validates actor/entity world, reach and LOS first;
 * entity_wolf_interact remains the host wrapper for SP and direct goldens. */
enum wolf_interact_result entity_wolf_interact_for(struct entity* wolf,
	struct server_local* s, struct server_player* actor, struct item_data* held);
/* S1-a2: `attacker_actor` is the lifetime-safe identity of the acting actor
 * when attacker_is_player (ACTOR_IDENT_HOST otherwise, and ignored then). It
 * sets the TRANSIENT retaliation target only -- persistent TAME OWNERSHIP
 * stays the owner-NAME field this function never touches. */
enum monster_damage_result entity_wolf_attack_from(
	struct entity* wolf, struct server_local* s, int damage,
	uint32_t attacker_id, bool attacker_is_player, bool attacker_is_arrow,
	bool from_skeleton_arrow, struct actor_ident attacker_actor);
bool entity_wolf_try_teleport_to_owner(struct entity* wolf,
	struct server_local* s, float owner_x, float owner_feet_y, float owner_z);
typedef bool (*wolf_normal_cube_query)(int x, int y, int z, void* context);
bool wolf_find_teleport_site(float owner_x, float owner_feet_y, float owner_z,
	wolf_normal_cube_query normal_cube, void* context, vec3 out);
static inline bool wolf_should_follow(float distance_sq) {
	return distance_sq > 25.0F;
}
static inline bool wolf_should_teleport(float distance_sq) {
	return distance_sq > 144.0F;
}
// [740c583: Entity.isWet] `inWater || canBlockBeRainedOn(floor(posX),
// floor(posY), floor(posZ))` -- the shake trigger's real gate. A wolf
// standing outdoors under open sky during rain is wet with NO water contact
// at all; body_in_water alone (the pre-fix CaveWii gate) misses that case.
// Takes the rain predicate already resolved by the caller (the call site's
// world_can_block_be_rained_on(s, fx, fy, fz)) so this stays a pure,
// world-free, directly-testable composition -- the same shape as
// wolf_update_automatic_sit's injected-booleans style above.
static inline bool wolf_is_wet(bool body_in_water,
	bool can_block_be_rained_on) {
	return body_in_water || can_block_be_rained_on;
}
// wolf_target_id==0 is overloaded: it is ALSO the benign "walking to owner"
// follow state (armed once owner distance > 5 blocks), not just "hostile,
// attack the player". Beta's attackEntity is only ever invoked by the AI
// when playerToAttack/entityToAttack names a genuine hostile target -- the
// follow path (getPathOrWalkableBlock) is architecturally separate and never
// arms it -- so id==0 may only be read as "attack the player" when the wolf
// is actually angry and wild. Both the melee AND leap gates in
// entity_monster.c must use this SAME derivation (a prior leap-only bug
// let a calm tamed wolf pounce on its own owner mid-follow).
// [740c583: EntityWolf.attackEntity]
static inline bool wolf_target_is_hostile_player(uint32_t target_id,
	uint8_t flags) {
	return target_id == 0 && wolf_flag_get(flags, WOLF_FLAG_ANGRY)
		&& !wolf_flag_get(flags, WOLF_FLAG_TAMED);
}
bool entity_wolf_begin_melee(struct entity* wolf, float distance,
	float target_min_y, float target_max_y, int* raw_damage);
bool entity_wolf_melee_in_range(const struct entity* wolf, float distance,
	float target_min_y, float target_max_y);
struct entity* entity_wolf_target(struct entity* wolf, struct server_local* s);
void entity_wolf_try_find_sheep(struct entity* wolf, struct server_local* s);
void entity_wolf_shake_tick(struct entity* wolf, struct server_local* s,
	bool is_wet, bool pathless, bool grounded);

#endif

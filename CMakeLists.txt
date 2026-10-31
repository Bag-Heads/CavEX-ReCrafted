cmake_minimum_required(VERSION 3.14)

project(cavex C)

# -ffp-contract=off: forbid FMA contraction so the worldgen Perlin/Simplex
# doubles match Java's separate multiply+add (FP-parity prerequisite; see
# docs/worldgen_design.md). Applies to the PC build; the Wii Makefile mirrors it.
add_compile_options(-O3 -ffp-contract=off -march=native -g -Wall -Wextra -Wno-unused-parameter)

# Phase-0 detection layer: opt-in AddressSanitizer + UBSan for the headless PC build
# (configure a SEPARATE build dir with -DSANITIZE=ON). Catches the heap overrun / use-after-
# free class that the no-MMU Wii silently tolerates until it freezes minutes later in an
# unrelated subsystem. ASan does NOT link on the MSYS2/mingw toolchain -- run this build
# under WSL/Linux gcc (see tools/asan_soak.sh). -fno-sanitize-recover makes UBSan abort
# (non-zero exit) so it gates CI instead of just printing.
option(SANITIZE "Build with AddressSanitizer + UBSan (headless PC, WSL/Linux only)" OFF)
if(SANITIZE)
	add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
						-fno-sanitize-recover=all)
	add_link_options(-fsanitize=address,undefined)
endif()

# Build stamp for the F3 overlay (screen_ingame.c CW_BUILD_ID): short git hash,
# deterministic dirty-worktree digest when needed, and date, so every capture
# self-identifies one exact source tree rather than only "some dirty build".
# Captured at CONFIGURE time -- our build loops reconfigure before building, so
# in practice it tracks HEAD; treat it as identification, not proof (the
# md5-verify discipline for the Wii .dol stands).
execute_process(COMMAND sh tools/worktree-stamp.sh
				WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
				OUTPUT_VARIABLE CW_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
				ERROR_QUIET)
if(NOT CW_GIT_HASH)
	set(CW_GIT_HASH "nogit")
endif()
string(TIMESTAMP CW_BUILD_DATE "%m.%d")
set_source_files_properties(source/game/gui/screen_ingame.c PROPERTIES
	COMPILE_DEFINITIONS "CW_BUILD_ID=\"${CW_GIT_HASH}_${CW_BUILD_DATE}\"")
set_source_files_properties(source/main.c PROPERTIES
	COMPILE_DEFINITIONS "CW_BUILD_ID=\"${CW_GIT_HASH}_${CW_BUILD_DATE}\"")
set_source_files_properties(source/game/gui/screen_title.c PROPERTIES
	COMPILE_DEFINITIONS "CW_BUILD_ID=\"${CW_GIT_HASH}_${CW_BUILD_DATE}\"")
# pack-cache-0927 (Fable M3): the pack caches fold the source-tree identity
# into every key -- the worktree-stamp part of CW_BUILD_ID (pack_cache.c drops
# the date/-dev suffix), the value the Wii Makefile already stamps -- so a
# PC-baked and a console-built cache agree only for the same tree, and any
# code change rebuilds the cache.
set_source_files_properties(source/platform/pack_cache.c PROPERTIES
	COMPILE_DEFINITIONS "CW_BUILD_ID=\"${CW_GIT_HASH}_${CW_BUILD_DATE}\"")

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)

include(CTest)

# ---- Canonical engine source manifests (docs/build_architecture.md) ----
# ONE authoritative list; every PC target consumes these manifests, never an
# ad-hoc file list or a copy of another target's SOURCES. The manifest-drift
# check near the CTest gate accounts for every on-disk source/**/*.c.
# Variant groups: TUs that legitimately compile more than one way, each with
# exactly one seam (grep-verified 2026-08-27):
#   input:      CAVEWII_AGENT_BRIDGE (app/agent rig vs plain)
#   tick queue: FCAVEX_MAX_EXPLOSIONS_PER_TICK (tnt test overrides to 2)
#   world walk: M_ASSERT(...)=void (walk-guard's shipping-NDEBUG shape)
set(CAVEX_INPUT_VARIANT_SOURCES
	source/game/camera.c
	source/platform/input.c)
set(CAVEX_TICK_QUEUE_VARIANT_SOURCES
	source/network/server_world_tick_queue.c)
set(CAVEX_WORLD_WALK_VARIANT_SOURCES
	source/world.c)
# Step 8a (2026-08-27): the first named SUBSYSTEM STRATUM. Byte-exact,
# oracle-tested worldgen; the hygiene tool's stratum gate holds its
# external include set frozen (see STRATA in check_header_hygiene.py).
# Same objects, same flags -- the boundary is the manifest + the gate;
# a separate static library bought nothing (asserts stance forces
# test/prod variants regardless) and so was not made, per the step-8 plan.
set(CAVEX_WORLDGEN_SOURCES
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/tree_runtime.c
	source/worldgen/decorate.c
	source/worldgen/worldgen.c
	source/worldgen/worldgen_hell.c)

# Step 8b (2026-08-27): the SIM stratum -- block/ + item/ simulation.
# Same objects/flags (boundary = manifest + the STRATA gate in
# check_header_hygiene.py, 38 frozen external edges incl. counted
# render/server direction debt).
set(CAVEX_SIM_SOURCES
	source/block/aabb.c
	source/block/block_bed.c
	source/block/block_bedrock.c
	source/block/block_bookshelf.c
	source/block/block_bricks.c
	source/block/block_brown_mushroom.c
	source/block/block_cactus.c
	source/block/block_cake.c
	source/block/block_cast_block.c
	source/block/block_chest.c
	source/block/chest_link.c
	source/block/block_clay.c
	source/block/block_cobblestone.c
	source/block/block_cobweb.c
	source/block/block_crops.c
	source/block/block_dirt.c
	source/block/block_dispenser.c
	source/block/dispenser_logic.c
	source/block/block_door.c
	source/block/block_double_slab.c
	source/block/block_farmland.c
	source/block/block_fence.c
	source/block/block_fire.c
	source/block/block_flower.c
	source/block/block_furnace.c
	source/block/furnace_logic.c
	source/block/furnace_recipes.c
	source/block/block_glass.c
	source/block/block_glowstone.c
	source/block/block_grass.c
	source/block/block_gravel.c
	source/block/block_ice.c
	source/block/block_iron_chest.c
	source/block/block_locked_chest.c
	source/block/block_jukebox.c
	source/block/jukebox_logic.c
	source/block/block_ladder.c
	source/block/block_button.c
	source/block/block_lava.c
	source/block/block_lever.c
	source/block/block_leaves.c
	source/block/block_log.c
	source/block/block_netherrack.c
	source/block/block_noteblock.c
	source/block/noteblock_logic.c
	source/block/block_obsidian.c
	source/block/block_ore.c
	source/block/block_planks.c
	source/block/block_portal.c
	source/block/block_pressure_plate.c
	source/block/block_pumpkin.c
	source/block/block_rail.c
	source/block/block_red_mushroom.c
	source/block/block_reed.c
	source/block/block_rose.c
	source/block/block_sand.c
	source/block/block_sandstone.c
	source/block/block_sapling.c
	source/block/blocks.c
	source/block/block_sign.c
	source/block/block_slab.c
	source/block/block_snow.c
	source/block/block_spawner.c
	source/block/block_sponge.c
	source/block/block_stairs.c
	source/block/block_stone.c
	source/block/block_tallgrass.c
	source/block/block_tnt.c
	source/block/block_torch.c
	source/block/block_trapdoor.c
	source/block/block_water.c
	source/block/fluid_spread.c
	source/block/fluid_block.c
	source/block/block_wool.c
	source/block/block_workbench.c
	source/block/block_tree2d.c
	source/block/block_minecart.c
	source/block/block_redstone_wire.c
	source/block/block_repeater.c
	source/block/redstone_logic.c
	source/block/redstone_power.c
	source/block/redstone_repeater.c
	source/block/redstone_piston.c
	source/block/piston_mobility.c
	source/block/piston_texture.c
	source/block/piston_moving.c
	source/block/tnt_texture.c
	source/block/redstone_piston_server.c
	source/block/block_piston.c
	source/block/redstone_server.c
	source/block/face_occlusion.c
	source/item/window_container.c
	source/item/item_nbt.c
	source/item/inventory.c
	source/item/inventory_click.c
	source/item/inventory_sort.c
	source/item/items.c
	source/item/tool.c
	source/item/recipe.c
	source/item/items/item_sugarcane.c
	source/item/items/item_porkchop.c
	source/item/items/item_porkchop_cooked.c
	source/item/items/item_door_wood.c
	source/item/items/item_door_iron.c
	source/item/items/item_apple.c
	source/item/items/item_apple_golden.c
	source/item/items/item_fish.c
	source/item/items/item_fish_cooked.c
	source/item/items/item_bread.c
	source/item/items/item_cookie.c
	source/item/items/item_dye.c
	source/item/items/item_hoe.c
	source/item/items/item_record.c
	source/item/items/item_shears.c
	source/item/items/item_mushroom_stew.c
	source/item/items/item_egg_zombie.c
	source/item/items/item_flint_steel.c
	source/item/items/item_seeds.c
	source/item/items/item_bed.c
	source/item/items/item_bucket.c
	source/item/items/item_bucket_lava.c
	source/item/items/item_bucket_water.c
	source/item/items/item_bucket_milk.c
	source/item/items/item_cake.c
	source/item/items/item_painting.c
	source/item/items/item_minecart.c
	source/item/items/item_boat.c
	source/item/items/item_minecart_chest.c
	source/item/items/item_minecart_furnace.c
	source/item/items/item_redstone.c
	source/item/items/item_repeater.c
	source/item/items/item_map.c
	source/item/items/item_map_render_convert.c
)

# Step 8c (2026-08-27): the WORLD stratum (chunk/mesher/lighting;
# world.c itself lives in the walk-variant group and
# world_visibility is header-only -- both are stratum MEMBERS for
# the gate regardless). Boundary = manifest + STRATA gate (18
# frozen edges in check_header_hygiene.py).
set(CAVEX_WORLD_SOURCES
	source/column_observer.c
	source/column_view.c
	source/chunk_mesher.c
	source/chunk.c
	source/light_brightness.c
	source/lighting.c
	source/stack.c
	source/world_redraw.c
)

# Steps 8d-8f (2026-08-27): the last three strata. Boundaries =
# manifest + the STRATA gate (frozen edges: entity 38, server 56,
# client 42 -- harvested reality incl. counted direction debt).
# Variant-group TUs (tick queue, camera/input, world.c) stay in
# their variant manifests but are gate-governed members regardless.
set(CAVEX_ENTITY_SOURCES
	source/entity/entity.c
	source/entity/armor.c
	source/entity/entity_local_player.c
	source/entity/entity_item.c
	source/entity/entity_monster.c
	source/entity/mob_motion.c
	source/entity/mob_motion_probe.c
	source/entity/entity_wolf.c
	source/entity/entity_minecart.c
	source/entity/entity_arrow.c
	source/entity/block_ray.c
	source/entity/entity_egg.c
	source/entity/entity_fireball.c
	source/entity/entity_primed_tnt.c
	source/entity/entity_boat.c
	source/entity/entity_fish.c
	source/entity/entity_remote_player.c
	source/entity/entity_painting.c
	source/entity/entity_falling_sand.c
	source/entity/pathfinding.c
	source/entity/monsters_object.c
)
set(CAVEX_SERVER_SOURCES
	source/network/server_creative.c
	source/network/client_interface.c
	source/network/client_admission.c
	source/network/client_frame_worlds.c
	source/network/piston_moving_transport.c
	source/network/complex_block_archive.c
	source/network/death_drop_journal.c
	source/network/death_drop_recovery.c
	source/network/entity_archive.c
	source/network/level_archive.c
	source/network/world_copy.c
	source/network/map_data.c
	source/network/map_data_archive.c
	source/network/map_idcounts.c
	source/network/region_archive.c
	source/network/save_path.c
	source/network/region_pool.c
	source/network/checkpoint_journal_guard.c
	source/network/server_dispenser.c
	source/network/server_furnace.c
	source/network/server_interface.c
	source/network/server_jukebox.c
	source/network/server_noteblock.c
	source/network/portal_teleporter.c
	source/network/server_transfer.c
	source/network/server_local.c
	source/network/server_local_guest.c
	source/network/vehicle_smoke_probe.c
	source/network/split_offer_piston_probe.c
	source/network/save_reload_probe.c
	source/network/server_spawner.c
	source/network/server_weather.c
	source/network/spawner.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/network/tile_entities_store.c
	source/network/tile_restore_plan.c
	source/network/server_world.c
	source/network/server_world_block_filter.c
	source/network/inventory_logic.c
	source/network/inventory_achievements.c
	source/network/inventory_quick_move.c
	source/network/inventory_player.c
	source/network/inventory_crafting.c
	source/network/inventory_furnace.c
	source/network/inventory_dispenser.c
	source/network/inventory_chest.c
	source/network/inventory_chest_shared.c
	source/network/inventory_minecart_chest.c
	source/network/inventory_iron_chest.c
	source/network/inventory_double_chest.c
	source/network/inventory_sign.c
	source/network/window_registry.c
)
set(CAVEX_CLIENT_SOURCES
	source/game/client_world_context.c
	source/game/gui/screen_ingame.c
	source/game/gui/achievement_notice.c
	source/game/gui/hud_survival_row.c
	source/game/gui/screen_load_world.c
	source/game/gui/loading_tips.c
	source/game/gui/screen_select_world.c
	source/game/gui/screen_texturepacks.c
	source/game/gui/screen_beta_import.c
	source/game/gui/backdrop_bake.c
	source/game/gui/startup_credits.c
	source/game/gui/loading_notice.c
	source/game/gui/spawn_search_wait.c
	source/game/gui/screen_inventory.c
	source/game/gui/screen_iron_chest.c
	source/game/gui/screen_crafting.c
	source/game/gui/screen_furnace.c
	source/game/gui/screen_chest.c
	source/game/gui/screen_dispenser.c
	source/game/gui/chest_layout.c
	source/game/gui/screen_sign.c
	source/game/gui/sign_osk_model.c
	source/game/gui/screen_chat.c
	source/game/gui/chat_composer_model.c
	source/game/gui/chat_hud.c
	source/game/gui/screen_pause.c
	source/game/gui/screen_achievements.c
	source/game/gui/screen_stats_save.c
	source/game/gui/container_menu.c
	source/game/gui/container_compact.c
	source/game/gui/recipe_browser.c
	source/game/gui/local_menu_host.c
	source/game/gui/local_pause.c
	source/game/gui/menu_header.c
	source/game/gui/screen_options.c
	source/game/gui/screen_video_settings.c
	source/game/gui/screen_controller_settings.c
	source/game/gui/screen_creative_picker.c
	source/game/gui/creative_picker_list.c
	source/game/gui/screen_death.c
	source/game/gui/screen_title.c
	source/game/gui/screen_create_world.c
	source/game/gui/cworld_form_model.c
	source/game/gui/play_session.c
	source/game/gui/screen_confirm.c
	source/game/gui/screen_convert_backup.c
	source/game/gui/screen_rename_world.c
	source/game/gui/widget_osk.c
	source/game/gui/widget_osk_frontend.c
	source/game/gui/widget_slotlist.c
	source/game/gui/screen.c
	source/game/autopilot.c
	source/game/attack_assist.c
	source/game/menu_autopilot_script.c
	source/game/menu_autopilot.c
	source/game/mp_reconnect_soak.c
	source/game/client_display_tick.c
	source/game/game_state.c
	source/game/client_session.c
	source/game/hw_run.c
	source/game/pool_selftest.c
	source/game/split_recovery_script.c
	source/game/split_portal_quit_script.c
	source/game/vehicle_smoke_script.c
	source/game/split_offer_piston_script.c
	source/game/save_reload_script.c
	source/game/chat_login_script.c
	source/game/mp_play_script.c
	source/game/mp_pause_menu_script.c
	source/game/sign_text_store.c
	source/game/sign_text_render_feed.c
	source/graphics/biome_color.c
	source/graphics/compass_fx.c
	source/graphics/watch_fx.c
	source/graphics/gfx_util.c
	source/graphics/gui_util.c
	source/graphics/render_block.c
	source/graphics/render_item.c
	source/graphics/render_model.c
	source/graphics/render_entity.c
	source/graphics/render_entity_fire.c
	source/graphics/render_name_tag.c
	source/graphics/render_name_tag_draw.c
	source/graphics/render_piston_moving.c
	source/graphics/render_sign_text.c
	source/graphics/render_sign_text_draw.c
	source/graphics/render_weather.c
	source/graphics/render_lightning.c
	source/graphics/texture_atlas.c
	source/graphics/water_fx.c
	source/platform/audio.c
	source/platform/audio_music.c
	source/platform/audio_registry.c
	source/platform/audio_cache.c
	source/platform/pack_cache.c
	source/platform/pack_manifest.c
	source/platform/tex_gx_conv.c
	source/platform/texture_cache.c
	source/platform/audio_deferred.c
	source/platform/displaylist.c
	source/platform/displaylist_scene_hash.c
	source/platform/gfx.c
	source/platform/hw_telemetry.c
	source/platform/service_observer.c
	source/platform/mem2.c
	source/platform/thread.c
	source/platform/input_inventory.c
	source/platform/controller_events.c
	source/platform/input_glyph.c
	source/platform/time.c
	source/platform/texture.c
	source/platform/tex_beta_compose.c
	source/platform/beta_import.c
	source/platform/beta_import_jar.c
	source/platform/beta_import_sound.c
	source/platform/texture_decode.c
	source/platform/texture_mip.c
	source/platform/watchdog.c
	source/platform/panic.c
)

set(CAVEX_ENGINE_CORE_SOURCES
	${CAVEX_ENTITY_SOURCES}
	${CAVEX_SERVER_SOURCES}
	${CAVEX_CLIENT_SOURCES}
	${CAVEX_WORLD_SOURCES}
	${CAVEX_SIM_SOURCES}
	${CAVEX_WORLDGEN_SOURCES}
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	source/arena.c
	source/carve_observer.c
	source/tps_observer.c
	source/settle_observer.c
	source/daytime.c
	source/util.c
	source/config.c
	source/texturepacks.c
	source/particle.c
	source/lodepng/lodepng.c
	source/parson/parson.c
	# LCE-01 lane B B1 (2026-09-14): achievement/statistic data model and its
	# versioned .cwstats file (docs/achievements_design.md 1.1/1.4/2/4).
	source/game/achievements/achievements_state.c
	source/game/achievements/achievements_persist.c
	# LCE-01 lane B B2 (2026-09-14): the game_event seam consumer -- ring,
	# coalescer, acceptance matrix, the sixteen unlock rules and the
	# bind/unbind/flush lifecycle (docs/achievements_design.md 1.1/1.2/3/5/6).
	source/game/achievements/achievements.c
	# LCE-01 lane B B4 (2026-09-14): the public API lane A and lane C read --
	# per-owner notice peek/ack, the screen data rows, Beta's three value
	# formatters and the status query (docs/achievements_design.md 1.6/7).
	source/game/achievements/achievements_api.c
	# LCE-02 lane B B3 (2026-09-14): Beta's statistic ROUNDING as pure
	# functions, so lane A's five movement emit sites (design 5.3) all produce
	# the centimetre EntityPlayer.addMovementStat / fall() produce -- three
	# different cast chains, one tested place (docs/achievements_design.md 1.4).
	source/game/stats/stats_beta.c
)
set(CAVEX_ENGINE_SOURCES
	${CAVEX_ENGINE_CORE_SOURCES}
	${CAVEX_WORLD_WALK_VARIANT_SOURCES}
	${CAVEX_TICK_QUEUE_VARIANT_SOURCES}
	${CAVEX_INPUT_VARIANT_SOURCES})
set(CAVEX_APP_ONLY_SOURCES
	source/main.c
	source/platform/remote_capture.c)
# MP + agent-bridge app modules stay OUT of the engine manifest on purpose:
# keeps every env test's link free of the socket provider and the
# gstate-reading collector (original rationale preserved from the
# target_sources(cavex ...) blocks these lists replace).
set(CAVEX_MP_SOURCES
	source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	source/platform/net_bringup.c
	source/game/gui/screen_multiplayer_connect.c
	source/game/gui/screen_multiplayer_setup.c)
set(CAVEX_AGENT_APP_SOURCES
	source/game/state_export.c
	source/game/state_export_emit.c
	source/game/agent_input.c)
# On-disk .c files under source/ that are deliberately NOT PC translation
# units. The drift check requires every file to be declared SOMEWHERE.
set(CAVEX_NON_PC_SOURCES
	# offline texture-FX generators (never linked; see fire-lava-fx notes)
	source/graphics/flame_fx.c
	source/graphics/lava_fx.c
	source/graphics/portal_fx.c
	source/graphics/water_flow_fx.c
	# textually #included by the source/platform/*.c wrappers, not own TUs
	source/platform/pc/audio.c
	source/platform/pc/displaylist.c
	source/platform/pc/gfx.c
	source/platform/pc/net_socket.c
	source/platform/pc/texture.c
	# Wii Makefile only
	source/platform/wii/audio.c
	source/platform/wii/displaylist.c
	source/platform/wii/gfx.c
	source/platform/wii/net_bringup.c
	source/platform/wii/net_socket.c
	source/platform/wii/panic.c
	source/platform/wii/texture.c
	source/platform/wii/watchdog.c)

# ---- Shared PRODUCTION engine objects ----
# Compiled ONCE with the plain production profile; linked by soak_stream,
# bench_far_geomip, and (when CAVEX_IPO is OFF) the cavex app itself. No test
# defines, no -UNDEBUG: production stance, same as these targets always had.
add_library(cavex_prod_core OBJECT
	${CAVEX_ENGINE_CORE_SOURCES}
	${CAVEX_WORLD_WALK_VARIANT_SOURCES}
	${CAVEX_TICK_QUEUE_VARIANT_SOURCES})
target_compile_definitions(cavex_prod_core PUBLIC PLATFORM_PC CGLM_ALL_UNALIGNED)
add_library(cavex_prod_input OBJECT ${CAVEX_INPUT_VARIANT_SOURCES})
target_compile_definitions(cavex_prod_input PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
add_library(cavex_prod_input_agent OBJECT ${CAVEX_INPUT_VARIANT_SOURCES})
target_compile_definitions(cavex_prod_input_agent PRIVATE PLATFORM_PC
	CGLM_ALL_UNALIGNED CAVEWII_AGENT_BRIDGE)
foreach(_cw_pool cavex_prod_core cavex_prod_input cavex_prod_input_agent)
	target_include_directories(${_cw_pool} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/source
		${CMAKE_CURRENT_SOURCE_DIR}/third_party)
	set_target_properties(${_cw_pool} PROPERTIES C_STANDARD 99)
endforeach()

add_executable(cavex ${CAVEX_APP_ONLY_SOURCES})

# CAVEWII_AGENT_BRIDGE gates the agent input/look hooks compiled INTO the shared
# engine sources (input.c, camera.c, main.c). Defined on the GAME target ONLY (not on
# the ~17 test exes that reuse those sources via ${_cavex_srcs}), so the agent calls
# compile out of the tests -- they never link agent_input.c. PC dev rig.
target_compile_definitions(cavex PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED CAVEWII_AGENT_BRIDGE)
target_include_directories(cavex PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

set_target_properties(
	cavex PROPERTIES
	C_STANDARD 99
	OUTPUT_NAME cavewii_v2
)

# IPO/LTO on the main app helps release perf, but it hides linker errors,
# slows iteration, and (IPO objects are GIMPLE) prevents sharing the
# production engine objects. Default: ON for non-test configs (pc-release),
# OFF when BUILD_TESTING is enabled so the app links the shared objects.
# Override explicitly with -DCAVEX_IPO=ON/OFF.
if(NOT DEFINED CAVEX_IPO)
	if(BUILD_TESTING)
		set(CAVEX_IPO FALSE)
	else()
		set(CAVEX_IPO TRUE)
	endif()
endif()
set_property(TARGET cavex PROPERTY INTERPROCEDURAL_OPTIMIZATION ${CAVEX_IPO})
if(CAVEX_IPO)
	# The IPO app compiles its own engine copy: GIMPLE objects can't be
	# shared with the non-IPO consumers.
	target_sources(cavex PRIVATE ${CAVEX_ENGINE_SOURCES})
else()
	target_link_libraries(cavex cavex_prod_core cavex_prod_input_agent)
endif()

target_link_libraries(cavex ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)

# Hard boundary (docs/build_architecture.md step 4): the playable app must
# never contain test-instrumented engine objects. See source/world.c's
# cavewii_test_engine_marker and cmake/check_no_test_marker.cmake.
add_custom_command(TARGET cavex POST_BUILD
	COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:cavex>
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_no_test_marker.cmake
	VERBATIM)

# ---- Headless streaming soak harness (Phase-0 detection layer) ----
# Drives the REAL generate->mesh->evict->remesh loop -- the server thread + mesher worker
# thread + this main thread all sharing the MEM2/newlib heaps -- with a scripted player walk
# that crosses chunk boundaries thousands of times. Run under ASan (-DSANITIZE=ON, WSL) it
# catches the cross-thread heap overrun/UAF off-hardware in seconds instead of a multi-build
# on-Wii freeze hunt. Reuses cavex's EXACT source set minus its main(), plus the harness entry
# point. Links GL but never calls gfx_setup, so it runs headless (no window/context).
# (the old ${_cavex_srcs} SOURCES-copy is gone: targets consume the
# CAVEX_*_SOURCES manifests or link the shared object pools directly)

# MP-S0 is PC-only. Add it after the reusable source snapshot so unrelated
# env tests do not inherit the socket provider or its platform library.
# mp_session.c (the GUI<->main-loop provider-selection seam, plan section
# 3.3) and screen_multiplayer_connect.c (the connecting/cancel/error screen)
# join it here for the same reason: both are referenced only from main.c
# (screen_title.c only touches the always-shared screen_multiplayer_connect
# POINTER in screen.c, never the impl struct or mp_session directly -- see
# screen.h's comment), so keeping them out of ${_cavex_srcs} keeps every
# other env test's link free of the socket provider.
target_sources(cavex PRIVATE ${CAVEX_MP_SOURCES})
if(WIN32)
	target_link_libraries(cavex ws2_32)
endif()

# CAVEWII_AGENT bridge: the live state-export collector + the pure JSON writer. Added
# to the GAME target ONLY, AFTER the _cavex_srcs capture above, so the ~17 test exes
# that reuse ${_cavex_srcs} do NOT pull in the gstate-reading collector (which would
# need engine glue they don't link). PC-only at compile time (PLATFORM_PC guard).
target_sources(cavex PRIVATE ${CAVEX_AGENT_APP_SOURCES})

add_executable(soak_stream tools/soak_stream.c)
target_link_libraries(soak_stream cavex_prod_core cavex_prod_input)
target_include_directories(soak_stream PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(soak_stream PROPERTIES C_STANDARD 99)
target_link_libraries(soak_stream ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)

# pack-cache-0927 HOST tool (NOT a test, never Wii): the PC producer of the
# opt-in pack caches (audio + textures) and of the helper digest manifest.
# opt-in audio pack cache. Runs the PRODUCTION registry scan (production
# object pool) and publishes Wii/PC generations through audio_cache.c
# (Astra astra-packcacheB-0927 item 3; the Python pack helper calls it).
add_executable(audio_cache_tool tools/audio_cache_tool.c)
target_link_libraries(audio_cache_tool cavex_prod_core cavex_prod_input)
target_include_directories(audio_cache_tool PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(audio_cache_tool PROPERTIES C_STANDARD 99)
target_link_libraries(audio_cache_tool ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)

# S1 HOST sound transcode tool (NOT a test, NOT part of cavex, never Wii):
# one Beta resource ogg/wav -> the engine's exact cache WAV (PCM16 mono,
# native rate), plus --manifest (the registry's id/key/glob table from the
# shared audio_sound_table.h X-macro). Driven per file by
# tools/make_beta_pack.ps1's sound stage. Standalone stb_vorbis copy in its
# own binary; the test suite instead compiles this file into test_audio_env
# header-only against the engine's copy (see that target).
add_executable(sound_transcode tools/sound_transcode.c)
target_include_directories(sound_transcode PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tools
)
set_target_properties(sound_transcode PROPERTIES C_STANDARD 99)
target_link_libraries(sound_transcode m)

if(BUILD_TESTING)

# ---- Shared TEST engine objects (steps 3+4, review-hardened 2026-08-27) ----
# The engine compiles ONCE for all full-engine tests. The 2026-08-27 seam
# audits (3 agents, every #ifdef site of all 14 CAVEWII_*_TEST defines)
# proved each one DORMANT: test-only functions, runtime-armed fail hooks
# defaulting off, and passive dead-write counters read only by test getters
# (accepted under the explicit dead-write-counters-are-dormant ruling).
# CAVEWII_TESTING is THE explicit test-instrumentation switch: it compiles
# the link-time marker (tests/support/test_engine_marker.c) into every test
# object set, the playable-target boundary check rejects it at configure
# time, and the post-link marker gates keep it out of shipping binaries.
# Note: CAVEWII_FAR_SLOT_DEADLOCK_TEST's guard variable is declared under
# CAVEWII_SERVER_LOCAL_TEST (server_local.c) -- the two stay paired here.
# Defines are PUBLIC on cavex_test_core: a pooled test's own tests/*.c MUST
# compile with the exact define set of the pool objects (headers expose test
# APIs under them); inheritance via the link edge enforces that.
# -UNDEBUG: the forced-asserts-for-tests rule lives in these pool flags.
# CAVEWII_RAND_SEED_TEST: the test seed policy (ASTRA-SEED-POLICY-0925,
# util.h): clock seeds become a fixed default or CW_TEST_SEED. It is also
# applied to the frontend test pool and to EVERY test_ target by the sweep
# below (standalone tests that compile util.c themselves), and the
# playable/production boundary check rejects it like every CAVEWII_*_TEST.
set(CAVEX_TEST_ENGINE_DEFINES
	PLATFORM_PC CGLM_ALL_UNALIGNED
	CAVEWII_TESTING
	CAVEWII_RAND_SEED_TEST
	CAVEWII_SERVER_LOCAL_TEST CAVEWII_CLIENT_INTERFACE_TEST
	CAVEWII_MAP_DATA_ARCHIVE_TEST CAVEWII_MAP_IDCOUNTS_TEST
	CAVEWII_REGION_ARCHIVE_TEST
	CAVEWII_COMPLEX_BLOCK_ARCHIVE_TEST CAVEWII_TILE_ENTITIES_STORE_TEST
	CAVEWII_FLUID_LIFECYCLE_TEST CAVEWII_FAR_SLOT_DEADLOCK_TEST
	CAVEWII_BLOCK_FIRE_TEST CAVEWII_DEATH_DROP_TEST
	CAVEWII_PORTAL_TELEPORTER_TEST CAVEWII_PISTON_RENDER_TEST
	CAVEWII_DISPLAYLIST_TEST CAVEWII_PARTICLE_TEST)
add_library(cavex_test_core OBJECT
	${CAVEX_ENGINE_CORE_SOURCES}
	tests/support/test_engine_marker.c)
target_compile_definitions(cavex_test_core PUBLIC ${CAVEX_TEST_ENGINE_DEFINES})
# Variant flavors: same TU, different single seam. A test links exactly one
# flavor per group (the defaults, unless it is the variant's own test).
add_library(cavex_test_world OBJECT ${CAVEX_WORLD_WALK_VARIANT_SOURCES})
add_library(cavex_test_world_massert_off OBJECT ${CAVEX_WORLD_WALK_VARIANT_SOURCES})
target_compile_options(cavex_test_world_massert_off PRIVATE
	"-DM_ASSERT(...)=((void)0)")
add_library(cavex_test_tickq OBJECT ${CAVEX_TICK_QUEUE_VARIANT_SOURCES})
add_library(cavex_test_tickq_tnt OBJECT ${CAVEX_TICK_QUEUE_VARIANT_SOURCES})
target_compile_definitions(cavex_test_tickq_tnt PRIVATE
	FCAVEX_MAX_EXPLOSIONS_PER_TICK=2)
add_library(cavex_test_input OBJECT ${CAVEX_INPUT_VARIANT_SOURCES})
add_library(cavex_test_input_agent OBJECT ${CAVEX_INPUT_VARIANT_SOURCES})
target_compile_definitions(cavex_test_input_agent PRIVATE CAVEWII_AGENT_BRIDGE)
foreach(_cw_pool cavex_test_world cavex_test_world_massert_off
		cavex_test_tickq cavex_test_tickq_tnt
		cavex_test_input cavex_test_input_agent)
	target_compile_definitions(${_cw_pool} PRIVATE ${CAVEX_TEST_ENGINE_DEFINES})
endforeach()
foreach(_cw_pool cavex_test_core cavex_test_world cavex_test_world_massert_off
		cavex_test_tickq cavex_test_tickq_tnt
		cavex_test_input cavex_test_input_agent)
	target_compile_options(${_cw_pool} PRIVATE -UNDEBUG)
	target_include_directories(${_cw_pool} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/source
		${CMAKE_CURRENT_SOURCE_DIR}/third_party)
	set_target_properties(${_cw_pool} PROPERTIES C_STANDARD 99)
endforeach()
# The standard full test engine is the DIRECT link set
#   cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
# (object-library objects only reach a consumer that links them directly --
# an INTERFACE bundle drops them on this CMake, proven 2026-08-27; variant
# tests swap exactly one member for its flavor).

add_executable(test_world_copy tests/test_world_copy.c source/network/world_copy.c
	source/cNBT/buffer.c source/cNBT/nbt_loading.c source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c source/cNBT/nbt_util.c)
target_link_libraries(test_world_copy PRIVATE ZLIB::ZLIB)
target_include_directories(test_world_copy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_world_copy PROPERTIES C_STANDARD 99)
add_test(NAME world_copy COMMAND test_world_copy)

add_executable(test_world_bfs
	tests/test_world_bfs.c
)
add_executable(test_client_context_route
	tests/test_client_context_route.c
	source/game/client_world_context.c
)
add_test(NAME test_client_context_route COMMAND test_client_context_route)
# split-dims B1 contract tests (b1-prep-0925, ASTRA-B1-RULINGS-0925): stale-
# generation rejection across slot re-creation, reset quiescing producers until
# every lease kind drains, a world switch rejecting the predecessor's in-flight
# mesh results while its leases drain, per-slot lifetime high-water exhaustion
# (no wrap/reuse; a failed init/reset leaves the context unchanged) and cleanup-
# before-resumption (after the drain, producers stay parked until the owner
# completes old-payload cleanup; retirement never resumes acquisition), and stale
# queued traffic across a stream reset and a reconnect with the server identity
# and the client epoch validated separately (ASTRA-B1-PHASE1-0925 #2). Authored
# RED (WILL_FAIL TRUE, cf. test_window_registry_unregistered) and promoted to
# ordinary green tests 2026-09-25 in B1 phase 1; the `_red_` names are kept so
# each contract's history stays contiguous. Harness: tests/client_context_red.h.
# split-dims B1 phase 2 (b1-wire-0925): the client world-context registry that
# client routing is wired to (stamped/unstamped routing, HOLD while quiescing,
# predecessor overlap and reclaim, the 12-entry pool bound), and the C1 -> B1
# guest-Y hand-off (main.c's per-heartbeat guest sample through the real
# heartbeat formatter: join, changed Y, leave, respawn).
add_executable(test_b1_registry
	tests/test_b1_registry.c
	source/game/client_world_context.c
)
add_test(NAME test_b1_registry COMMAND test_b1_registry)
add_executable(test_b1_guest_eye_heartbeat
	tests/test_b1_guest_eye_heartbeat.c
	source/game/hw_run.c
	source/platform/hw_telemetry.c
	source/platform/thread.c
	source/platform/watchdog.c
	source/platform/time.c
)
target_link_libraries(test_b1_guest_eye_heartbeat m)
target_compile_definitions(test_b1_guest_eye_heartbeat PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED CAVEWII_HW_TELEMETRY_TEST)
target_include_directories(test_b1_guest_eye_heartbeat PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
set_target_properties(test_b1_guest_eye_heartbeat PROPERTIES C_STANDARD 99)
add_test(NAME test_b1_guest_eye_heartbeat COMMAND test_b1_guest_eye_heartbeat)
add_executable(test_b1_route_families tests/test_b1_route_families.c)
target_link_libraries(test_b1_route_families cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_b1_route_families PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_b1_route_families PROPERTIES C_STANDARD 99)
add_test(NAME test_b1_route_families COMMAND test_b1_route_families)
# Split-dims B2 prep (b2-0926): world-bound client payloads, default OFF.
add_executable(test_b2_world_payloads tests/test_b2_world_payloads.c)
target_link_libraries(test_b2_world_payloads cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_b2_world_payloads PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_b2_world_payloads PROPERTIES C_STANDARD 99)
add_test(NAME test_b2_world_payloads COMMAND test_b2_world_payloads)
set_tests_properties(test_b2_world_payloads PROPERTIES TIMEOUT 120)
# B2 Fable F5: slot-1 GPU / display-list lifetime on reset/release (wrapped destroy).
add_executable(test_b2_gpu_lifetime tests/test_b2_gpu_lifetime.c)
target_link_libraries(test_b2_gpu_lifetime cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_b2_gpu_lifetime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_options(test_b2_gpu_lifetime PRIVATE -Wl,--wrap=displaylist_destroy)
set_target_properties(test_b2_gpu_lifetime PROPERTIES C_STANDARD 99)
add_test(NAME test_b2_gpu_lifetime COMMAND test_b2_gpu_lifetime)
set_tests_properties(test_b2_gpu_lifetime PROPERTIES TIMEOUT 120)
# B2 view scope (T0) + guest-world sound (T5, R2-2): wrapped audio_play3d.
add_executable(test_b2_view_scope tests/test_b2_view_scope.c)
target_link_libraries(test_b2_view_scope cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_b2_view_scope PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_options(test_b2_view_scope PRIVATE -Wl,--wrap=audio_play3d)
set_target_properties(test_b2_view_scope PROPERTIES C_STANDARD 99)
add_test(NAME test_b2_view_scope COMMAND test_b2_view_scope)
set_tests_properties(test_b2_view_scope PROPERTIES TIMEOUT 120)
# B2 main.c wiring (B2-MAINC-PLAN T1-T3): the per-frame world passes, wrapped world_*.
add_executable(test_b2_frame_worlds tests/test_b2_frame_worlds.c)
target_link_libraries(test_b2_frame_worlds cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_b2_frame_worlds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_options(test_b2_frame_worlds PRIVATE
	-Wl,--wrap=world_pre_render -Wl,--wrap=world_pre_render_views
	-Wl,--wrap=world_pre_render_clear -Wl,--wrap=world_update_lighting
	-Wl,--wrap=world_update_lod -Wl,--wrap=world_update_lod_views
	-Wl,--wrap=world_near_ring_status -Wl,--wrap=world_build_chunks
	-Wl,--wrap=world_render_completed -Wl,--wrap=world_gpu_wait_idle)
set_target_properties(test_b2_frame_worlds PROPERTIES C_STANDARD 99)
add_test(NAME test_b2_frame_worlds COMMAND test_b2_frame_worlds)
set_tests_properties(test_b2_frame_worlds PROPERTIES TIMEOUT 120)
# B2 activation gates (b2-gates-0926): in-place guest-world reset + world-addressed resend.
add_executable(test_b2_gates tests/test_b2_gates.c)
target_link_libraries(test_b2_gates cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_b2_gates PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_b2_gates PROPERTIES C_STANDARD 99)
add_test(NAME test_b2_gates COMMAND test_b2_gates)
set_tests_properties(test_b2_gates PROPERTIES TIMEOUT 120)
# split-dims B2 -> A2-T client contract (b2-a2t-0926): concurrent client
# worlds, host join, stamped BEGIN scope, guest session kept across a host switch.
# WORLD-RETIRE-SRV (Fable B2-A2T F1, server half): a vacated LIVE world drains
# to zero resident columns, stays LIVE and retires its client slot once.
add_executable(test_world_retire_srv tests/test_world_retire_srv.c)
target_link_libraries(test_world_retire_srv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_world_retire_srv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_world_retire_srv PROPERTIES C_STANDARD 99)
add_test(NAME test_world_retire_srv COMMAND test_world_retire_srv)
add_executable(test_b2_a2t_client tests/test_b2_a2t_client.c)
target_link_libraries(test_b2_a2t_client cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_b2_a2t_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_b2_a2t_client PROPERTIES C_STANDARD 99)
add_test(NAME test_b2_a2t_client COMMAND test_b2_a2t_client)
set_tests_properties(test_b2_a2t_client PROPERTIES TIMEOUT 120)
# b2-a2t-cap-0927: A2-T admission client half -- capacity reserve/commit/release
# + landing-ready timing (Astra astra-a2t-0927).
add_executable(test_b2_a2t_admission tests/test_b2_a2t_admission.c)
target_link_libraries(test_b2_a2t_admission cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_b2_a2t_admission PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_b2_a2t_admission PROPERTIES C_STANDARD 99)
add_test(NAME test_b2_a2t_admission COMMAND test_b2_a2t_admission)
set_tests_properties(test_b2_a2t_admission PROPERTIES TIMEOUT 120)
foreach(_cw_b1_red stale_gen reset_quiesce switch_drain exhaustion cleanup_resume stale_queue)
	add_executable(test_client_ctx_red_${_cw_b1_red}
		tests/test_client_ctx_red_${_cw_b1_red}.c
		source/game/client_world_context.c
	)
	add_test(NAME test_client_ctx_red_${_cw_b1_red} COMMAND test_client_ctx_red_${_cw_b1_red})
endforeach()
# split-dims lane B card B-NOW (bnow-0926, astra-next-0926 "B NOW"): real-producer
# reset/reconnect isolation. Production server_local emitters -> the real clin_inbox ->
# the real client_interface consumer (harness: tests/test_b1_reset_harness.h). Clauses
# that rely on producer stamps A/C have not applied yet exit 1 through the b1r_red()
# ledger (never abort): each such binary runs twice -- `base` (setup + every GREEN
# invariant; misses only recorded) as an ordinary test, and `_red` registered
# WILL_FAIL, naming the producer that must stamp. Drop WILL_FAIL when that producer
# stamps (the red run then passes and WILL_FAIL turns the suite red on purpose).
set(_cw_b1_reset_red stale_terrain stale_time_weather session_reuse)
set(_cw_b1_reset_green mesher_lease pooled_release)
foreach(_cw_b1_reset IN LISTS _cw_b1_reset_red _cw_b1_reset_green)
	add_executable(test_b1_reset_${_cw_b1_reset} tests/test_b1_reset_${_cw_b1_reset}.c)
	target_link_libraries(test_b1_reset_${_cw_b1_reset} cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
		${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
	target_include_directories(test_b1_reset_${_cw_b1_reset} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
	set_target_properties(test_b1_reset_${_cw_b1_reset} PROPERTIES C_STANDARD 99)
	add_test(NAME test_b1_reset_${_cw_b1_reset} COMMAND test_b1_reset_${_cw_b1_reset} base)
	set_tests_properties(test_b1_reset_${_cw_b1_reset} PROPERTIES TIMEOUT 240)
	if(_cw_b1_reset IN_LIST _cw_b1_reset_red)
		add_test(NAME test_b1_reset_${_cw_b1_reset}_red COMMAND test_b1_reset_${_cw_b1_reset} red)
		# B1 local stamping (lane B handoff, 2026-09-26): every named producer
		# stamps now, so the red runs PASS and are enforced (WILL_FAIL dropped).
		set_tests_properties(test_b1_reset_${_cw_b1_reset}_red PROPERTIES TIMEOUT 240)
	endif()
endforeach()
# G6 S1 (b1stamp-remote-0926): local and remote world identities are disjoint,
# both orders (remote->local, local->remote) through the real producers.
add_executable(test_b1_stamp_namespace tests/test_b1_stamp_namespace.c)
target_link_libraries(test_b1_stamp_namespace cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_b1_stamp_namespace PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_b1_stamp_namespace PROPERTIES C_STANDARD 99)
add_test(NAME test_b1_stamp_namespace COMMAND test_b1_stamp_namespace)
set_tests_properties(test_b1_stamp_namespace PROPERTIES TIMEOUT 240)
target_link_libraries(test_world_bfs m)
target_include_directories(test_world_bfs PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
add_test(NAME test_world_bfs COMMAND test_world_bfs)

add_executable(test_arena
	source/arena.c
	tests/test_arena.c
)
target_link_libraries(test_arena m)
target_include_directories(test_arena PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
add_test(NAME test_arena COMMAND test_arena)

add_executable(test_vertex_light_equiv
	tests/test_vertex_light_equiv.c
)
target_link_libraries(test_vertex_light_equiv m)
add_test(NAME test_vertex_light_equiv COMMAND test_vertex_light_equiv)

add_executable(test_chunk_reachable_equiv
	tests/test_chunk_reachable_equiv.c
)
target_link_libraries(test_chunk_reachable_equiv m)
add_test(NAME test_chunk_reachable_equiv COMMAND test_chunk_reachable_equiv)

add_executable(test_displaylist_vertex_equiv
	tests/test_displaylist_vertex_equiv.c
	tests/support/displaylist_finalize_wii.c
	tests/support/displaylist_finalize_pc.c
	source/platform/displaylist_scene_hash.c
)
target_include_directories(test_displaylist_vertex_equiv PRIVATE tests/support/gx_finalize)
target_link_libraries(test_displaylist_vertex_equiv m GLEW::GLEW OpenGL::GL)
add_test(NAME test_displaylist_vertex_equiv COMMAND test_displaylist_vertex_equiv)

# M2 mesh blocks (docs/p2_storage_ruling_fable_2026-09-09.md, Memory plan v2): the
# real Wii emitter compiled on the host proves block mode emits byte-identical
# GX streams with one exact-size carve allocation per generation.
add_executable(test_displaylist_block_equiv tests/test_displaylist_block_equiv.c
	source/platform/displaylist_scene_hash.c)
target_include_directories(test_displaylist_block_equiv PRIVATE tests/support/gx_finalize ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_displaylist_block_equiv m)
add_test(NAME test_displaylist_block_equiv COMMAND test_displaylist_block_equiv)

# L205: the same lifetime implementation in both real backends, delayed GPU,
# poisoned allocator, and worker/main queue backpressure on the PC host.
add_executable(test_displaylist_retirement tests/test_displaylist_retirement.c
	source/platform/displaylist_scene_hash.c)
target_link_libraries(test_displaylist_retirement m ${CMAKE_THREAD_LIBS_INIT} GLEW::GLEW OpenGL::GL)
add_test(NAME test_displaylist_retirement COMMAND test_displaylist_retirement)
add_executable(test_displaylist_retirement_wii tests/test_displaylist_retirement.c
	source/platform/displaylist_scene_hash.c)
target_compile_definitions(test_displaylist_retirement_wii PRIVATE CAVEWII_RETIRE_WII_MODEL)
target_include_directories(test_displaylist_retirement_wii PRIVATE tests/support/gx_finalize)
target_link_libraries(test_displaylist_retirement_wii m ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_displaylist_retirement_wii COMMAND test_displaylist_retirement_wii)
set_tests_properties(test_displaylist_retirement test_displaylist_retirement_wii PROPERTIES TIMEOUT 30)

# lane/dl-ladder-pools (2026-09-08): ladder-rung display-list sizing. Proves the
# rung table IS the sequence the existing grow recurrence produces (so the grow
# path pays no extra bytes), and -- against a model of the libogc __lwp_heap the
# carve runs on -- that a rung-sized rebuild refits the block it freed exactly
# while the old byte-exact sizing shreds the same arena. Self-contained
# (header-only ladder), same convention as the equivalence tests above.
add_executable(test_displaylist_ladder
	tests/test_displaylist_ladder.c
)
target_link_libraries(test_displaylist_ladder m)
add_test(NAME test_displaylist_ladder COMMAND test_displaylist_ladder)

# lane/dl-ladder-pools (2026-09-08): fixed-tenant bitmap-slot pools. Accounting
# contract (take/release/exhaustion/overflow, double-free, interior pointers,
# census totals) plus the static pins that keep the two shipped slot counts tied
# to the far-ring / MAX_CHUNKS / COLUMN_VIEW_INDEX_BYTES derivations.
add_executable(test_mem2_pool
	tests/test_mem2_pool.c
)
target_include_directories(test_mem2_pool PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_mem2_pool PRIVATE PLATFORM_PC)
# colcap-1006 R3 item 4: the shrink concurrency case drives the pool from
# four threads under a mutex (the allocator-lock contract).
target_link_libraries(test_mem2_pool m pthread)
add_test(NAME test_mem2_pool COMMAND test_mem2_pool)

# colcap-1006 (ASTRA-CC-R5 item 2): the pool_selftest_3m driver (Wii column-
# pool T1 proof) on the PC column-pool emulation, with the real ordered scene
# hash accumulator. The Wii run itself is the hardware proof; this pins the
# driver logic (churn/refusal/ballast/scene state machine) before a slot is
# spent on it.
add_executable(test_pool_selftest
	tests/test_pool_selftest.c
	source/game/pool_selftest.c
	source/platform/displaylist_scene_hash.c
	source/platform/mem2.c
)
target_include_directories(test_pool_selftest PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_pool_selftest PRIVATE PLATFORM_PC)
target_link_libraries(test_pool_selftest m)
add_test(NAME test_pool_selftest COMMAND test_pool_selftest)
set_tests_properties(test_pool_selftest PROPERTIES TIMEOUT 120)

# lane/perf-t1-frame-drops (2026-09-03): T1-a triple-buffering capacity proof
# (docs/perf_frame_drops_rootcause_2026-09-03.md). Self-contained mirror of
# source/platform/wii/gfx.c's fb_ring/semaphore handoff, same convention as
# the equivalence tests above (gfx.c only compiles against gccore.h on real
# Wii hardware).
add_executable(test_fb_ring_equiv
	tests/test_fb_ring_equiv.c
)
target_link_libraries(test_fb_ring_equiv m)
add_test(NAME test_fb_ring_equiv COMMAND test_fb_ring_equiv)

# lane/graphics-platform (2026-09-02): render_block_cracks no longer swaps the
# GLOBAL blocks[type]->getTextureIndex vtable slot (a cross-thread race with
# the mesher, chunk_mesher.c) -- it carries the override on a stack-local
# block_info field instead. Self-contained mirror per the equivalence-test
# convention above; also covers block_cracks_texture's delay/10 divide-by-zero
# guard (same two functions).
add_executable(test_block_cracks_texture_override
	tests/test_block_cracks_texture_override.c
)
target_link_libraries(test_block_cracks_texture_override m)
add_test(NAME test_block_cracks_texture_override COMMAND test_block_cracks_texture_override)

# lane/graphics-platform (2026-09-02): world.c's texture-animation offset
# multiplied a time_diff_ms() (int32_t) result by 7 in 32-bit arithmetic,
# overflowing (UB) past ~85.2h of session/world time. Widened to int64_t;
# mirror cross-checks the widened formula against the original 32-bit one in
# the safe range and against an independent double-precision oracle past it.
add_executable(test_world_anim_timer_overflow
	tests/test_world_anim_timer_overflow.c
)
target_link_libraries(test_world_anim_timer_overflow m)
add_test(NAME test_world_anim_timer_overflow COMMAND test_world_anim_timer_overflow)

# The immediate-mode draw path must walk a list with the SAME stride/offsets it
# was written with. Pins the 2026-07-31 console crash: tinted lists were drawn as
# GX_VTXFMT0 at a 9-byte stride with no CLR1 emit while gfx_tint() had declared
# CLR1 -> GX FIFO desync (piston pushing grass/leaves).
add_executable(test_displaylist_immediate_layout
	tests/test_displaylist_immediate_layout.c
)
target_link_libraries(test_displaylist_immediate_layout m)
add_test(NAME test_displaylist_immediate_layout COMMAND test_displaylist_immediate_layout)

# R1 per-list biome-tint split: proves the untinted (9-byte) / tinted (12-byte)
# list routing preserves every face's geometry byte-for-byte (only the redundant
# white tint is dropped from untinted faces).
add_executable(test_mesh_tint_split
	tests/test_mesh_tint_split.c
)
target_link_libraries(test_mesh_tint_split m)
add_test(NAME test_mesh_tint_split COMMAND test_mesh_tint_split)

# Surface-LOD tree regression: keep real trunk/canopy hull faces, exclude logs
# from merged terrain skirts, preserve ordinary skirts + the untouched LOD0
# face path, and reject unknown far_mode strings to the safe terrain fallback.
add_executable(test_lod_tree_policy tests/test_lod_tree_policy.c)
target_link_libraries(test_lod_tree_policy m)
target_include_directories(test_lod_tree_policy PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
add_test(NAME test_lod_tree_policy COMMAND test_lod_tree_policy)

# T2 SPIKE (opt-in, PC-measured only -- docs/far_ring_geomip_design_2026-08-26.md):
# unit tests for chunk_lod_geomip.h's pure error-metric/projection/merge-decision
# math, standalone (no mesher link needed). Exhaustively proves the disabled-gate
# byte-path-equivalence contract the same way test_lod_tree_policy above proves
# chunk_lod_face_allowed's LOD0 gate.
add_executable(test_lod_geomip_policy tests/test_lod_geomip_policy.c)
target_link_libraries(test_lod_geomip_policy m)
target_include_directories(test_lod_geomip_policy PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
add_test(NAME test_lod_geomip_policy COMMAND test_lod_geomip_policy)

# Vendor-patch pin (bug-hunt D7): compile-time marker check that the local
# m-dict.h OOM patch survived any re-vendor of third_party/m-lib. See
# third_party/m-lib/PATCHES.md.
add_executable(test_vendor_patches tests/test_vendor_patches.c)
target_link_libraries(test_vendor_patches m)
target_include_directories(test_vendor_patches PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_vendor_patches COMMAND test_vendor_patches)

add_executable(test_mob_spawn_light
	tests/test_mob_spawn_light.c
)
target_link_libraries(test_mob_spawn_light m) # cosf: libm is implicit on MSYS, explicit on Linux/CI
add_test(NAME test_mob_spawn_light COMMAND test_mob_spawn_light)

# D3 daylight-curve faithfulness: proves gfx_update_light's lightmap mapping
# (integer sky-subtract + single lightBrightnessTable lookup) is byte-faithful to
# Beta 1.7.3, and that the production light tables match generateLightBrightnessTable.
add_executable(test_daylight_curve
	tests/test_daylight_curve.c
	source/light_brightness.c
	source/daytime.c
	source/worldgen/beta_math.c
	source/platform/time.c
	source/util.c
	source/game/game_state.c
)
target_compile_definitions(test_daylight_curve PRIVATE PLATFORM_PC)
target_include_directories(test_daylight_curve PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_daylight_curve m)
add_test(NAME test_daylight_curve COMMAND test_daylight_curve)

# parity-render2-0924/U1: Beta player brightness tints the underwater overlay.
add_executable(test_underwater_overlay_tint
	source/light_brightness.c tests/test_underwater_overlay_tint.c)
target_link_libraries(test_underwater_overlay_tint m)
add_test(NAME test_underwater_overlay_tint COMMAND test_underwater_overlay_tint
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}) # caller-wiring source guard
# next-gaps-0924/R1b: Beta RenderLiving hurt/death wash (brightness,0,0) at 0.4 over
# the model, brightness = the model's own lightmap entry (light_brightness_current).
add_executable(test_hurt_wash_parity
	source/light_brightness.c tests/test_hurt_wash_parity.c)
target_include_directories(test_hurt_wash_parity PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_hurt_wash_parity m)
add_test(NAME test_hurt_wash_parity COMMAND test_hurt_wash_parity)

add_executable(test_double_chest
	source/block/chest_link.c
	tests/test_double_chest.c
)
target_link_libraries(test_double_chest m)
target_include_directories(test_double_chest PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
add_test(NAME test_double_chest COMMAND test_double_chest)

# C1: chest window-id collision (data-loss) -- WINDOWC_CHEST must not alias the
# permanent player-inventory window (id 0).
add_executable(test_window_id_unique
	source/item/inventory_click.c
	source/item/recipe.c
	source/item/window_container.c
	source/item/inventory.c
	source/platform/time.c
	tests/test_window_id_unique.c
)
target_include_directories(test_window_id_unique PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_window_id_unique PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED
	CAVEWII_CLIENT_INTERFACE_TEST)
target_link_libraries(test_window_id_unique m)
add_test(NAME test_window_id_unique COMMAND test_window_id_unique)

# Container/window registry (window_registry.h), the single source of truth that
# replaced the six hand-maintained dispatch tables of CODESTYLE.md S3.3.
# Pure macro expansion over the same rows the engine reads -- links nothing.
add_executable(test_window_registry
	tests/test_window_registry.c
)
target_include_directories(test_window_registry PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_window_registry PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_window_registry m)
add_test(NAME test_window_registry COMMAND test_window_registry)

# The negative half: a new window type registered ONLY as a registry row must
# fail the BUILD, not ship and fail silently at runtime. EXCLUDE_FROM_ALL so an
# ordinary build never touches it; the test drives its own compile and WILL_FAIL
# turns a successful build into a test failure.
add_executable(test_window_registry_unregistered EXCLUDE_FROM_ALL
	tests/test_window_registry_unregistered.c
)
target_include_directories(test_window_registry_unregistered PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_window_registry_unregistered PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED)
add_test(NAME test_window_registry_unregistered
	COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
		--target test_window_registry_unregistered --config $<CONFIG>
)
set_tests_properties(test_window_registry_unregistered PROPERTIES WILL_FAIL TRUE)

# C2: chest item-dupe -- single chest place clears the WHOLE pooled row and drops
# only its own 27 slots (Beta getSizeInventory()==27).
add_executable(test_chest_slot_clear
	tests/test_chest_slot_clear.c
)
target_link_libraries(test_chest_slot_clear m)
target_include_directories(test_chest_slot_clear PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_chest_slot_clear PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
add_test(NAME test_chest_slot_clear COMMAND test_chest_slot_clear)

# Batch B player env-damage core: fall damage half-heart (B2), ladder gate (B3),
# full-health respawn (B5), strict void-kill threshold (B12), floored column (B1).
add_executable(test_player_fall
	tests/test_player_fall.c
)
target_include_directories(test_player_fall PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_player_fall PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_player_fall m)
add_test(NAME test_player_fall COMMAND test_player_fall)

# Batch B env-damage cadences: drowning 300/-20/20 (B7), fire burn + ignite ramp
# (B6), cactus contact (B11).
add_executable(test_player_env
	tests/test_player_env.c
)
target_include_directories(test_player_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_player_env PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_player_env m)
add_test(NAME test_player_env COMMAND test_player_env)

# C7: click-outside-to-drop -- the Beta whole/one drop-count math.
add_executable(test_inventory_drop
	source/item/inventory.c
	tests/test_inventory_drop.c
)
target_include_directories(test_inventory_drop PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_inventory_drop PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_inventory_drop m)
add_test(NAME test_inventory_drop COMMAND test_inventory_drop)

# An on_destroy disposal request must never free caller-owned/embedded storage.
add_executable(test_inventory_ownership
	source/item/inventory.c
	tests/test_inventory_ownership.c
)
target_include_directories(test_inventory_ownership PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_inventory_ownership PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_inventory_ownership m)
add_test(NAME test_inventory_ownership COMMAND test_inventory_ownership)

# C3: recipe_match() durability gate must be read PER-CELL (Beta ShapedRecipes).
add_executable(test_recipe_durability
	source/item/recipe.c
	tests/test_recipe_durability.c
)
target_include_directories(test_recipe_durability PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_recipe_durability PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_recipe_durability m)
add_test(NAME test_recipe_durability COMMAND test_recipe_durability)

# C4: right-click on a crafting OUTPUT slot == whole-stack pickup (Beta).
add_executable(test_crafting_output_click
	source/item/inventory_click.c
	source/item/inventory.c
	source/item/recipe.c
	source/network/inventory_logic.c
	source/network/inventory_achievements.c
	source/network/inventory_player.c
	source/network/inventory_crafting.c
	tests/test_crafting_output_click.c
)
target_include_directories(test_crafting_output_click PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_crafting_output_click PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_crafting_output_click m)
add_test(NAME test_crafting_output_click COMMAND test_crafting_output_click)

# C5: sign char-cycle constants (the IR-pointer poll makes the char UI reachable).
add_executable(test_sign_charcycle
	tests/test_sign_charcycle.c
	source/network/inventory_sign.c
)
target_include_directories(test_sign_charcycle PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_sign_charcycle PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_sign_charcycle m)
add_test(NAME test_sign_charcycle COMMAND test_sign_charcycle)

# Sign OSK: explicit set-char bounds + close-time durable copy.
add_executable(test_sign_set_env
	tests/test_sign_set_env.c
	source/game/gui/sign_osk_model.c
	source/network/inventory_sign.c
)
target_include_directories(test_sign_set_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_sign_set_env PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_sign_set_env m)
add_test(NAME test_sign_set_env COMMAND test_sign_set_env)

# C6: sign placement free-slot scan must be bounded by MAX_SIGNS (static asserts).
add_executable(test_sign_slot_bound
	tests/test_sign_slot_bound.c
)
target_link_libraries(test_sign_slot_bound m)
target_include_directories(test_sign_slot_bound PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_sign_slot_bound PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
add_test(NAME test_sign_slot_bound COMMAND test_sign_slot_bound)

# bug_hunt_2026-06-26: the SRPC_WINDOW_CLICK dispatch must reject an out-of-range
# UNTRUSTED client slot (inventory_slot_valid) before inventory_action, whose own
# bound check is an assert() -- a no-op under the shipping .dol's NDEBUG -> OOB.
add_executable(test_window_click_slot_bound
	tests/test_window_click_slot_bound.c
)
target_link_libraries(test_window_click_slot_bound m)
target_include_directories(test_window_click_slot_bound PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_window_click_slot_bound PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
add_test(NAME test_window_click_slot_bound COMMAND test_window_click_slot_bound)

# C8: Wii IR pointer calibration invariant (input_init after gfx_setup).
add_executable(test_ir_calibration_width tests/test_ir_calibration_width.c)
target_link_libraries(test_ir_calibration_width m)
target_include_directories(test_ir_calibration_width PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_compile_definitions(test_ir_calibration_width PRIVATE PLATFORM_PC)
add_test(NAME test_ir_calibration_width COMMAND test_ir_calibration_width)

# Microbenchmark (not a pass/fail test) — ref vs optimized mesher hot loops.
add_executable(bench_mesher
	tests/bench_mesher.c
)
target_link_libraries(bench_mesher m)

# Microbenchmark — per-vertex displaylist_vertex vs per-face displaylist_quad.
add_executable(bench_displaylist_emit
	tests/bench_displaylist_emit.c
)
target_link_libraries(bench_displaylist_emit m)

# Microbenchmark — R1 per-list tint split: mesh-byte + emit-time delta.
add_executable(bench_mesh_tint_split
	tests/bench_mesh_tint_split.c
)
target_link_libraries(bench_mesh_tint_split m)

add_executable(test_sign_archive
	source/network/complex_block_archive.c
	source/network/tile_restore_plan.c
	tests/test_sign_archive.c
)
target_link_libraries(test_sign_archive m)
target_include_directories(test_sign_archive PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_sign_archive PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
add_test(NAME test_sign_archive COMMAND test_sign_archive)

# level.dat crash-window recovery and save-schema compatibility. Keep this a
# lean archive target so persistence checks are not coupled to graphics/gameplay.
add_executable(test_level_archive_recovery
	source/network/level_archive.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_level_archive_recovery.c
	tests/support/level_archive_io_stub.c # G9 R19: region-layer seams, OFF

	source/item/item_nbt.c)
target_include_directories(test_level_archive_recovery PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_level_archive_recovery PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED)
set_target_properties(test_level_archive_recovery PROPERTIES C_STANDARD 99)
target_link_libraries(test_level_archive_recovery ZLIB::ZLIB m)
add_test(NAME test_level_archive_recovery COMMAND test_level_archive_recovery)

# IMP-PLAYER-DEFAULT: Beta health defaults for imported level.dat without Player/Health.
add_executable(test_level_archive_player_health
	source/network/level_archive.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_level_archive_player_health.c
	tests/support/level_archive_io_stub.c
	source/item/item_nbt.c)
target_include_directories(test_level_archive_player_health PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_level_archive_player_health PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED)
set_target_properties(test_level_archive_player_health PROPERTIES C_STANDARD 99)
target_link_libraries(test_level_archive_player_health ZLIB::ZLIB m)
add_test(NAME test_level_archive_player_health COMMAND test_level_archive_player_health)

# CR-02 (docs/audit_2026-08-20_code_review.md): a late-invalid entry in the
# archived Inventory NBT list must not clear/partially overwrite the live
# inventory. Lean archive+NBT build, same shape as test_level_archive_recovery
# above (stubs inventory_clear() to avoid pulling in the gameplay/graphics
# graph).
add_executable(test_sort_import_roundtrip tests/test_sort_import_roundtrip.c)
target_link_libraries(test_sort_import_roundtrip cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_sort_import_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
# Link-time file double affects this executable only; the production objects
# stay shared. It blocks all publication/rollback after a simulated power cut.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
  target_link_options(test_sort_import_roundtrip PRIVATE
    "-Wl,--wrap=fwrite" "-Wl,--wrap=fflush" "-Wl,--wrap=rename" "-Wl,--wrap=remove"
    "-Wl,--wrap=fclose" "-Wl,--wrap=fread" "-Wl,--wrap=ferror"
    "-Wl,--wrap=inflateInit2_" "-Wl,--wrap=malloc")
  if(WIN32)
    target_link_options(test_sort_import_roundtrip PRIVATE "-Wl,--wrap=_commit" "-Wl,--wrap=__imp__commit")
  else()
    target_link_options(test_sort_import_roundtrip PRIVATE "-Wl,--wrap=fsync")
  endif()
endif()
add_test(NAME test_sort_import_roundtrip COMMAND test_sort_import_roundtrip)

add_executable(test_inventory_load_transactional
	source/network/level_archive.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_inventory_load_transactional.c
	tests/support/level_archive_io_stub.c # G9 R19: region-layer seams, OFF

	source/item/item_nbt.c)
target_include_directories(test_inventory_load_transactional PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_inventory_load_transactional PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED)
set_target_properties(test_inventory_load_transactional PROPERTIES C_STANDARD 99)
target_link_libraries(test_inventory_load_transactional ZLIB::ZLIB m)
add_test(NAME test_inventory_load_transactional COMMAND test_inventory_load_transactional)

# CR-03 (same register): the sign recipe outputs ONE item 323, not three
# BLOCK_SIGN. Links only recipe.c (the test_piston_recipe convention).
add_executable(test_sign_recipe
	source/item/recipe.c
	tests/test_sign_recipe.c
)
target_link_libraries(test_sign_recipe m)
target_compile_definitions(test_sign_recipe PRIVATE PLATFORM_PC)
target_include_directories(test_sign_recipe PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_sign_recipe COMMAND test_sign_recipe)

# ASTRA-SEED-POLICY-0925: the test seed policy itself (util.h) -- fixed default,
# CW_TEST_SEED / CW_TEST_SEED_FORCE overrides, malformed/zero values abort (child
# processes). Standalone on purpose: it also proves the CMake sweep carries
# CAVEWII_RAND_SEED_TEST to a test that compiles util.c itself (#error if not).
add_executable(test_rand_seed_policy
	source/util.c
	tests/test_rand_seed_policy.c
)
target_compile_definitions(test_rand_seed_policy PRIVATE PLATFORM_PC)
target_include_directories(test_rand_seed_policy PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_rand_seed_policy m)
set_target_properties(test_rand_seed_policy PROPERTIES C_STANDARD 99)
add_test(NAME test_rand_seed_policy COMMAND test_rand_seed_policy)

# CR-07 (same register): file_read()'s short-read guard.
add_executable(test_file_read_short
	source/util.c
	tests/test_file_read_short.c
)
target_compile_definitions(test_file_read_short PRIVATE PLATFORM_PC)
target_include_directories(test_file_read_short PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_file_read_short m)
add_test(NAME test_file_read_short COMMAND test_file_read_short)

add_executable(test_archive_write_preserves_input
	source/network/complex_block_archive.c
	source/network/tile_restore_plan.c
	tests/test_archive_write_preserves_input.c
)
target_link_libraries(test_archive_write_preserves_input m)
target_include_directories(test_archive_write_preserves_input PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_archive_write_preserves_input PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED CAVEWII_COMPLEX_BLOCK_ARCHIVE_TEST
)
add_test(NAME test_archive_write_preserves_input
	COMMAND test_archive_write_preserves_input
)

add_executable(test_config_fallback
	source/config.c
	source/parson/parson.c
	tests/test_config_fallback.c
)
target_link_libraries(test_config_fallback m)
target_link_options(test_config_fallback PRIVATE
	-Wl,--wrap=json_serialize_to_file_pretty -Wl,--wrap=rename)
# REL-01a-tests: the fallback cases load the real shipped Wii defaults.
target_compile_definitions(test_config_fallback PRIVATE
	CW_TEST_CONFIG_WII_JSON="${CMAKE_CURRENT_SOURCE_DIR}/config_wii.json")
target_include_directories(test_config_fallback PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
add_test(NAME test_config_fallback COMMAND test_config_fallback)

add_executable(test_hw_telemetry
	source/game/hw_run.c
	source/platform/hw_telemetry.c
	# The sender's idle park (hw_tel_wake.h, 2026-09-08) is built on the
	# portable mutex/condvar wrappers, so the async-sender half of this test
	# now needs their PC implementation. thread.c is self-contained (it
	# defines its own tps callback; cf. test_svin_reliable, which also links
	# it alone) -- no new transitive dependency comes with it.
	source/platform/thread.c
	# The crash mailbox replays the SEQ-SUFFIXED hang reports (2026-07-29), so it now calls
	# wd_report_path_for_seq. On PLATFORM_PC watchdog.c is the no-op dispatcher plus the
	# shared report-assembly half, which is exactly where that pure helper lives.
	source/platform/watchdog.c
	# watchdog.c times the server phases (watchdog_srv_phase_take): it needs the clock.
	source/platform/time.c
	tests/test_hw_telemetry.c
)
target_link_libraries(test_hw_telemetry m)
# Count/reject heap requests across the real measurement lifecycle in the
# focused fixture, including calls from hw_run.c and the telemetry emitter.
target_link_options(test_hw_telemetry PRIVATE
	-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
target_compile_definitions(test_hw_telemetry PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED CAVEWII_HW_TELEMETRY_TEST)
target_include_directories(test_hw_telemetry PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
set_target_properties(test_hw_telemetry PROPERTIES C_STANDARD 99)
add_test(NAME test_hw_telemetry COMMAND test_hw_telemetry)

# hw1030n: the spawn-search wait keeps the telemetry heartbeat alive through a
# long Beta walk and shows the probe count (real spawn_search_wait.c, fake
# clock/platform in the test).
add_executable(test_spawn_search_wait
	source/game/gui/spawn_search_wait.c
	tests/test_spawn_search_wait.c
)
target_compile_definitions(test_spawn_search_wait PRIVATE PLATFORM_PC)
target_include_directories(test_spawn_search_wait PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_spawn_search_wait PROPERTIES C_STANDARD 99)
add_test(NAME test_spawn_search_wait COMMAND test_spawn_search_wait)

# Pure/headless (CODESTYLE 4.4 short form): menu_autopilot_script.c has zero
# gfx/game_state/GLFW/WPAD dependency by design (menu_autopilot_script.h), so
# this test links only that one module -- no PLATFORM_PC define, no cglm, no
# gfx stub needed.
add_executable(test_menu_autopilot_script
	source/game/menu_autopilot_script.c
	tests/test_menu_autopilot_script.c
)
set_target_properties(test_menu_autopilot_script PROPERTIES C_STANDARD 99)
add_test(NAME test_menu_autopilot_script COMMAND test_menu_autopilot_script)

# ui-build-1026/T16a: the world_copy_convert route (menu_ap_copy_*), pure like
# the walker above: one module plus a screen model. With a directory argument
# it also writes the fixture telemetry tests/fixtures/world_copy_convert/ uses.
add_executable(test_world_copy_profile
	source/game/menu_autopilot_script.c
	tests/test_world_copy_profile.c
)
set_target_properties(test_world_copy_profile PROPERTIES C_STANDARD 99)
add_test(NAME test_world_copy_profile COMMAND test_world_copy_profile)

# b1-chat-opener-0919/16 G-0b: the menus_dwell STEP CONTRACT, pinned from both
# sides. tools/wii_telemetry_server.py's min_map_step is an ordinal out of the
# script's own enum; it has silently drifted stale twice. Same pure/headless
# link as the script test above, plus the repo root as the working directory
# so it can read the collector as text.
add_executable(test_menus_dwell_contract
	source/game/menu_autopilot_script.c
	tests/test_menus_dwell_contract.c
)
set_target_properties(test_menus_dwell_contract PROPERTIES C_STANDARD 99)
add_test(NAME test_menus_dwell_contract COMMAND test_menus_dwell_contract)
set_tests_properties(test_menus_dwell_contract PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Pure/headless, same rationale as test_menu_autopilot_script above:
# hw_run.c only includes <stdio.h>/<string.h> and its own header, so this
# TOTAL-ness regression suite for hw_run_parse_file/hw_run_parse_args
# (2026-09-02, cw_run_args hardening lane -- see the file's own top comment
# for the weaknesses found and fixed) links only that one module.
add_executable(test_hw_run_args_parse
	source/game/hw_run.c
	tests/test_hw_run_args_parse.c
)
target_include_directories(test_hw_run_args_parse PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(test_hw_run_args_parse PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED)
set_target_properties(test_hw_run_args_parse PROPERTIES C_STANDARD 99)
add_test(NAME test_hw_run_args_parse COMMAND test_hw_run_args_parse)

add_executable(test_stage_sha256
	tools/wii_stage_server/source/sha256.c
	tests/test_stage_sha256.c
)
target_link_libraries(test_stage_sha256 m)
target_include_directories(test_stage_sha256 PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source
)
set_target_properties(test_stage_sha256 PROPERTIES C_STANDARD 99)
add_test(NAME test_stage_sha256 COMMAND test_stage_sha256)

add_executable(test_stage_manifest_paths
	tests/test_stage_manifest_paths.c
)
target_link_libraries(test_stage_manifest_paths m)
target_include_directories(test_stage_manifest_paths PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source
)
set_target_properties(test_stage_manifest_paths PROPERTIES C_STANDARD 99)
add_test(NAME test_stage_manifest_paths COMMAND test_stage_manifest_paths)

# Stage-server subnet + token auth hardening (2026-09, CWSTAGE4): pure/
# header-only helpers (stage_auth.h) shared verbatim with main.c's
# cw_stage_auth_gate() -- constant-time token compare, netmask-membership
# subnet check, and the file-content trim used when loading
# sd:/apps/CavEX/stage_token.txt.
add_executable(test_stage_auth
	tests/test_stage_auth.c
)
target_link_libraries(test_stage_auth m)
target_include_directories(test_stage_auth PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source
)
set_target_properties(test_stage_auth PROPERTIES C_STANDARD 99)
add_test(NAME test_stage_auth COMMAND test_stage_auth)

# hw1028b: CWFETCH1 takes the rest of the line as its path (stage_fetch_path.h)
# so "<world> (backup)" folders are fetchable; bounded, ".." refused.
add_executable(test_stage_fetch_path
	tests/test_stage_fetch_path.c
)
target_link_libraries(test_stage_fetch_path m)
target_include_directories(test_stage_fetch_path PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source
)
set_target_properties(test_stage_fetch_path PROPERTIES C_STANDARD 99)
add_test(NAME test_stage_fetch_path COMMAND test_stage_fetch_path)

# Stage-server saves LIST / HASH / MOVE verbs (push-0921/C8, 2026-09-21):
# pure/header-only decision surface (stage_saves.h) shared verbatim with
# main.c's CWSAVES1/CWARCHIVE1 handlers -- the name/batch/root validators,
# the lossless hex name transport, cw_saves_meta_path(), the auth truth
# table, and the LIST/HASH/MOVE drivers themselves, which the test runs
# against an in-memory fake card through the same cw_saves_fs_ops main.c
# fills with real FAT calls (the stage_send.h / stage_netinit.h precedent).
add_executable(test_stage_saves_validator
	tests/test_stage_saves_validator.c
)
target_link_libraries(test_stage_saves_validator m)
target_include_directories(test_stage_saves_validator PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source
)
set_target_properties(test_stage_saves_validator PROPERTIES C_STANDARD 99)
add_test(NAME test_stage_saves_validator COMMAND test_stage_saves_validator)


# Stage-server send path (2026-09-02, real-Wii CWFETCH1 body corruption):
# header-only policy (stage_send.h) shared verbatim with main.c's send_all(),
# driven here against a fake IOS socket that reproduces libogc/IOS's
# "queue what fits, still return -EAGAIN" partial send. Pins the regression
# witness (the old retry-from-the-same-pointer loop reproduces the field
# signature: duplication boundary at 4096 absolute payload bytes) and the
# fix's invariant (the wire is ALWAYS a prefix of the payload).
add_executable(test_stage_send
	tests/test_stage_send.c
)
target_link_libraries(test_stage_send m)
target_include_directories(test_stage_send PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source
)
set_target_properties(test_stage_send PROPERTIES C_STANDARD 99)
add_test(NAME test_stage_send COMMAND test_stage_send)

# Stage-server network bring-up (2026-09-02, "never reaches listen()"):
# header-only policy (stage_netinit.h) shared verbatim with main.c's
# configure_network(), driven here against a fake libogc that reproduces
# v3.1.0's two unbounded waits -- net_init()'s LWP_ThreadSleep on a queue
# net_init_async() never broadcasts, and the init chain's uncapped 100ms
# IPC_ENOENT alarm retry. Part 1 witnesses that the blocking shape provably
# never terminates; part 2 pins the bound and the abort-before-every-retry
# rule that keeps a wedged _init_busy from poisoning the next attempt.
add_executable(test_stage_netinit
	tests/test_stage_netinit.c
)
target_link_libraries(test_stage_netinit m)
target_include_directories(test_stage_netinit PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source
)
set_target_properties(test_stage_netinit PROPERTIES C_STANDARD 99)
add_test(NAME test_stage_netinit COMMAND test_stage_netinit)

add_executable(test_stage_runprep
	tools/wii_stage_server/source/dol_launch.c
	tools/wii_stage_server/source/dol_runprep.c
	tests/test_stage_runprep.c
)
target_link_libraries(test_stage_runprep m)
target_include_directories(test_stage_runprep PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source
)
set_target_properties(test_stage_runprep PROPERTIES C_STANDARD 99)
add_test(NAME test_stage_runprep COMMAND test_stage_runprep)

add_executable(verify_wii_launch_plan
	tools/wii_stage_server/source/dol_runprep.c
	tools/wii_stage_server/source/sha256.c
	tools/verify_wii_launch_plan.c
)
target_link_libraries(verify_wii_launch_plan m)
target_include_directories(verify_wii_launch_plan PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source
)
set_target_properties(verify_wii_launch_plan PROPERTIES C_STANDARD 99)

add_executable(test_texture_packs
	source/config.c
	source/parson/parson.c
	source/texturepacks.c
	source/util.c
	tests/test_texture_packs.c
)
# util.c pulls util.h -> platform/time.h, which needs a platform typedef
# (the test_file_read_short precedent).
target_compile_definitions(test_texture_packs PRIVATE PLATFORM_PC)
target_link_libraries(test_texture_packs m)
target_include_directories(test_texture_packs PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_texture_packs COMMAND test_texture_packs)

# Pack Helper audio/texture separation: actual config transactions plus the
# production effect/music registries (legacy fallback, explicit audio root,
# persistence/rollback, session-stable record root, no texture-triggered reload).
add_executable(test_pack_helper_audio tests/test_pack_helper_audio.c)
target_link_libraries(test_pack_helper_audio
	cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_pack_helper_audio PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_pack_helper_audio PROPERTIES C_STANDARD 99)
target_link_libraries(test_pack_helper_audio
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_pack_helper_audio COMMAND test_pack_helper_audio
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# User-provided texture PNGs are bounded before LodePNG allocates either its
# encoded-file/IDAT scratch copies or the decoded RGBA buffer.
add_executable(test_texture_decode_safety
	source/platform/texture_decode.c
	source/lodepng/lodepng.c
	tests/test_texture_decode_safety.c
)
target_link_libraries(test_texture_decode_safety m)
target_include_directories(test_texture_decode_safety PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
set_target_properties(test_texture_decode_safety PROPERTIES C_STANDARD 99)
add_test(NAME test_texture_decode_safety COMMAND test_texture_decode_safety)

# Raw-jar texture packs (tex_beta_compose.h): the load-time chest/gui_2/anim
# compositions must equal the Pack Helper's own output pixel for pixel. The
# Python driver generates a fixture jar layout (never real Mojang files), runs
# tools/pack_helper/conversion.py on it and this probe on the same folder.
add_executable(tex_beta_compose_probe
	tests/tex_beta_compose_probe.c
	source/platform/tex_beta_compose.c
	source/platform/texture_decode.c
	source/lodepng/lodepng.c
	source/texturepacks.c
	source/config.c
	source/parson/parson.c
	source/util.c
)
target_compile_definitions(tex_beta_compose_probe PRIVATE PLATFORM_PC)
target_compile_options(tex_beta_compose_probe PRIVATE -UNDEBUG)
target_link_libraries(tex_beta_compose_probe m)
target_include_directories(tex_beta_compose_probe PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(tex_beta_compose_probe PROPERTIES C_STANDARD 99)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
	add_test(NAME test_tex_beta_compose
		COMMAND ${Python3_EXECUTABLE} -B
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tex_beta_compose.py
			--probe $<TARGET_FILE:tex_beta_compose_probe>
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ingame-import-1030 (coder A): in-game "Import Beta 1.7.3". The C test covers
# the streaming zip reader (store/deflate/data descriptor, corrupt, limits),
# SHA-256 and the swap/power-cut recovery; the Python driver builds a fixture
# jar (seeded noise, never Mojang files) and requires the importer's pack to
# equal tools/pack_helper core.prepare's for it (bytes; gui_2/anim decoded).
# beta_import_thread_pc.c stands in for thread.c's five calls (pthreads).
set(_cw_beta_import_sources
	source/platform/beta_import.c
	source/platform/beta_import_jar.c
	source/platform/beta_import_sound.c
	source/platform/pack_cache.c
	source/platform/tex_beta_compose.c
	source/platform/texture_decode.c
	source/lodepng/lodepng.c
	source/util.c
	tests/beta_import_thread_pc.c
	tests/beta_import_vorbis_pc.c
)
add_executable(test_beta_import tests/test_beta_import.c ${_cw_beta_import_sources})
add_executable(beta_import_probe tests/beta_import_probe.c ${_cw_beta_import_sources})
foreach(_cw_t test_beta_import beta_import_probe)
	target_compile_definitions(${_cw_t} PRIVATE PLATFORM_PC)
	target_compile_options(${_cw_t} PRIVATE -UNDEBUG)
	target_include_directories(${_cw_t} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
	target_link_libraries(${_cw_t} ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB m)
	set_target_properties(${_cw_t} PROPERTIES C_STANDARD 99)
endforeach()
add_test(NAME test_beta_import COMMAND test_beta_import)
if(Python3_Interpreter_FOUND)
	add_test(NAME test_beta_import_e2e
		COMMAND ${Python3_EXECUTABLE} -B
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_beta_import.py
			--probe $<TARGET_FILE:beta_import_probe>
			--sound-tool $<TARGET_FILE:test_beta_import_sound>
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ingame-import-1030 follow-up: a pack folder with spaces ("My Pack") lists,
# selects, saves and resolves through the real texture resolver; the
# manifest display name (texturepacks_display_name) is read from it.
add_executable(test_pack_path_spaces
	tests/test_pack_path_spaces.c
	source/platform/texture.c
	source/platform/tex_beta_compose.c
	source/platform/texture_decode.c
	source/platform/texture_mip.c
	source/config.c
	source/parson/parson.c
	source/lodepng/lodepng.c
	source/util.c
	source/texturepacks.c
)
target_compile_definitions(test_pack_path_spaces PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_pack_path_spaces PRIVATE source third_party tests)
target_compile_options(test_pack_path_spaces PRIVATE -O2 -UNDEBUG -ffunction-sections -fdata-sections)
target_link_options(test_pack_path_spaces PRIVATE -Wl,--gc-sections)
target_link_libraries(test_pack_path_spaces m)
set_target_properties(test_pack_path_spaces PROPERTIES
	C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION TRUE)
add_test(NAME test_pack_path_spaces COMMAND test_pack_path_spaces)
set_tests_properties(test_pack_path_spaces PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Real file resolver; LTO removes unused renderer paths, so no GL context is
# needed to compare decoded controller pixels before GPU upload.
add_executable(test_shared_controller_textures
    tests/test_shared_controller_textures.c
    source/platform/texture.c
    source/platform/tex_beta_compose.c
    source/platform/texture_decode.c
    source/platform/texture_mip.c
    source/config.c
    source/parson/parson.c
    source/lodepng/lodepng.c
    source/util.c
)
target_compile_definitions(test_shared_controller_textures PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_shared_controller_textures PRIVATE source third_party tests)
target_compile_options(test_shared_controller_textures PRIVATE -O2 -ffunction-sections -fdata-sections)
target_link_options(test_shared_controller_textures PRIVATE -Wl,--gc-sections)
target_link_libraries(test_shared_controller_textures m)
set_target_properties(test_shared_controller_textures PROPERTIES
    C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION TRUE)
add_test(NAME test_shared_controller_textures COMMAND test_shared_controller_textures)
set_tests_properties(test_shared_controller_textures PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# BG-BAKE-1028 task 3: the generated menu-backdrop cache. The REAL resolver/
# publisher (texture.c, the test_shared_controller_textures link shape: LTO +
# gc-sections drop the renderer paths) over private pack folders: author art
# untouched, cold/warm, pack mutation, every interrupted publication boundary,
# open/write/close/rename/alloc/read-only failures, corrupt pairs, two packs.
add_executable(test_backdrop_cache
    tests/test_backdrop_cache.c
    source/platform/texture.c
    source/platform/tex_beta_compose.c
    source/platform/texture_decode.c
    source/platform/texture_mip.c
    source/config.c
    source/parson/parson.c
    source/lodepng/lodepng.c
    source/util.c
)
target_compile_definitions(test_backdrop_cache PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_backdrop_cache PRIVATE source third_party tests)
target_compile_options(test_backdrop_cache PRIVATE -O2 -ffunction-sections -fdata-sections)
target_link_options(test_backdrop_cache PRIVATE -Wl,--gc-sections)
target_link_libraries(test_backdrop_cache m)
set_target_properties(test_backdrop_cache PROPERTIES
    C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION TRUE)
add_test(NAME test_backdrop_cache COMMAND test_backdrop_cache)
set_tests_properties(test_backdrop_cache PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# GLYPH/1 (Astra pack-0926 #1): controller glyphs never come from a pack.
# The REAL resolver (texture.c, the test_shared_controller_textures link shape:
# LTO + gc-sections drop the renderer paths) plus the REAL prompt/notice
# painters (gui_util.c) over a headless quad observer.
add_executable(test_controller_glyph_pack_proof
    tests/test_controller_glyph_pack_proof.c
    source/platform/texture.c
    source/platform/tex_beta_compose.c
    source/platform/texture_decode.c
    source/platform/texture_mip.c
    source/platform/input_inventory.c
    source/platform/input_glyph.c
    source/platform/time.c
    source/graphics/gui_util.c
    source/config.c
    source/parson/parson.c
    source/lodepng/lodepng.c
    source/util.c
)
target_compile_definitions(test_controller_glyph_pack_proof PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_controller_glyph_pack_proof PRIVATE source third_party tests)
target_compile_options(test_controller_glyph_pack_proof PRIVATE -O2 -ffunction-sections -fdata-sections)
target_link_options(test_controller_glyph_pack_proof PRIVATE -Wl,--gc-sections)
target_link_libraries(test_controller_glyph_pack_proof m)
set_target_properties(test_controller_glyph_pack_proof PROPERTIES
    C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION TRUE)
add_test(NAME test_controller_glyph_pack_proof COMMAND test_controller_glyph_pack_proof)
set_tests_properties(test_controller_glyph_pack_proof PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_texture_mip
	source/platform/texture_mip.c
	tests/test_texture_mip.c
)
target_link_libraries(test_texture_mip m)
target_include_directories(test_texture_mip PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
set_target_properties(test_texture_mip PROPERTIES C_STANDARD 99)
add_test(NAME test_texture_mip COMMAND test_texture_mip)

add_executable(test_texture_atlas_failure
	source/graphics/texture_atlas.c
	source/platform/texture_mip.c
	tests/test_texture_atlas_failure.c
)
target_link_libraries(test_texture_atlas_failure m)
target_include_directories(test_texture_atlas_failure PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_texture_atlas_failure PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED)
set_target_properties(test_texture_atlas_failure PROPERTIES C_STANDARD 99)
add_test(NAME test_texture_atlas_failure COMMAND test_texture_atlas_failure)

# Phase 2 hostile-mob mechanics: per-mob stat table + weighted spawn picker.
# source/util.c: rand_gen_int/struct random_gen, needed by the skeleton dual-drop-
# roll regression test (test_skeleton_dual_drop_roll) -- util.c pulls util.h ->
# platform/time.h, which needs PLATFORM_PC (the test_texture_packs precedent).
add_executable(test_mob_types
	source/entity/monsters_object.c
	source/util.c
	tests/test_mob_types.c
)
target_include_directories(test_mob_types PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_compile_definitions(test_mob_types PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
# libm explicitly: MSYS links it implicitly, Linux does not -- wolf_shake_angle's
# sinf broke the local_ci WSL job while MSYS ctest stayed green (2026-08-14).
target_link_libraries(test_mob_types m)
add_test(NAME test_mob_types COMMAND test_mob_types)

# Beta 1.7.3 mob->player melee: pins damage / reach / vertical overlap /
# knockback direction and the i-frame + swing-cooldown state machine, binding the
# shared source/entity/combat.h so the values cannot drift from the engine.
add_executable(test_zombie_melee
	source/entity/monsters_object.c
	tests/test_zombie_melee.c
)
target_include_directories(test_zombie_melee PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_zombie_melee m)
add_test(NAME test_zombie_melee COMMAND test_zombie_melee)

# Beta 1.7.3 mob water movement (M2 mass-drowning fix): pins the swim-roll
# chance, water gravity/drag ratios and the impulse->gravity->drag order
# (shared source/entity/combat.h, drift-proof).
add_executable(test_mob_water
	tests/test_mob_water.c
)
target_include_directories(test_mob_water PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_mob_water m)
add_test(NAME test_mob_water COMMAND test_mob_water)

# Beta 1.7.3 slime (M2 final units): size-as-id mapping, hop schedule
# (nextInt(20)+10, /3 aggro), contact damage (size>1 only), the 4-child
# +-size/8 split grid, and the hop-impulse engine-units band. Shared
# monsters_object.h + combat.h, drift-proof; needs the def table for
# mob_melee_damage.
add_executable(test_slime
	source/entity/monsters_object.c
	source/worldgen/java_random.c
	tests/test_slime.c
)
target_include_directories(test_slime PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_compile_definitions(test_slime PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_slime m)
add_test(NAME test_slime COMMAND test_slime)

# M2 head-placement fix: binds the production body/head matrix composition
# (render_entity_anchor.h) -- anchor property on all 7 mobs, byte-identity
# for the on-axis biped/creeper vs the old composition, and the old pig
# 12px-at-south error pinned as the historical reference.
add_executable(test_head_anchor
	tests/test_head_anchor.c
)
target_include_directories(test_head_anchor PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_compile_definitions(test_head_anchor PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_head_anchor m)
add_test(NAME test_head_anchor COMMAND test_head_anchor)

# next-gaps-0924/G8: Beta death tilt (RenderLiving.rotateCorpse sqrt curve, spider 180,
# squid none) through the shared production composition (render_entity_anchor.h + combat.h).
add_executable(test_death_tilt
	source/entity/monsters_object.c
	tests/test_death_tilt.c
)
target_include_directories(test_death_tilt PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_compile_definitions(test_death_tilt PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_death_tilt m)
add_test(NAME test_death_tilt COMMAND test_death_tilt)

# Beta 1.7.3 squid (M2 final units): swim-vector math, the nextInt(50)/
# !inWater/vec-zero re-pick gate, the 75%-stroke propulsion pulse, and the
# beached (v-0.08)*0.98 fall with its -3.92 terminal. Pure combat.h helpers.
add_executable(test_squid
	tests/test_squid.c
)
target_include_directories(test_squid PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_squid m)
add_test(NAME test_squid COMMAND test_squid)

# Axis-aligned block picking vs aabb_intersection_ray's inf slab math (bug hunt
# B1): interior axis-aligned rays through real block AABBs (unit cube + slab)
# must hit and report the true entry face. Gate for the B1 fix shape: this
# passed BEFORE the defensive else+assert closure, ruling the epsilon guard
# out; the grazing-ray analysis lives in the test header.
add_executable(test_block_pick_axis
	tests/test_block_pick_axis.c
	source/block/aabb.c
)
target_include_directories(test_block_pick_axis PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
set_target_properties(test_block_pick_axis PROPERTIES C_STANDARD 99)
target_link_libraries(test_block_pick_axis m)
add_test(NAME test_block_pick_axis COMMAND test_block_pick_axis)

# Beta 1.7.3 skeleton arrow inaccuracy (M2): pins deviation 12 / sigma 0.09 and
# the normalize->spread->speed order (shared source/entity/combat.h).
add_executable(test_arrow_spread
	tests/test_arrow_spread.c
)
target_include_directories(test_arrow_spread PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_arrow_spread m)
add_test(NAME test_arrow_spread COMMAND test_arrow_spread)

# Beta 1.7.3 mob chase steering: pins the 16-block follow range + the
# direct-approach unit heading (shared source/entity/combat.h, drift-proof).
add_executable(test_mob_chase
	tests/test_mob_chase.c
)
target_include_directories(test_mob_chase PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_mob_chase m)
add_test(NAME test_mob_chase COMMAND test_mob_chase)

# Wii-controller PLAYABILITY tunables (2nd carve-out from byte-exact parity): pins
# the mob speed cap + the attack wind-up telegraph and the wind-up state machine,
# so a documented, Dolphin-tuned playability value can't silently revert.
add_executable(test_mob_playability
	tests/test_mob_playability.c
)
target_include_directories(test_mob_playability PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_mob_playability m)
add_test(NAME test_mob_playability COMMAND test_mob_playability)

# Beta 1.7.3 undead daylight sun-burn (zombie + skeleton): pins the fire constants
# + the day/night + sky-exposure ignition gate + the fire DoT cadence (shared
# source/entity/combat.h, drift-proof).
add_executable(test_mob_sunburn
	tests/test_mob_sunburn.c
)
target_include_directories(test_mob_sunburn PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_mob_sunburn m)
add_test(NAME test_mob_sunburn COMMAND test_mob_sunburn)

# Beta 1.7.3 spider daytime passivity: pins the light-<=10 acquisition gate and
# that it is distinct from the spawn gate (<=7).
add_executable(test_spider_daypassive
	tests/test_spider_daypassive.c
)
target_include_directories(test_spider_daypassive PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_spider_daypassive m)
add_test(NAME test_spider_daypassive COMMAND test_spider_daypassive)

# Beta 1.7.3 creeper proximity fuse: pins the fuse constants, the 3-block-ignite /
# 7-block-release hysteresis, the swell->detonate cadence, and deflate-and-survive.
add_executable(test_creeper_fuse
	tests/test_creeper_fuse.c
)
target_include_directories(test_creeper_fuse PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_creeper_fuse m)
add_test(NAME test_creeper_fuse COMMAND test_creeper_fuse)

# PARITY FIX (audit register 2026-08-25, entities-mobs #1): a charged creeper's
# doubled (6.0F) blast power must survive BOTH the direct and the cap-deferred/
# retried detonation path -- entity_monster_retry_pending_explosion previously
# hardcoded the unpowered 3.0F, never reading e->data.monster.charged.
add_executable(test_creeper_charged_explosion tests/test_creeper_charged_explosion.c)
target_link_libraries(test_creeper_charged_explosion cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_creeper_charged_explosion PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_creeper_charged_explosion PROPERTIES C_STANDARD 99)
target_link_libraries(test_creeper_charged_explosion ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_creeper_charged_explosion COMMAND test_creeper_charged_explosion)

# Cross-slice harden audit: pins the O(1) monotonic entity-id contract (unique / > 0 /
# strictly monotonic / never reused after free) that replaced entity_gen_id's O(N)
# max-key scan at the server spawn sites, and the shared mob_is_targetable predicate
# (type==MONSTER && health>0 && death_time==0) that keeps the arrow sweep + bow
# aim-assist from locking onto a death-flop corpse. Pure logic, no engine link.
add_executable(test_entity_id_targetable
	tests/test_entity_id_targetable.c
)
target_include_directories(test_entity_id_targetable PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_entity_id_targetable m)
add_test(NAME test_entity_id_targetable COMMAND test_entity_id_targetable)

# audit #18 mob-polish grab-bag: pins the PURE helpers for the creeper swell^4 inflation,
# the deathTime flop curve, the rand.nextInt(3) drop count, the pack-spawn 6-draw scatter
# order, and the getEntityBrightness 0.66-body light sample -- plus guards that the
# (faithful) skeleton arrow 0.6 speed / flat-4 damage and the relabelled ARROW_KNOCKBACK
# stay put. All logic lives in combat.h; the GX/render + RNG wiring isn't unit-testable.
add_executable(test_mob_polish
	source/entity/monsters_object.c
	tests/test_mob_polish.c
)
target_include_directories(test_mob_polish PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_mob_polish m)
add_test(NAME test_mob_polish COMMAND test_mob_polish)

# audit #17f emissive-tint overlay: pins the PURE tint DECISION logic (creeper white
# swell pulse, red hurt-flash predicate, [0,1] amount clamp) that drives the FIFO-safe
# gfx_entity_tint on both backends. The GX/GL calls aren't unit-testable; the decision
# logic lives in combat.h and is the subject here.
add_executable(test_emissive_tint
	tests/test_emissive_tint.c
)
target_include_directories(test_emissive_tint PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_emissive_tint m)
add_test(NAME test_emissive_tint COMMAND test_emissive_tint)

# Beta 1.7.3 persistent target latch + retaliation (audit #14): pins the retention/
# leash constants and the pure mob_should_hold_target() decision -- fresh acquire arms
# the latch, LOS flicker keeps the chase, expiry drops to wander, an out-of-leash player
# frees the mob, and a player hit (retaliation) arms it regardless of light/LOS.
add_executable(test_mob_target_latch
	tests/test_mob_target_latch.c
)
target_include_directories(test_mob_target_latch PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_mob_target_latch m)
add_test(NAME test_mob_target_latch COMMAND test_mob_target_latch)

# Beta 1.7.3 mob line-of-sight core (audit #13): pins the pure canEntityBeSeen voxel
# DDA (source/entity/los.h) -- clear line visible, solid wall blocks, start/end cells
# excluded, thin diagonal wall not leaked, point-blank degenerate handled -- AND the
# per-block LOS-occlusion rule (los_block_occludes: slab/stair/farmland block,
# fluids/decorations pass, glass/leaves documented-deviation pass).
add_executable(test_mob_los
	tests/test_mob_los.c
)
target_include_directories(test_mob_los PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_mob_los m)
add_test(NAME test_mob_los COMMAND test_mob_los)

# FH-13: Beta 1.7.3 A* pathfinding core (source/entity/pathfinding.{c,h}) --
# PathPoint.func_22329_a hash values (incl. negative coords), the Path binary
# min-heap (hand-traced push/pop/change-distance incl. a tie), and
# Pathfinder.createEntityPathTo end to end on tiny synthetic worlds (straight
# corridor, L-bend forced around a wall, a 1-block gap routed around via the
# getSafePoint 4-fall cap, a 2-high ceiling constraint, and a sealed/unreachable
# pocket). Pure/world-free, mirrors test_mob_los's style.
add_executable(test_pathfinding_golden
	tests/test_pathfinding_golden.c
	source/entity/pathfinding.c
)
target_include_directories(test_pathfinding_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_pathfinding_golden m)
add_test(NAME test_pathfinding_golden COMMAND test_pathfinding_golden)

# Beta 1.7.3 skeleton ranged attack + arrow: pins the fire range/cooldown + arrow
# gravity/drag/damage and the flight model (shared source/entity/combat.h).
add_executable(test_skeleton_arrow
	tests/test_skeleton_arrow.c
)
target_include_directories(test_skeleton_arrow PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_skeleton_arrow m)
add_test(NAME test_skeleton_arrow COMMAND test_skeleton_arrow)

# Skeleton keep-distance + circle-strafe (audit #17e): pins the standoff band
# constants and the pure skeleton_steer() decision -- too-close backs AWAY (documented
# Wii-playability), in-band is pure-tangent orbit (faithful b1.7.3 strafe), beyond-band
# closes TOWARD, out of follow range wanders, always unit + no-NaN on the degenerate.
add_executable(test_skeleton_strafe
	tests/test_skeleton_strafe.c
)
target_include_directories(test_skeleton_strafe PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_skeleton_strafe m)
add_test(NAME test_skeleton_strafe COMMAND test_skeleton_strafe)

# Beta 1.7.3 PLAYER bow + arrow sweep/stick/pickup/despawn (audit #17c/#17d): pins the
# INSTANT-FIRE bow (no 1.8 charge curve), the fixed launch speeds (bow 1.5 vs skel 0.6),
# the per-scale on-hit damage (player 32 / mob 4), the owner-grace sweep exclusion, the
# pickable rule (player-fired only), and the in-air/in-ground despawn split. Binds the
# shared source/entity/combat.h pure helpers.
add_executable(test_bow_arrow
	tests/test_bow_arrow.c
)
target_include_directories(test_bow_arrow PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_bow_arrow m)
add_test(NAME test_bow_arrow COMMAND test_bow_arrow)

# Beta 1.7.3 natural mob despawn (audit #16): pins the distance/age/roll constants and
# the pure mob_despawn_action() decision -- >128 despawns instantly (age-independent),
# age>600 + 1/800 roll -> <32 resets the clock / 32..128 despawns, a roll miss keeps,
# and the per-tick lifecycle (mid-band despawns at age 601, near never despawns).
add_executable(test_mob_despawn
	tests/test_mob_despawn.c
)
target_include_directories(test_mob_despawn PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_mob_despawn m)
add_test(NAME test_mob_despawn COMMAND test_mob_despawn)

# Beta 1.7.3 PLAYER melee: pins the per-tool damage table (fist 1, sword 5-8, tool
# 2-5, hoe 1) on the mob 20-pt scale + the mob i-frame / knockback gate constants.
add_executable(test_player_melee
	tests/test_player_melee.c
)
target_include_directories(test_player_melee PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
# mob motion units S2: the knockBack composition is mob_motion_knockback (engine pools)
target_link_libraries(test_player_melee cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_player_melee COMMAND test_player_melee)

# Beta 1.7.3 living-entity ENVIRONMENTAL HAZARDS (audit #15): pins the fall/drown/lava
# constants + the pure mob_fall_damage() helper on the mob 20-pt scale -- fall ceil(d-3)
# (3->0, 4->1, 6->3, 23->20 lethal), drown cadence (first hit @320 ticks then 2/20t),
# and lava 4 + fire-field=600 (distinct from the sun-burn 300) feeding the existing DoT.
add_executable(test_mob_hazards
	tests/test_mob_hazards.c
)
target_include_directories(test_mob_hazards PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_mob_hazards m)
add_test(NAME test_mob_hazards COMMAND test_mob_hazards)

# Beta 1.7.3 spider combat: pins the leap gate/impulse, the wall-climb ascend, and
# the per-mob melee damage (spider 16 vs zombie 40), binding the shared combat.h.
add_executable(test_spider_combat
	tests/test_spider_combat.c
)
target_include_directories(test_spider_combat PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_spider_combat m)
add_test(NAME test_spider_combat COMMAND test_spider_combat)

# Beta 1.7.3 spider COLLISION/INTERACTION box (1.4x0.9, audit #17a) + the ladder
# descent clamp (motionY >= -0.15, audit #17b): pins the dims + the clamp helper.
add_executable(test_spider_box
	source/entity/monsters_object.c
	tests/test_spider_box.c
)
target_include_directories(test_spider_box PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_spider_box m)
add_test(NAME test_spider_box COMMAND test_spider_box)

# Beta 1.7.3 world generation: byte-exact java.util.Random PRNG (gen foundation).
add_executable(test_java_random
	source/worldgen/java_random.c
	tests/test_java_random.c
)
target_link_libraries(test_java_random m)
target_include_directories(test_java_random PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
add_test(NAME test_java_random COMMAND test_java_random)

# Beta 1.7.3 Perlin/octave noise (built on the java.util.Random PRNG).
add_executable(test_noise
	source/worldgen/java_random.c
	source/worldgen/noise.c
	tests/test_noise.c
)
target_include_directories(test_noise PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_noise m)
add_test(NAME test_noise COMMAND test_noise)

# Beta 1.7.3 Simplex noise (biome temperature/humidity/weirdness fields).
add_executable(test_simplex
	source/worldgen/java_random.c
	source/worldgen/simplex.c
	tests/test_simplex.c
)
target_include_directories(test_simplex PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_simplex m)
add_test(NAME test_simplex COMMAND test_simplex)

# Beta 1.7.3 overworld terrain generator (density + block fill + surface).
add_executable(test_worldgen
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/worldgen.c
				source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	tests/test_worldgen.c
)
target_include_directories(test_worldgen PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
# worldgen.c includes blocks.h (for the block table consulted by the sky-light
# pass), which transitively needs a platform macro to resolve ptime_t. The
# terrain code calls no platform functions, so PLATFORM_PC only satisfies the
# headers; nothing platform-specific is linked.
target_compile_definitions(test_worldgen PRIVATE PLATFORM_PC)
target_link_libraries(test_worldgen m)
add_test(NAME test_worldgen COMMAND test_worldgen)

# Beta 1.7.3 Nether (Hell) generator: ChunkProviderHell terrain/surface +
# MapGenCavesHell + populate. Terrain is deterministic + self-golden; populate
# is byte-exact given an explicit RNG (non-deterministic per-chunk in Beta).
add_executable(test_worldgen_hell
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/worldgen.c
	source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	tests/test_worldgen_hell.c
)
target_include_directories(test_worldgen_hell PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_worldgen_hell PRIVATE PLATFORM_PC)
target_link_libraries(test_worldgen_hell m)
add_test(NAME test_worldgen_hell COMMAND test_worldgen_hell)

# BOOT-CREDITS-1028: the boot credit cards (startup_credits.c): shipped
# atlas/manifest vs the approved stills, first/later-boot timing, A handling,
# footer, data-driven frames, load failures, the atomic seen-marker.
add_executable(test_startup_credits
	source/game/gui/startup_credits.c
	source/lodepng/lodepng.c
	tests/test_startup_credits.c
)
target_include_directories(test_startup_credits PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_compile_definitions(test_startup_credits PRIVATE PLATFORM_PC)
target_link_libraries(test_startup_credits m)
add_test(NAME test_startup_credits COMMAND test_startup_credits)
set_tests_properties(test_startup_credits PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# BG-BAKE-1028 task 2: the storage-free menu-backdrop scene bake. Real worldgen
# terrain + Beta populate over bake-owned columns against a fake renderer host:
# lifecycle, allocation failure, cancellation, scene-readiness gate and
# same-input byte-identical 1024x576 pixels (find_lake's worldgen deps).
add_executable(test_backdrop_bake
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	source/worldgen/worldgen.c
	source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	source/platform/time.c
	source/game/gui/backdrop_bake.c
	tests/test_backdrop_bake.c
)
target_include_directories(test_backdrop_bake PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_backdrop_bake PRIVATE PLATFORM_PC)
target_link_libraries(test_backdrop_bake m)
add_test(NAME test_backdrop_bake COMMAND test_backdrop_bake)

# BOOT-CREDITS-1028 T5: fast first-boot settle == legacy settle (real world light).
add_executable(test_backdrop_settle_equiv tests/test_backdrop_settle_equiv.c)
target_link_libraries(test_backdrop_settle_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
    ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_backdrop_settle_equiv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_backdrop_settle_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_backdrop_settle_equiv COMMAND test_backdrop_settle_equiv)

# P0 crash guard: the ~80KB/chunk server chunk arrays must allocate from MEM2 (mem2_alloc),
# not the ~6MB MEM1 newlib heap they overflowed -> OOM -> crash. Proves the relocation, that
# the resident set exceeds 6MB, and that there is no alloc/free leak. Same worldgen deps as
# test_worldgen + mem2.c (the alloc/free + the stat counters the guard reads).
add_executable(test_mem1_budget
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/worldgen.c
				source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	tests/test_mem1_budget.c
)
target_compile_definitions(test_mem1_budget PRIVATE PLATFORM_PC CAVEWII_WORLDGEN_TEST)
target_include_directories(test_mem1_budget PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_mem1_budget m)
add_test(NAME test_mem1_budget COMMAND test_mem1_budget)

# oracle-diff: differential worldgen parity gate. dump_chunk emits a chunk in the
# canonical KEY<TAB>VALUE text format (see .claude/skills/oracle-diff/); the
# test_oracle_worldgen CTest regenerates it and diffs against a committed golden
# vector with tests/oracle/oracle_diff.py (exit code = the gate). The golden is a
# capture from the mc173 Rust Beta-1.7.3 server (an EXTERNAL oracle): CaveWii's
# gen_terrain was verified byte-exact vs mc173 across 8 seed/chunk scenarios incl.
# negative coords (see tests/oracle/README.md). Same worldgen deps as test_worldgen;
# intentionally NOT named test_* (capture tool, not an assert test).
add_executable(dump_chunk
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/worldgen.c
				source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	tests/oracle/dump_chunk.c
)
target_compile_definitions(dump_chunk PRIVATE PLATFORM_PC)
# Capture tools are not test_ targets, so the suite's forced -UNDEBUG never
# reaches them -- but their goldens feed the oracle gates, so a Release-
# configured build dir must not strip their (or their deps') assert guards
# into a garbage capture (the dump_redstone Release incident, 2026-08-26).
target_compile_options(dump_chunk PRIVATE -UNDEBUG)
target_include_directories(dump_chunk PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(dump_chunk m)

# oracle-diff LIGHTING capture tool (tests/oracle/dump_light.c): builds the
# synthetic "arena1" scenario and emits the settled sky/block light field in
# the canonical x,y,z<TAB>sky,block format. Diffed by test_oracle_lighting
# against a committed mc173 capture (an EXTERNAL Beta-1.7.3 light engine).
# Pure-unit deps like test_lighting_multi: lighting.c + stack.c + a stub
# blocks[] table. Capture tool, deliberately NOT named test_*.
add_executable(dump_light
	source/lighting.c
	source/stack.c
	tests/oracle/dump_light.c
)
target_compile_definitions(dump_light PRIVATE PLATFORM_PC)
target_compile_options(dump_light PRIVATE -UNDEBUG) # capture-tool rule (see dump_chunk)
target_include_directories(dump_light PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(dump_light m)

# oracle-diff REDSTONE capture tool (tests/oracle/dump_redstone.c): the proven
# repeater-circuit env fixture (torch -> 8-wire run -> delay-4 repeater ->
# output wire) driven per-tick through the REAL server_world_tick, emitting
# x,y,z@tick<TAB>power. Diffed by test_oracle_redstone against a committed
# mc173 capture. Full-engine deps (env-test pattern). NOT named test_*.
add_executable(dump_redstone tests/oracle/dump_redstone.c)
target_link_libraries(dump_redstone cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_compile_options(dump_redstone PRIVATE -UNDEBUG) # capture-tool rule (see dump_chunk)
target_include_directories(dump_redstone PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(dump_redstone PROPERTIES C_STANDARD 99)
target_link_libraries(dump_redstone ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)

option(CAVEWII_REQUIRE_TEST_TOOLS
	"Fail configure when Python, PowerShell, or bash tooling tests cannot be registered" OFF)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
	add_test(NAME test_mob_windup_render COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mob_windup_render.py
		--cc ${CMAKE_C_COMPILER})
	# Beta RenderArrow mesh/UV/light/state and optional texture fallback.
	add_test(NAME test_arrow_render COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_arrow_render.py
		--cc ${CMAKE_C_COMPILER})
	# Focused production thrown-item billboard geometry/UV/light regression.
	add_test(NAME test_thrown_billboard COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_thrown_billboard.py
		--cc ${CMAKE_C_COMPILER})
	add_test(NAME test_pack_helper_audio_source
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pack_helper_audio.py)
	# c1-pack-helper-fix-0917/5 (Astra gate 3 condition 5): the rest of the Pack
	# Helper Python suite was written but never registered, so a regression in
	# tools/pack_helper/*.py could not fail `ctest`. One ctest test per MODULE
	# (unittest's own runner reports every failing case inside it, and a single
	# module is the smallest unit whose name is stable). test_pack_helper_audio.py
	# is deliberately NOT in this list -- it is a plain script, not a unittest
	# module, and is already registered above as test_pack_helper_audio_source.
	#
	# Invocation: `-m unittest tests.<module>` from the SOURCE ROOT, not the
	# script path used by the tooling tests above. These modules import the code
	# under test as `tools.pack_helper.<x>` / `tools.import_owned_beta_assets`,
	# and running a script puts tests/ (not the repo root) on sys.path, so
	# `python tests/test_pack_helper_core.py` dies with
	# "ModuleNotFoundError: No module named 'tools'". `-m` puts the working
	# directory on sys.path instead, hence the WORKING_DIRECTORY below.
	#
	# No extra tool gating: these modules need Pillow (a real dependency of the
	# helper itself) and, for the GUI one, tkinter -- they do NOT need
	# PyInstaller, a frozen bundle or the 3.12 build venv. The packaging
	# contract tests drive package_app/build_resources against fixtures with the
	# PyInstaller call mocked, and GuiTests.setUp already self-skips with
	# skipTest() when tk.Tk() finds no display.
	foreach(_cw_pack_helper_module core conversion desktop gui)
		add_test(NAME test_pack_helper_${_cw_pack_helper_module}
			COMMAND ${Python3_EXECUTABLE}
				-m unittest tests.test_pack_helper_${_cw_pack_helper_module}
			WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
	endforeach()
else()
	message(STATUS "Python3 not found -- skipping test_pack_helper_audio_source")
	message(STATUS "Python3 not found -- skipping the Pack Helper Python suite (test_pack_helper_core/conversion/desktop/gui)")
endif()
find_program(POWERSHELL_EXECUTABLE NAMES powershell pwsh)

# BASH_EXECUTABLE discovery. A bare `find_program(... NAMES bash)` on a stock
# Windows machine resolves to C:/Windows/System32/bash.exe -- the WSL launcher
# shim (or, on some machines, an equally useless App-Execution-Alias stub
# under WindowsApps) -- which cannot run a Windows-path script: it hands the
# path to a *WSL* /bin/bash, which sees "C:/.../guard-ip.sh" as a literal,
# nonexistent POSIX path and fails with "No such file or directory" (this bit
# CMake invoked from PowerShell, which is what `tools/cwbuild.ps1`'s
# `cmake --preset verify` does -- reproduced end-to-end). Search the real
# Git-for-Windows / MSYS2 bash locations FIRST with NO_DEFAULT_PATH so they
# win over the shim, fall back to a bare PATH search only if that search
# comes up empty, and reject a result under System32 or WindowsApps either
# way -- including one a previous, pre-fix configure already cached, which
# would otherwise short-circuit this search and keep serving the shim forever.
if(WIN32)
	if(BASH_EXECUTABLE AND (BASH_EXECUTABLE MATCHES "[Ss]ystem32" OR BASH_EXECUTABLE MATCHES "WindowsApps"))
		unset(BASH_EXECUTABLE CACHE)
	endif()
	find_program(BASH_EXECUTABLE NAMES bash
		HINTS
			"$ENV{ProgramFiles}/Git/bin"
			"$ENV{ProgramFiles}/Git/usr/bin"
			"$ENV{ProgramW6432}/Git/bin"
			"C:/msys64/usr/bin"
		NO_DEFAULT_PATH)
	if(NOT BASH_EXECUTABLE)
		find_program(BASH_EXECUTABLE NAMES bash)
	endif()
	if(BASH_EXECUTABLE AND (BASH_EXECUTABLE MATCHES "[Ss]ystem32" OR BASH_EXECUTABLE MATCHES "WindowsApps"))
		message(STATUS "Ignoring ${BASH_EXECUTABLE} -- that's the WSL launcher/App-Execution-Alias shim, not a real bash that can run a Windows-path script")
		unset(BASH_EXECUTABLE CACHE)
		set(BASH_EXECUTABLE "BASH_EXECUTABLE-NOTFOUND")
	endif()
else()
	find_program(BASH_EXECUTABLE NAMES bash)
endif()
# (BASH_EXECUTABLE above also serves test_make_release.py, which drives
# tools/make_release.sh under git-bash/MSYS2 on Windows, /bin/bash on Linux.)
if(CAVEWII_REQUIRE_TEST_TOOLS AND NOT Python3_Interpreter_FOUND)
	message(FATAL_ERROR "Python3 is required for the CaveWii tooling test suite")
endif()
if(CAVEWII_REQUIRE_TEST_TOOLS AND NOT POWERSHELL_EXECUTABLE)
	message(FATAL_ERROR "PowerShell (powershell or pwsh) is required for the CaveWii tooling test suite")
endif()
if(CAVEWII_REQUIRE_TEST_TOOLS AND NOT BASH_EXECUTABLE)
	message(FATAL_ERROR "A real Git-for-Windows/MSYS2 bash (not the WSL launcher or a WindowsApps alias shim) is required for the CaveWii tooling test suite")
	message(FATAL_ERROR "bash is required for the CaveWii tooling test suite")
endif()
if(Python3_Interpreter_FOUND)
	add_test(NAME test_oracle_worldgen COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/run_worldgen_oracle.py
		--bin $<TARGET_FILE:dump_chunk>
		--golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/worldgen/mc173/seed-42-chunk-0_0.txt
		--oracle-name mc173@16f39e7
		--seed 42 --cx 0 --cz 0)
	# Lighting parity vs the external mc173 light engine: the settled sky/block
	# field of the synthetic "arena1" scenario (heightmap sky emission,
	# horizontal under-platform bleed, leaves partial opacity, torch/glowstone
	# spheres) must match the committed mc173 capture cell-for-cell. Closes the
	# lighting leg of the oracle-diff plan (worldgen was the first).
	add_test(NAME test_oracle_lighting COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/run_lighting_oracle.py
		--bin $<TARGET_FILE:dump_light>
		--golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/lighting/mc173/arena1.txt
		--oracle-name mc173@16f39e7
		--scenario arena1)
	# Redstone per-tick parity vs mc173's tick engine: the repeater-circuit
	# fixture's wire powers, repeater output state, and rise/fall timing
	# (x,y,z@tick -> power scheme) must match the committed mc173 capture
	# tick-for-tick. Third oracle-diff leg (worldgen, lighting, redstone).
	add_test(NAME test_oracle_redstone COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/run_lighting_oracle.py
		--bin $<TARGET_FILE:dump_redstone>
		--golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/redstone/rs1_arbitrated.txt
		--oracle-name mc173@16f39e7+decompile-arbitration
		--scenario rs1)
	add_test(NAME test_wii_telemetry_server COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_telemetry_server.py
		--selftest)
	add_test(NAME test_heartbeat_fps_window COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_heartbeat_fps_window.py
		--bin $<TARGET_FILE:test_hw_telemetry>)
	# capture-fix-1003: live-capture client/protocol witnesses and the real
	# client <-> production-service pair (listener recreated per connection).
	add_test(NAME test_remote_capture COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_remote_capture.py)
	add_test(NAME test_remote_capture_service COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_remote_capture_service.py)
	add_test(NAME test_wii_anchor_gap COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wii_anchor_gap.py)
	# push-0921/C7: the build stamp embedded in every artifact is a FIXED
	# 7-char short hash (tools/worktree-stamp.sh, tools/make_release.sh both
	# pin `rev-parse --short=7`), but plain `git rev-parse --short HEAD` lets
	# git choose the length -- 8 chars in this repo -- so a rig script
	# deriving its EXPECTED stamp that way silently disagrees with the
	# embedded one and false-refuses a real run ("build stamp mismatch"),
	# hit on the real rig twice on 2026-09-21. Sweeps every tools/**/*.ps1,
	# tools/**/*.sh and .claude/skills/build-wii/scripts/* for
	# `rev-parse --short` not pinned `--short=7`, with a commented allow-list
	# for the two confirmed label-only uses that never gate a run.
	add_test(NAME test_rig_stamp_short7 COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rig_stamp_short7.py)
	# b1-chat-keyboard-0917: mp_chat_login_3m's marker + receipt contract
	# (ChatLoginEvidence), replayed offline -- no Wii, no server, no socket.
	# Registered here rather than inside the collector's own --selftest so the
	# chat-login rules can be run (and fail) on their own.
	add_test(NAME test_chat_login_evidence COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chat_login_evidence.py)
	add_test(NAME test_tnt_evidence COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tnt_evidence.py)
	# telreliable-0926: heartbeat tick gaps attributed to the device sender
	# counters (tel_fail/tel_wto/tel_hb_repl/...), replayed offline.
	add_test(NAME test_telemetry_heartbeat_gap COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_telemetry_heartbeat_gap.py)
	# splitdims-profile-0926: split_dims_6m collector verdicts
	# (SplitDimsEvidence): dims-active PASS, dims_not_active FAIL, and every
	# rejection class, offline.
	add_test(NAME test_split_dims_collector COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_split_dims_collector.py)
	# colcap-1006 (ASTRA-CC-R3 item 2): anchor-run column pool evidence and
	# accumulated gate violations, graded through the real collector parser
	# on a verbatim real-Wii cc_c1 excerpt plus boundary/malformed cases.
	add_test(NAME test_anchor_pool_collector COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_anchor_pool_collector.py)
	# colcap-1006 (ASTRA-CC-R5 item 2): pool_selftest_3m grading + the
	# cross-run ordered-GX verdict, fed the real C formatter's events
	# (test_pool_selftest --emit-events, run from the build dir).
	add_test(NAME test_pool_selftest_collector COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pool_selftest_collector.py)
	set_tests_properties(test_pool_selftest_collector PROPERTIES
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} TIMEOUT 300)
	# colcap-1006 (ASTRA-CC-R5 item 3): split_regrow_8m drain + warmed
	# regrowth grading and the allocator cost block ("pac").
	add_test(NAME test_regrow_collector COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_regrow_collector.py)
	set_tests_properties(test_regrow_collector PROPERTIES TIMEOUT 300)
	# b1-chat-opener-0919 (Astra ruling 2 D2): mp_chat_chord_1m own marker
	# contract (ChatChordEvidence), replayed offline. A SEPARATE profile and a
	# separate test on purpose -- a chord claim may never be read out of a
	# login run.
	add_test(NAME test_chat_chord_evidence COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chat_chord_evidence.py)
	# b1-chat-opener-0919/05 (review finding 1): every hw_run profile must be
	# registered in EVERY tool that names profiles and in the collector -- a
	# half-registered profile can be requested but never staged, launched,
	# collected or validated, and only a wasted hardware slot reveals it.
	add_test(NAME test_profile_registration COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_profile_registration.py)
	# push-0921/B5 (Astra gate 1, Lane B item 7): mp_play_10m's collector
	# contract (MpPlayEvidence) -- the device marker stream AND the mc173-diag
	# fixture receipts, replayed offline. Registered on its own, like the two
	# chat contracts above, so the play rules can be run (and fail) without a
	# ten-minute hardware slot; the negatives are what the offline replay leg
	# of the PC -> replay -> Wii sequence rests on.
	add_test(NAME test_mp_play_evidence COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mp_play_evidence.py)
	# b1-chat-opener-0919/08 (Astra ruling 3 D5): docs/controls_sheet.md has to
	# stay true. The check recomputes the EFFECTIVE bindings (config_wii.json
	# plus input.c's fallbacks), the control modes and the gesture constants
	# and fails by family + role when the sheet disagrees -- and applies the
	# same coherence rule to the pointer-look preset, so the stick-moves-and-
	# looks hybrid the live card shipped with cannot come back.
	add_test(NAME test_controls_sheet COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_controls_sheet.py)
	# REL-06a: fastcmp equivalence.py capture selection (explicit --capture >
	# FASTCMP_CAPTURE > saved default), refusal of a missing/incomplete capture,
	# and an overridden synthetic capture passing the reference real-tape mutant
	# and planted-divergence equivalence. Hermetic: compiles fastcmp.c into a
	# temp folder with this compiler; never reads the D: evidence capture.
	add_test(NAME test_fastcmp_capture COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fastcmp_capture.py
		--cc ${CMAKE_C_COMPILER})
	set_tests_properties(test_fastcmp_capture PROPERTIES TIMEOUT 600)
	add_test(NAME test_anchor_measurement_clock COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_anchor_measurement_clock.py
		--cc ${CMAKE_C_COMPILER})
	# S-A10 (R-A9 test gaps 4 and 5): main.c's split-screen seams -- the
	# guest-maintenance block (guest_joining's membership, the one-settle latch
	# and its A9-M4 reset placement, the refuse -> settle(false) -> leave
	# ORDER) and the config-flush falling edge (including A9-L9's mid-play
	# world_loaded clear with screen_ingame current). Same mechanism as
	# test_anchor_measurement_clock above: the harness SLICES the production
	# text out of source/main.c and compiles it against stubs, so the seam
	# never had to be extracted out of the loop to become testable and the test
	# cannot drift from what main.c actually runs.
	add_test(NAME test_split_live_seam COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_split_live_seam.py
		--cc ${CMAKE_C_COMPILER})
	# It shells out to the C compiler once; a wedged toolchain must fail the
	# test rather than stall the run (the same reason test_split_live_options
	# carries a timeout).
	set_tests_properties(test_split_live_seam PROPERTIES TIMEOUT 180)
	add_test(NAME test_hardware_proof_nudge COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/.claude/hooks/hardware-proof-nudge.py
		--selftest)
	# 2026-09-02 (lane/process): the hard commit/merge gate and the blanket-add
	# guard both grew --selftest modes; wire them into ctest so `ctest -R
	# 'nudge|hw_proof|blanket|guard'` (the lane's own verification command)
	# actually exercises all of them, not just the pre-existing nudge.
	add_test(NAME test_require_hw_proof COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/.claude/hooks/require-hw-proof.py
		--selftest)
	add_test(NAME test_no_blanket_add COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/.claude/hooks/no-blanket-add.py
		--selftest)
	if(BASH_EXECUTABLE)
		add_test(NAME test_guard_ip COMMAND ${BASH_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/.claude/hooks/guard-ip.sh
			--selftest)
		# KANBAN Reported 09-14: the Wii stack-usage gate forwards LIBOGC_LIB to
		# `make dev` (Makefile sidecar gate) and has a --scan-only mode. Runs a
		# copy of the script in a scratch repo with a stub make; no devkitPPC.
		add_test(NAME test_check_wii_stack_usage COMMAND ${BASH_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_check_wii_stack_usage.sh)
	endif()
	if(POWERSHELL_EXECUTABLE)
		add_test(NAME test_run_staged_tnt_forwarding COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_run_staged_tnt_forwarding.py
			--powershell ${POWERSHELL_EXECUTABLE})
		add_test(NAME test_validate_wii_pass COMMAND ${POWERSHELL_EXECUTABLE}
			-NoProfile -ExecutionPolicy Bypass
			-File ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_wii_pass.ps1
			-SelfTest)
		add_test(NAME test_invoke_wii_power_cycle COMMAND ${POWERSHELL_EXECUTABLE}
			-NoProfile -ExecutionPolicy Bypass
			-File ${CMAKE_CURRENT_SOURCE_DIR}/tools/invoke_wii_power_cycle.ps1
			-SelfTest)
		# 2026-09-24 review fix S5: run_wii_hardware.ps1 must fail closed on a
		# stale/missing/invalid verdict or a null/nonzero collector exit, and
		# still power-cycle on those failures. Mocked; no Wii or network.
		add_test(NAME test_run_wii_hardware_verdict COMMAND ${POWERSHELL_EXECUTABLE}
			-NoProfile -ExecutionPolicy Bypass
			-File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_run_wii_hardware_verdict.ps1)
		add_test(NAME test_collect_staged_validator COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_collect_staged_validator.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
		add_test(NAME test_run_staged_power_cycle COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_run_staged_power_cycle.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
		add_test(NAME test_run_wii_power_cycle COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_run_wii_power_cycle.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
		# 2026-09-25 (PWRCYCLE, Astra 09-24 follow-up): the five throw paths of
		# run_wii_hardware.ps1 that precede or replace a collector verdict
		# (build-verify failure, missing .dol, missing wiiload exe, wiiload
		# attempts exhausted, collector timeout) must each run the configured
		# power-cycle hook exactly once under -PowerCycleOnFailure and never
		# without it. Drives a scratch copy of the runner with fakes; the
		# collector-timeout case waits out the runner's real 60 s budget twice
		# (switch on / off), hence the timeout. No Wii, no network.
		add_test(NAME test_run_wii_hardware_failure_paths COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_run_wii_hardware_failure_paths.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
		set_tests_properties(test_run_wii_hardware_failure_paths PROPERTIES TIMEOUT 600)
		add_test(NAME test_wii_profile_matrix COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wii_profile_matrix.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
		add_test(NAME test_wii_profile_matrix_power_cycle_between COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wii_profile_matrix_power_cycle_between.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
		add_test(NAME test_wii_profile_matrix_resume COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wii_profile_matrix_resume.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
		add_test(NAME test_wii_lighting_bench_runner COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wii_lighting_bench_runner.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
		add_test(NAME test_run_staged_mailbox COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_run_staged_mailbox.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
		# 2026-09-02 cw_run_args.txt abort/cleanup hardening (lane/run-args-
		# hardening): pins the run_staged_wii_hardware.ps1 try/finally that
		# neutralizes a staged-but-never-consumed cw_run_args.txt (the
		# reported stale-armed-file incident) -- fires on a zero-heartbeat
		# staged run, and does NOT fire on an explicit -NoCollect run. Named
		# so it does NOT match the lane's own `ctest -E
		# 'wii_profile_matrix|power_cycle'` exclusion filter (unlike its
		# sibling test_run_staged_power_cycle above) -- it drives two real
		# stage-server round trips per subprocess but no telemetry wait, so
		# it stays fast.
		add_test(NAME test_run_staged_neutralize_args COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_run_staged_neutralize_args.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
		# 2026-09-02 fetch sha256/streamed-bytes mismatch fix (fetch_file()
		# in tools/wii_stage_server/source/main.c, Wii-only so not
		# ctest-buildable directly): pins the PC-side half of the fix --
		# tools/fetch_wii_stage_file.ps1 must reject a CWFETCH1 reply whose
		# declared sha256 does not match the streamed bytes, and still
		# accept a well-behaved one.
		add_test(NAME test_fetch_wii_stage_file_sha_mismatch COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fetch_wii_stage_file_sha_mismatch.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
		# 2026-09-02 put_wii_stage_file.ps1 (generic CWSTAGE4 file PUT,
		# promoted from the scratchpad put_kz.ps1 one-off): pins the
		# PC-side sha256-verification half the same way
		# test_fetch_wii_stage_file_sha_mismatch does for the GET
		# direction -- the client must reject a stage server PASS reply
		# whose declared sha256 doesn't match the uploaded bytes, and
		# still accept a well-behaved one.
		add_test(NAME test_put_wii_stage_file_sha_mismatch COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_put_wii_stage_file_sha_mismatch.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})

		# 2026-09-21 push-0921/C8 saves tools. The PURE PowerShell helpers
		# (paged-inventory consistency, the write-ahead manifest reader, the
		# reconcile truth table, the bounded level.dat decode, the rig-lock
		# discipline and the run-created-world identification) are pinned by
		# cw_saves_common.ps1's own selftest; the NETWORK behaviour of the
		# three tools is driven against a python fake stage server that also
		# models the card, so the tests can assert what did and did not move.
		add_test(NAME test_stage_saves_tools_selftest COMMAND ${POWERSHELL_EXECUTABLE}
			-NoProfile -ExecutionPolicy Bypass
			-File ${CMAKE_CURRENT_SOURCE_DIR}/tools/list_wii_saves.ps1
			-SelfTest)
		add_test(NAME test_stage_saves_tools COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stage_saves_tools.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--powershell ${POWERSHELL_EXECUTABLE})
	else()
		message(STATUS "powershell not found -- skipping PowerShell tooling tests")
	endif()
	if(BASH_EXECUTABLE)
		# 2026-09-02 release pipeline (Codex review P0: no release pipeline
		# existed; CI shipped a bare stock .dol, meta.xml said 0.0.1-dev
		# dated May). Drives tools/make_release.sh end-to-end against a fake
		# .dol/.elf/PASS.json via its --skip-objdump-gate-for-tests escape
		# hatch (which is refused whenever --pass is supplied or
		# --allow-uncertified is absent, so the hardware-certified path can
		# never take it) and asserts: -stock refused, missing PASS refused,
		# --publish always refused, and on the happy path -- zip layout,
		# SHA256SUMS covers every file and verifies, provenance.json fields
		# present, and the excluded assets (assets/armor/*.png,
		# assets/controls.png) are genuinely absent from the zip.
		add_test(NAME test_make_release COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_make_release.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--bash ${BASH_EXECUTABLE})
		# Builds several full release zips (incl. the tutorial-world cases);
		# far past ctest's default limit on Windows, passes when unbounded.
		set_tests_properties(test_make_release PROPERTIES TIMEOUT 1800)
		# bootdeath-0917 / K12 (Astra ruling 2 D1, 2026-09-17): the post-link
		# refusal gate on libogc's raw SD DMA buffer, shared by the Makefile's
		# .dol rule and both build-verify.sh entry points. Drives
		# tools/check_sd0_dma_buffer.sh with a FAKE nm so the 4 KB-crossing
		# boundaries, the 64-byte alignment rule, the size rule and every
		# fail-closed path are testable with no devkitPPC and no Wii build -- a
		# real .elf cannot be made to place a library symbol at a chosen
		# address. Same bash-driven shape as test_make_release above. The
		# preserved experiment ELFs live outside the repo and are checked only
		# when present (CI skips them).
		add_test(NAME test_check_sd0_dma_buffer COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_check_sd0_dma_buffer.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--bash ${BASH_EXECUTABLE})
		# hw1029b (2026-10-29): post-build refusal gate on a foreign/stale Wii
		# object dir (tools/check_build_deps.sh) -- a build/ copied between runner
		# clones kept the other clone's .d header paths and linked mixed struct offsets.
		add_test(NAME test_build_gate_deps COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/tests/test_build_gate_deps.py
			--repo ${CMAKE_CURRENT_SOURCE_DIR}
			--bash ${BASH_EXECUTABLE})
	else()
		message(STATUS "bash not found -- skipping test_make_release")
		message(STATUS "bash not found -- skipping test_check_sd0_dma_buffer")
	endif()
else()
	message(STATUS "Python3 not found -- skipping Python tooling tests")
endif()

# gameplay-quality scorer self-test gate: scripts/gameplay_assess.py --selftest
# runs synthetic run logs through the PURE assess()/render_markdown() and asserts
# the four objective dimension scores + the solid/rough/broken verdict (its in-file
# _selftest, gameplay_assess.py:533-602). Tooling only -- no Beta-rule / sim code is
# touched -- so it gates the scorer against drift the same way amahpour-CavEX wires
# its scorer into CI (amahpour gameplay_assess.py:33-34, the documented --selftest
# entry point). Reuse the interpreter already validated by find_package(Python3)
# above. A second PATH-only search can disagree with FindPython3 on Windows
# (registry-discovered Python versus WindowsApps command aliases) and silently
# drop this test from an otherwise complete inventory.
if(Python3_Interpreter_FOUND)
	add_test(NAME test_gameplay_assess COMMAND ${Python3_EXECUTABLE}
		${CMAKE_SOURCE_DIR}/scripts/gameplay_assess.py --selftest)
else()
	message(STATUS "Python3 not found -- skipping test_gameplay_assess (gameplay-quality scorer self-test)")
endif()

# lane/owned-assets-importer (2026-09-02): tools/import_owned_beta_assets.py,
# the "user owns the jar" half of the hybrid shipping strategy
# (docs/ship_distribution_plan_2026-08-17.md Track 2 / docs/import_owned_assets.md).
# Pure Python + a synthetic jar/resources fixture -- no git plumbing, no bash --
# so this needs only the same Python3 interpreter gameplay_assess already uses.
if(Python3_Interpreter_FOUND)
	add_test(NAME test_import_owned_beta_assets COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_import_owned_beta_assets.py)
	# Every engine sound key is searched by the importer or allowlisted (synthetic data).
	add_test(NAME test_sound_import_coverage COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sound_import_coverage.py)
else()
	message(STATUS "Python3 not found -- skipping test_import_owned_beta_assets")
endif()

# EAD-007: every pack's anim.png = Beta motion/alpha in its own colour ramp, checked
# against packs/beta-base/anim.png by inverting Beta's own colour formulas
# (independent of tools/gen_anim_fx). Pillow, like the other pack tests.
if(Python3_Interpreter_FOUND)
	add_test(NAME test_anim_pack_colours COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_anim_pack_colours.py)
endif()

# Offline spawn finder: locate a dramatic on-land mountain spawn for a seed.
add_executable(find_spawn
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/worldgen.c
				source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	tools/find_spawn.c
)
target_compile_definitions(find_spawn PRIVATE PLATFORM_PC)
target_include_directories(find_spawn PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(find_spawn m)

# Offline lake finder: locate an above-sea-level populate pond (+ a shore spawn)
# for a seed, by running terrain + populate (lakes) over a grid of quadrants.
add_executable(find_lake
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	source/worldgen/worldgen.c
				source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	tools/find_lake.c
)
target_compile_definitions(find_lake PRIVATE PLATFORM_PC)
target_include_directories(find_lake PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(find_lake m)

# Microbenchmark: terrain vs populate vs relight cost per worldgen quadrant.
add_executable(bench_worldgen
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	source/worldgen/worldgen.c
				source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	tests/bench_worldgen.c
)
target_compile_definitions(bench_worldgen PRIVATE PLATFORM_PC WORLDGEN_PROFILE)
target_include_directories(bench_worldgen PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(bench_worldgen m)
# Match the shipping build's cross-TU inlining: cavex has IPO/LTO on (and the Wii
# .dol uses -flto=auto), so without this the bench links noise.c as a separate TU
# and its "% of terrain" numbers include call overhead into perlin_generate/
# grad/fade/lerp that the real build already removes. Measure against the real
# baseline, not an un-inlined bench.
set_property(TARGET bench_worldgen PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

# Far-ring chunk-fill bench + equivalence guard: the direct-fill path (mirrors
# world_build_far_section) vs a slow per-cell reference producing the same current
# synthetic output. Standalone (replicates the structs/hot paths -- world.c can't
# link in isolation). Gates byte-identical Overworld+Nether blocks/heightmaps,
# including mixed no-surface/lava columns, and prints the measured speedup.
add_executable(bench_far_chunk
	tests/bench_far_chunk.c
)
target_link_libraries(bench_far_chunk m)
add_test(NAME bench_far_chunk COMMAND bench_far_chunk)

# Client lighting budget bench: wraps the real lighting_flood_resume hot loop
# and reports compute-only cost for one production 4096-pop render-thread slice.
add_executable(bench_lighting_budget
	source/lighting.c
	source/stack.c
	tests/bench_lighting_budget.c
)
target_compile_definitions(bench_lighting_budget PRIVATE PLATFORM_PC)
target_include_directories(bench_lighting_budget PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(bench_lighting_budget m)
add_test(NAME bench_lighting_budget COMMAND bench_lighting_budget)

# Targeted post-populate relight test (heightmap/skylight/block-light).
add_executable(test_relight
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	source/worldgen/worldgen.c
				source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	source/lighting.c
	source/stack.c
	tests/test_relight.c
)
target_compile_definitions(test_relight PRIVATE PLATFORM_PC)
target_include_directories(test_relight PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_relight m)
add_test(NAME test_relight COMMAND test_relight)

# D1 skylight occlusion: proves the gen-time horizontal skylight bleed lights
# under-overhang/cave-mouth cells to the brute-force fixed point while leaving open
# columns byte-unchanged (drives the real worldgen_lighting + lighting flood).
# M4 furnace: pin the Beta smelt-recipe map (shared by the instant furnace + the
# timed-furnace tick env). Pure -- links only furnace_recipes.c.
add_executable(test_furnace_recipes
	source/block/furnace_recipes.c
	tests/test_furnace_recipes.c
)
target_link_libraries(test_furnace_recipes m)
target_compile_definitions(test_furnace_recipes PRIVATE PLATFORM_PC)
target_include_directories(test_furnace_recipes PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_furnace_recipes COMMAND test_furnace_recipes)

# M4 furnace: golden test for the Beta TileEntityFurnace burn/cook tick model,
# authored before the rewrite (drives the real furnace_tick headlessly).
add_executable(test_furnace_burn
	source/block/furnace_logic.c
	tests/test_furnace_burn.c
)
target_link_libraries(test_furnace_burn m)
target_compile_definitions(test_furnace_burn PRIVATE PLATFORM_PC)
target_include_directories(test_furnace_burn PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_furnace_burn COMMAND test_furnace_burn)

# D2 (dispenser-slice adversarial review): regression test for the zero-seed
# rand_gen_gaussian hang + its rand_gen() root fix. Pure -- links only util.c
# (needs libm for sqrtf/logf, unlike the two furnace tests above).
add_executable(test_rand_gen
	source/util.c
	tests/test_rand_gen.c
)
target_compile_definitions(test_rand_gen PRIVATE PLATFORM_PC)
target_include_directories(test_rand_gen PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_rand_gen m)
add_test(NAME test_rand_gen COMMAND test_rand_gen)

# M4 redstone: golden test for Beta's BOOLEAN power model (redstone_power.c, step 3) -- the
# faithful replacement of the interim power stand-ins. Pins torch direct power, strong power
# re-emitted through a normal cube, the wiresProvidePower toggle, the faithful NOT-gate input
# (wire run into a block face), no torch self-power, and pressure-plate sourcing.
add_executable(test_redstone_power_golden
	source/block/redstone_power.c
	tests/test_redstone_power_golden.c
)
target_link_libraries(test_redstone_power_golden m)
target_compile_definitions(test_redstone_power_golden PRIVATE PLATFORM_PC)
target_include_directories(test_redstone_power_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_redstone_power_golden COMMAND test_redstone_power_golden)

# M4 redstone: golden test for the LEVER (BlockLever, id 69) as a toggleable power source. Drives
# the REAL two-layer stack (redstone_power.c boolean source feeding redstone_logic.c's wire flood,
# exactly as redstone_server.c wires them) and asserts the adjacent wire's integer strength: OFF=0,
# ON=15, OFF=0 -- plus the lever's mount-face weak power re-emitted through a cube.
add_executable(test_redstone_lever_power
	source/block/redstone_power.c
	source/block/redstone_logic.c
	tests/test_redstone_lever_power.c
)
target_link_libraries(test_redstone_lever_power m)
target_compile_definitions(test_redstone_lever_power PRIVATE PLATFORM_PC)
target_include_directories(test_redstone_lever_power PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_redstone_lever_power COMMAND test_redstone_lever_power)

# Redstone-wire RENDER faithfulness: pins the shared connection probe (redstone_wire_connections,
# Beta var26-29 == var6-9) and the renderer's var35 cross/line/rotated-line shape selection across
# every player topology (dot, straight, L-corner, T, +, source connect, no-connect-to-wall, the
# +-1 vertical stair links + ceiling gate, repeater facing). Guards render_block_redstone_wire.
add_executable(test_redstone_wire_render_connections
	source/block/redstone_power.c
	tests/test_redstone_wire_render_connections.c
)
target_link_libraries(test_redstone_wire_render_connections m)
target_compile_definitions(test_redstone_wire_render_connections PRIVATE PLATFORM_PC)
target_include_directories(test_redstone_wire_render_connections PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_redstone_wire_render_connections COMMAND test_redstone_wire_render_connections)

# M4 redstone: golden test for Beta BlockRedstoneWire power spread (synchronous recursive
# flood, flat-15 source, -1/hop fade), authored before the wire rewrite (golden-suite-first).
add_executable(test_redstone_wire_attenuation_golden
	source/block/redstone_logic.c
	tests/test_redstone_wire_attenuation_golden.c
)
target_link_libraries(test_redstone_wire_attenuation_golden m)
target_compile_definitions(test_redstone_wire_attenuation_golden PRIVATE PLATFORM_PC)
target_include_directories(test_redstone_wire_attenuation_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_redstone_wire_attenuation_golden COMMAND test_redstone_wire_attenuation_golden)

# M4 redstone: torch inverter golden -- drives redstone_torch_update through the REAL
# scheduled-tick queue to verify the Beta 2-game-tick toggle delay on both edges.
add_executable(test_redstone_torch_inverter_2tick_golden
	source/block/redstone_logic.c
	source/network/server_world_tick_queue.c
	tests/test_redstone_torch_inverter_2tick_golden.c
)
target_link_libraries(test_redstone_torch_inverter_2tick_golden m)
target_compile_definitions(test_redstone_torch_inverter_2tick_golden PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_redstone_torch_inverter_2tick_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_redstone_torch_inverter_2tick_golden COMMAND test_redstone_torch_inverter_2tick_golden)

# M4 redstone: repeater delay-table golden (step 6) -- drives redstone_repeater.c through the REAL
# scheduled-tick queue to verify the Beta {2,4,6,8} game-tick delay (field_22023_b*2, applied
# once) on both edges, for all 4 delay settings.
add_executable(test_redstone_repeater_delay_table_golden
	source/block/redstone_repeater.c
	source/network/server_world_tick_queue.c
	tests/test_redstone_repeater_delay_table_golden.c
)
target_link_libraries(test_redstone_repeater_delay_table_golden m)
target_compile_definitions(test_redstone_repeater_delay_table_golden PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_redstone_repeater_delay_table_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_redstone_repeater_delay_table_golden COMMAND test_redstone_repeater_delay_table_golden)

# M4 pistons (phase 1): golden for Beta BlockPistonBase DECISION logic (redstone_piston.c) over a
# flat stub cell grid -- quasi-connectivity incl. the block-above BUD quirk + front-face exclusion,
# canPushBlock per mobility class, canExtend at the 12-block limit, the updatePistonState
# extend/retract truth table, and determineOrientation. NO engine wiring (decision-only, zero Wii risk).
add_executable(test_piston_logic_golden
	source/block/redstone_piston.c
	tests/test_piston_logic_golden.c
)
target_link_libraries(test_piston_logic_golden m)
target_compile_definitions(test_piston_logic_golden PRIVATE PLATFORM_PC)
target_include_directories(test_piston_logic_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_piston_logic_golden COMMAND test_piston_logic_golden)

# M4 pistons (phase 2a): golden for the Beta BlockPistonBase MATERIALIZATION (piston_extend / tryExtend
# + piston_retract / playBlock) -- records the exact add_moving_te/set_block/settle/drop op SEQUENCE the
# move emits over a stub grid and pins it to the decompile (the column shift + head, destroy-terminal
# drop, the 12-limit refusal, and the non-sticky/sticky retract). Pure module, no engine -- zero Wii risk.
add_executable(test_piston_materialize_golden
	source/block/redstone_piston.c
	source/block/piston_moving.c
	tests/test_piston_materialize_golden.c
)
target_link_libraries(test_piston_materialize_golden m)
target_compile_definitions(test_piston_materialize_golden PRIVATE PLATFORM_PC)
target_include_directories(test_piston_materialize_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_piston_materialize_golden COMMAND test_piston_materialize_golden)

# Moving-piston RPC golden: exact float-bit wire round-trip, 13-byte compact
# chunk records, uncapped dynamic ownership, atomic chunk replacement, >255
# sidecars in one chunk, stale-host filtering, and loaded-list snapshot order.
add_executable(test_piston_moving_transport
	source/block/piston_moving.c
	source/network/piston_moving_transport.c
	tests/test_piston_moving_transport.c
)
target_link_libraries(test_piston_moving_transport m)
target_compile_definitions(test_piston_moving_transport PRIVATE PLATFORM_PC)
target_include_directories(test_piston_moving_transport PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_piston_moving_transport PROPERTIES C_STANDARD 99)
add_test(NAME test_piston_moving_transport COMMAND test_piston_moving_transport)

# M4 pistons (phase 2a): golden re-pinning the Beta mobility table (piston_mobility.c) to the decompile
# (Material.getMaterialMobility + the bed/door/pressure-plate overrides) so a silent table edit can't
# drift from Beta. Also pins the deliberate flag-0 absences (tile-entity blocks, obsidian/bedrock).
add_executable(test_piston_mobility_golden
	source/block/piston_mobility.c
	tests/test_piston_mobility_golden.c
)
target_link_libraries(test_piston_mobility_golden m)
target_compile_definitions(test_piston_mobility_golden PRIVATE PLATFORM_PC)
target_include_directories(test_piston_mobility_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_piston_mobility_golden COMMAND test_piston_mobility_golden)

# M4 pistons (phase 4a): recipe-match golden pinning the Beta piston + sticky-piston crafting recipes
# (CraftingManager). Pure shape-match, links only recipe.c.
add_executable(test_piston_recipe
	source/item/recipe.c
	tests/test_piston_recipe.c
)
target_link_libraries(test_piston_recipe m)
target_compile_definitions(test_piston_recipe PRIVATE PLATFORM_PC)
target_include_directories(test_piston_recipe PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_piston_recipe COMMAND test_piston_recipe)

# lane/items: recipe-match goldens for five Beta CraftingManager recipes that
# were entirely absent from recipe.c -- chest/furnace minecart, boat,
# painting, lever. Pure shape-match, links only recipe.c (the
# test_piston_recipe.c convention).
add_executable(test_new_item_recipes
	source/item/recipe.c
	tests/test_new_item_recipes.c
)
target_link_libraries(test_new_item_recipes m)
target_compile_definitions(test_new_item_recipes PRIVATE PLATFORM_PC)
target_include_directories(test_new_item_recipes PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_new_item_recipes COMMAND test_new_item_recipes)

# Batch C (FH-09/FH-10/FH-11): the shapeless recipe engine (ShapelessRecipes
# matches semantics: any-position, no-extras, meta-sensitive, list-removal)
# + RecipesDyes' 31 recipes (16 wool recolours + 15 dye-mix goldens), plus the
# shaped cookie and fishing-rod recipes. Pure id+durability matching, links
# only recipe.c (the test_piston_recipe / test_sign_recipe convention).
add_executable(test_shapeless_recipes
	source/item/recipe.c
	tests/test_shapeless_recipes.c
)
target_link_libraries(test_shapeless_recipes m)
target_compile_definitions(test_shapeless_recipes PRIVATE PLATFORM_PC)
target_include_directories(test_shapeless_recipes PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_shapeless_recipes COMMAND test_shapeless_recipes)

# Faithfulness PIN: Beta 1.7.3 chainmail armor IS craftable from fire (the 'fire armor' quirk). Positive
# test so the stale "chain armor should not be craftable" claim cannot be acted on without failing here.
add_executable(test_chain_armor_recipe
	source/item/recipe.c
	tests/test_chain_armor_recipe.c
)
target_link_libraries(test_chain_armor_recipe m)
target_compile_definitions(test_chain_armor_recipe PRIVATE PLATFORM_PC)
target_include_directories(test_chain_armor_recipe PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_chain_armor_recipe COMMAND test_chain_armor_recipe)

# M4 pistons (phase 4b): golden for the directional face textures (piston_face_texat) -- Beta
# getBlockTextureFromSideAndMetadata: front plate/inner, back, sides, for base/sticky/head across facings.
add_executable(test_piston_texture
	source/block/piston_texture.c
	tests/test_piston_texture.c
)
target_link_libraries(test_piston_texture m)
target_compile_definitions(test_piston_texture PRIVATE PLATFORM_PC)
target_include_directories(test_piston_texture PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_piston_texture COMMAND test_piston_texture)

# TNT face textures golden -- Beta BlockTNT.getBlockTextureFromSide: side/top/bottom = idx/+1/+2
# (the underside is the plain crate bottom, not the fuse-ringed top); primed rendering
# keeps these base tiles and applies its white pulse in the entity renderer, never snow.
add_executable(test_tnt_texture
	source/block/tnt_texture.c
	tests/test_tnt_texture.c
)
target_link_libraries(test_tnt_texture m)
target_compile_definitions(test_tnt_texture PRIVATE PLATFORM_PC)
target_include_directories(test_tnt_texture PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_tnt_texture COMMAND test_tnt_texture)

# Beta 1.7.3 TextureWaterFX port guard: pins the per-texel colour formula (clamp ->
# v^2 -> R/G/B/A with Blue pinned at 255), the resting flat tile, channel ranges
# across many live ticks, animation, and fixed-seed determinism. water_fx.c is PURE
# (engine-free) so it links standalone here.
add_executable(test_water_fx
	source/graphics/water_fx.c
	tests/test_water_fx.c
)
target_link_libraries(test_water_fx m)
target_include_directories(test_water_fx PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_water_fx COMMAND test_water_fx)

# TextureWaterFlowFX -- the FLOW tile (every horizontal face of a water block).
# Pins the B=255 invariant the fork placeholder art violated, plus the scroll and
# the five kernel deltas that separate flow from still.
add_executable(test_water_flow_fx
	source/graphics/water_fx.c
	source/graphics/water_flow_fx.c
	tests/test_water_flow_fx.c
)
target_link_libraries(test_water_flow_fx m)
target_include_directories(test_water_flow_fx PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_water_flow_fx COMMAND test_water_flow_fx)

# Beta 1.7.3 TextureLavaFX + TextureLavaFlowFX port guard: pins the per-texel colour
# formula (v=clamp(field*2,0,1); R=v*100+155, G=v^2*255, B=v^4*128, A=255), the
# resting flat tile, channel ranges over live ticks, both still+flow tiles animating,
# and fixed-seed determinism. lava_fx.c is PURE (math.h only) -> links standalone.
add_executable(test_lava_fx
	source/graphics/lava_fx.c
	tests/test_lava_fx.c
)
target_include_directories(test_lava_fx PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_lava_fx m)
add_test(NAME test_lava_fx COMMAND test_lava_fx)

# Beta 1.7.3 TexturePortalFX port guard: pins the purple colour ramp (incl. the
# authentic byte WRAP above intensity 1), fixed-seed(100) determinism of the
# whole 32-frame strip (FNV self-golden), and the spiral rotation.
add_executable(test_portal_fx
	source/graphics/portal_fx.c
	source/worldgen/beta_math.c
	source/worldgen/java_random.c
	tests/test_portal_fx.c
)
target_include_directories(test_portal_fx PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_portal_fx m)
add_test(NAME test_portal_fx COMMAND test_portal_fx)

# ead-fx-ramp: offline RGB ramps preserve Beta alpha and the no-ramp filmstrips.
add_executable(test_anim_fx_ramp
	source/graphics/lava_fx.c
	source/graphics/flame_fx.c
	source/graphics/water_fx.c
	source/graphics/water_flow_fx.c
	source/graphics/portal_fx.c
	source/worldgen/beta_math.c
	source/worldgen/java_random.c
	tests/test_anim_fx_ramp.c
)
target_include_directories(test_anim_fx_ramp PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/source/graphics
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_anim_fx_ramp m)
add_test(NAME test_anim_fx_ramp COMMAND test_anim_fx_ramp)

# Beta 1.7.3 TextureFlamesFX port guard: pins the per-texel colour formula
# (v=clamp(field*1.8,0,1); R=v*155+100, G=v^2*255, B=v^10*255, A=v<0.5?0:255), the
# resting flat tile, channel ranges + hard 0/255 alpha over live ticks, animation,
# and fixed-seed determinism. flame_fx.c is PURE (engine-free) -> links standalone.
add_executable(test_flame_fx
	source/graphics/flame_fx.c
	tests/test_flame_fx.c
)
target_link_libraries(test_flame_fx m)
target_include_directories(test_flame_fx PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_flame_fx COMMAND test_flame_fx)

# fire-anim-0924/F1: Beta's TWO TextureFlamesFX instances (Minecraft.java:282-283)
# -- per-instance state, one shared RNG stream consumed instance 0 then 1, each
# byte-exact to an in-test transcription of TextureFlamesFX.onTick. PURE.
add_executable(test_flame_fx_two_instances
	source/graphics/flame_fx.c
	tests/test_flame_fx_two_instances.c
)
target_link_libraries(test_flame_fx_two_instances m)
target_include_directories(test_flame_fx_two_instances PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_flame_fx_two_instances COMMAND test_flame_fx_two_instances)

# fire-anim-0924/F1: tex_init's anim.png fire+16 fallback -- a pack whose cols 6/13
# are transparent or the magenta placeholder keeps every fire+16 user on cols 3/10.
# Header-only classifier + lodepng; reads assets/anim.png and the CC0 Earth & Ash
# anim.png from the source root (read only).
add_executable(test_anim_fire_strip_fallback
	source/lodepng/lodepng.c
	tests/test_anim_fire_strip_fallback.c
)
target_include_directories(test_anim_fire_strip_fallback PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_anim_fire_strip_fallback m)
add_test(NAME test_anim_fire_strip_fallback COMMAND test_anim_fire_strip_fallback
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Beta 1.7.3 TextureCompassFX port guard (2026-09-03, lane/parity-compass-clock):
# hand-derived rest-frame pin (sin/cos land IEEE-exact at 0.0/1.0, so the needle
# pixel coordinates are boundary-safe) + a 2000-tick equivalence sweep against an
# INDEPENDENT straight-line re-transcription of the decompile embedded in the test
# file (CODESTYLE.md 4.1/4.2 -- not self-derived from compass_fx.c) + determinism.
# compass_fx.c is PURE (math.h only) -> links standalone.
add_executable(test_compass_fx
	source/graphics/compass_fx.c
	tests/test_compass_fx.c
)
target_link_libraries(test_compass_fx m)
target_include_directories(test_compass_fx PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_compass_fx COMMAND test_compass_fx)

# Beta 1.7.3 TextureWatchFX port guard (2026-09-03, lane/parity-compass-clock):
# same discipline as test_compass_fx -- a hand-derived exact-arithmetic rest-frame
# pin, a NULL-dial pass-through pin, and a 2000-tick equivalence sweep against an
# independent reference port. watch_fx.c is PURE (math.h only) -> links standalone.
add_executable(test_watch_fx
	source/graphics/watch_fx.c
	tests/test_watch_fx.c
)
target_link_libraries(test_watch_fx m)
target_include_directories(test_watch_fx PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_watch_fx COMMAND test_watch_fx)

# CONTENT pin on the animated-texture atlas. The three FX ports above are golden-tested
# as pure algorithms, but fire/lava/portal render from anim.png CELLS (they are
# .transparent -> pass 1, which binds anim.png globally at world.c:1572), and the
# offline bake that puts the port output INTO anim.png was written and never run against
# a shipped asset -- so all 28 frames of all three tiles were fork art (the portal a flat
# magenta placeholder) while make_beta_pack.ps1's existence-only gate printed OK. This
# re-runs the ports in-process with the generator's exact seeds/warmup/stride and
# compares every texel. lodepng is linked here for the first time in any test target.
# WORKING_DIRECTORY = the source root: the test opens assets/anim.png by relative path.
add_executable(test_anim_pack_art
	source/graphics/water_fx.c
	source/graphics/water_flow_fx.c
	source/graphics/lava_fx.c
	source/graphics/flame_fx.c
	source/graphics/portal_fx.c
	source/worldgen/beta_math.c
	source/worldgen/java_random.c
	source/lodepng/lodepng.c
	tests/test_anim_pack_art.c
)
target_include_directories(test_anim_pack_art PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_anim_pack_art m)
add_test(NAME test_anim_pack_art COMMAND test_anim_pack_art
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Beta 1.7.3 chest window (handoff 2026-07-29 P2). screen_chest.c drew the background as ONE
# 176x167 blit; Beta's container.png is a SIX-row DOUBLE-chest sheet needing TWO rects that
# SKIP source rows 71..125 for a small chest -- hence the empty rows seen on hardware. The
# layout is extracted into pure functions here so the rects, the 63 slot origins, the
# 167-vs-168 centering trap and the two labels are all golden-testable without a GL context.
# check [6] additionally decodes the SHEET and asserts its cell geometry: the bundled asset
# was authored to the FORK's slot rows and is only 166 rows tall, so the code fix alone would
# have broken the default pack. lodepng is linked for the asset half (the test_anim_pack_art
# precedent). WORKING_DIRECTORY = source root: it opens assets/ and source/ by relative path.
add_executable(test_chest_layout
	source/game/gui/chest_layout.c
	source/lodepng/lodepng.c
	tests/test_chest_layout.c
)
target_link_libraries(test_chest_layout m)
target_include_directories(test_chest_layout PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_chest_layout COMMAND test_chest_layout
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# CAVEWII_AGENT bridge ACT-half guard: agent_input's parser + frame-latch edge semantics
# (HOLD level, PULSE one-shot rising edge, degrees->radians look). Standalone -- agent_input.c
# is self-contained (no gstate), so the test links only it. PLATFORM_PC selects its body and
# pthread satisfies the (unused-here) reader thread's pthread_create reference.
add_executable(test_agent_input
	source/game/agent_input.c
	# agent_input.c writes the core-owned chat controller-mode flag
	# (gui/chat_composer_model.h): link the pure model that defines it.
	source/game/gui/chat_composer_model.c
	tests/test_agent_input.c
)
target_link_libraries(test_agent_input m)
target_compile_definitions(test_agent_input PRIVATE PLATFORM_PC)
target_include_directories(test_agent_input PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_agent_input pthread)
add_test(NAME test_agent_input COMMAND test_agent_input)

# Wii display-list SIZING guard: the redstone-wire + lever renderers' count_only pass must
# return EXACTLY the quads their emit pass writes (a mismatch overflows the exactly-sized GX
# list under NDEBUG -> the frozen-no-dump freeze class). Links render_block.c for real; the
# test file supplies trivial link-only stubs for the engine symbols render_block.c's OTHER
# (never-called) functions reference, so the suite stays self-contained (no engine registry).
add_executable(test_redstone_render_count
	source/graphics/render_block.c
	source/block/redstone_power.c
	tests/test_redstone_render_count.c
)
target_compile_definitions(test_redstone_render_count PRIVATE PLATFORM_PC)
target_include_directories(test_redstone_render_count PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_redstone_render_count m)
add_test(NAME test_redstone_render_count COMMAND test_redstone_render_count)

# FH-04/FH-05/FH-06 (docs/fork_hack_audit_2026-08-21.md): portal shouldSideBeRendered predicate
# + stairs corner-riser / fence post+rail bounds-derived UV, all against the real render_block.c
# renderers. Same self-contained link pattern as test_redstone_render_count (render_block.c only,
# link-only stubs for the leaf engine symbols its OTHER never-called functions reference; this
# test's one real dependency the wire renderer would need, redstone_wire_connections, is stubbed
# the same way test_redstone_render_count stubs render_block_fluid's unused dependency).
add_executable(test_render_block_beta_uv_golden
	source/graphics/render_block.c
	tests/test_render_block_beta_uv_golden.c
)
target_compile_definitions(test_render_block_beta_uv_golden PRIVATE PLATFORM_PC)
target_include_directories(test_render_block_beta_uv_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_render_block_beta_uv_golden m)
add_test(NAME test_render_block_beta_uv_golden COMMAND test_render_block_beta_uv_golden)

# next-gaps-0924/X1: Beta renderBlockFire half-coordinate U swap, emitted UV golden.
add_executable(test_fire_block_uv_flip
	source/graphics/render_block.c
	tests/test_fire_block_uv_flip.c
)
target_compile_definitions(test_fire_block_uv_flip PRIVATE PLATFORM_PC)
target_include_directories(test_fire_block_uv_flip PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_fire_block_uv_flip m)
add_test(NAME test_fire_block_uv_flip COMMAND test_fire_block_uv_flip)

# critique-1028 door: BlockDoor.setDoorRotation(getState(meta)) golden -- selection, collision
# and rendered slab for all 16 metadata x wood/iron (maintainer report: open/hinge-flipped door
# drew on the opposite edge from its click box). Same self-contained render_block.c link pattern
# as test_fire_block_uv_flip, plus the real block_door.c + aabb.c (stubs in the test file).
add_executable(test_door_bounds_golden
	source/graphics/render_block.c
	source/block/block_door.c
	source/block/aabb.c
	tests/test_door_bounds_golden.c
)
target_compile_definitions(test_door_bounds_golden PRIVATE PLATFORM_PC)
target_include_directories(test_door_bounds_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_door_bounds_golden m)
add_test(NAME test_door_bounds_golden COMMAND test_door_bounds_golden)

# M4 redstone: torch burnout golden -- >=8 lit->idle toggles within a 100-tick window block
# the relight (the static torchUpdates list); recovers after the window clears.
add_executable(test_redstone_torch_burnout_golden
	source/block/redstone_logic.c
	tests/test_redstone_torch_burnout_golden.c
)
target_link_libraries(test_redstone_torch_burnout_golden m)
target_compile_definitions(test_redstone_torch_burnout_golden PRIVATE PLATFORM_PC)
target_include_directories(test_redstone_torch_burnout_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_redstone_torch_burnout_golden COMMAND test_redstone_torch_burnout_golden)

# M4 redstone: wire neighbour-NOTIFY gate golden -- Beta only notifies components on a 0-crossing
# OR a drop to strength <=1 (the var9-reuse quirk: `var8==0 || var9==0`); >=2->>=2 dims notify
# nothing. Pins the gate the production flood was missing (it notified on every strength change).
add_executable(test_redstone_wire_notify_gate_golden
	source/block/redstone_logic.c
	tests/test_redstone_wire_notify_gate_golden.c
)
target_link_libraries(test_redstone_wire_notify_gate_golden m)
target_compile_definitions(test_redstone_wire_notify_gate_golden PRIVATE PLATFORM_PC)
target_include_directories(test_redstone_wire_notify_gate_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_redstone_wire_notify_gate_golden COMMAND test_redstone_wire_notify_gate_golden)

# M4 redstone: torch burnout SATURATION golden -- a full burnout list evicts the oldest entry
# instead of silently dropping a new record, so a real clock still reaches its >=8 burnout under
# a many-clock load (bounded-memory deviation; Beta's torchUpdates is unbounded).
add_executable(test_redstone_torch_burnout_saturation_golden
	source/block/redstone_logic.c
	tests/test_redstone_torch_burnout_saturation_golden.c
)
target_link_libraries(test_redstone_torch_burnout_saturation_golden m)
target_compile_definitions(test_redstone_torch_burnout_saturation_golden PRIVATE PLATFORM_PC)
target_include_directories(test_redstone_torch_burnout_saturation_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_redstone_torch_burnout_saturation_golden COMMAND test_redstone_torch_burnout_saturation_golden)

# M4 redstone: torch burnout RELOAD regression (R1) -- the process-static burnout list must be
# cleared at the world boundary, and the 100-tick prune must drop future-dated entries left by a
# session clock that reset to 0, so a torch at a reused (x,y,z) does not load pre-burnt (stuck dark).
add_executable(test_redstone_torch_burnout_reload_golden
	source/block/redstone_logic.c
	tests/test_redstone_torch_burnout_reload_golden.c
)
target_link_libraries(test_redstone_torch_burnout_reload_golden m)
target_compile_definitions(test_redstone_torch_burnout_reload_golden PRIVATE PLATFORM_PC)
target_include_directories(test_redstone_torch_burnout_reload_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_redstone_torch_burnout_reload_golden COMMAND test_redstone_torch_burnout_reload_golden)

# M4 redstone: wire neighbour-notify REACH (R3) -- Beta's blocksNeedingUpdate drain is a two-hop
# reach (25 distinct cells); a torch mounted on a solid block adjacent to the wire is armed only
# via the 2nd hop. Pins the pure redstone_wire_notify_targets enumerator the production rs_mark uses.
add_executable(test_redstone_wire_notify_reach_golden
	source/block/redstone_logic.c
	tests/test_redstone_wire_notify_reach_golden.c
)
target_link_libraries(test_redstone_wire_notify_reach_golden m)
target_compile_definitions(test_redstone_wire_notify_reach_golden PRIVATE PLATFORM_PC)
target_include_directories(test_redstone_wire_notify_reach_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_redstone_wire_notify_reach_golden COMMAND test_redstone_wire_notify_reach_golden)

add_executable(test_skylight_occlusion
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	source/worldgen/worldgen.c
				source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	source/lighting.c
	source/stack.c
	tests/test_skylight_occlusion.c
)
target_compile_definitions(test_skylight_occlusion PRIVATE PLATFORM_PC)
target_include_directories(test_skylight_occlusion PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_skylight_occlusion m)
add_test(NAME test_skylight_occlusion COMMAND test_skylight_occlusion)

# Faithfulness proof for the cheap terrain-only far skin (server_world_extract_far_skin):
# the single-chunk skin == the post-populate skin at every NON-tree/NON-lake column.
add_executable(test_far_skin_equiv
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	source/worldgen/worldgen.c
				source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	source/platform/time.c
	tests/test_far_skin_equiv.c
)
target_compile_definitions(test_far_skin_equiv PRIVATE PLATFORM_PC)
target_include_directories(test_far_skin_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_far_skin_equiv m)
add_test(NAME test_far_skin_equiv COMMAND test_far_skin_equiv)

# Nether far-ring roof exclusion (M6 render-distance fix): the far skin must skip the
# bedrock ceiling and skin the netherrack under it, not raise an occluding roof plane.
# Fully standalone (synthetic chunk, replicated predicate) -- no worldgen sources.
add_executable(test_far_skin_nether tests/test_far_skin_nether.c)
target_link_libraries(test_far_skin_nether m)
target_compile_definitions(test_far_skin_nether PRIVATE PLATFORM_PC)
target_include_directories(test_far_skin_nether PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_far_skin_nether COMMAND test_far_skin_nether)

# Distant-water-surface fix guard: the far-ring extract must report the WATER SURFACE
# (BLOCK_WATER_STILL) over ocean/lake columns, not the see-through-skipped seabed.
# Self-contained (own minimal block table + the mirrored per-column predicate).
add_executable(test_far_water_surface
	tests/test_far_water_surface.c
)
target_link_libraries(test_far_water_surface m)
target_compile_definitions(test_far_water_surface PRIVATE PLATFORM_PC)
target_include_directories(test_far_water_surface PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
add_test(NAME test_far_water_surface COMMAND test_far_water_surface)

# find_ocean_seed (tools/find_ocean_seed.c): a one-off worldgen tool to pin a
# deterministic ocean-shore seed for far-water captures. NOT wired as a CMake target:
# this object set hit an opaque ld crash in the local build env (ld returns 1/5 with no
# diagnostic, while test_far_skin_equiv with the identical worldgen sources links fine).
# The source is kept for a future build (clean env) or to run via debug.world_seed in
# the game; do not re-add the target until the linker quirk is understood.

# Byte-for-byte equivalence guard for the batched lighting flood (P1): one
# lighting_update_at_blocks == N x lighting_update_at_block. Pure unit -- links only
# the lighting engine + stack, with a local block table + flat grid harness.
add_executable(test_lighting_multi
	source/lighting.c
	source/stack.c
	tests/test_lighting_multi.c
)
target_compile_definitions(test_lighting_multi PRIVATE PLATFORM_PC)
target_include_directories(test_lighting_multi PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_lighting_multi m)
add_test(NAME test_lighting_multi COMMAND test_lighting_multi)

# Byte-for-byte guard for the client relight resume boundary: any slice size must
# converge to the same field as the old unbudgeted lighting_update_at_block path.
add_executable(test_lighting_client_budget_equiv
	source/lighting.c
	source/stack.c
	tests/test_lighting_client_budget_equiv.c
)
target_compile_definitions(test_lighting_client_budget_equiv PRIVATE PLATFORM_PC)
target_include_directories(test_lighting_client_budget_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_lighting_client_budget_equiv m)
add_test(NAME test_lighting_client_budget_equiv COMMAND test_lighting_client_budget_equiv)

# Production-driver guard for the Wii freeze fix: world_update_lighting itself must
# spend at most LIGHTING_CLIENT_POPS_PER_FRAME work per frame, including stale-drop
# backlog scans and active flood resumes through the real client world/chunk callbacks.
add_executable(test_lighting_client_driver_budget
	tests/test_lighting_client_driver_budget.c
)
target_link_libraries(test_lighting_client_driver_budget cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_lighting_client_driver_budget PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_lighting_client_driver_budget PROPERTIES C_STANDARD 99)
target_link_libraries(test_lighting_client_driver_budget
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_link_options(test_lighting_client_driver_budget PRIVATE "-Wl,--wrap=chunk_set_light")
add_test(NAME test_lighting_client_driver_budget
		 COMMAND test_lighting_client_driver_budget --golden
		 "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/lighting_client_r16.bin.gz")

# LIGHT-CLIENT-LOSSLESS (hw1023a Finding 2): world_update_lighting never loses work.
# A flood over the 500k ceiling, and every client queue grow failing (low memory),
# both end at an independent full recompute; an edge edit beside an absent column
# never relights against "darkness" (Beta doChunksNearChunkExist).
add_executable(test_lighting_client_lossless
	tests/test_lighting_client_lossless.c
)
target_link_libraries(test_lighting_client_lossless cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_lighting_client_lossless PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_lighting_client_lossless PROPERTIES C_STANDARD 99)
target_link_libraries(test_lighting_client_lossless
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_lighting_client_lossless COMMAND test_lighting_client_lossless)

# R17 remote-origin production ingress replay, frozen before R16 at 2f18d992b.
add_executable(test_lighting_remote_ingress_equiv
	tests/test_lighting_remote_ingress_equiv.c)
target_link_libraries(test_lighting_remote_ingress_equiv
	cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_lighting_remote_ingress_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_lighting_remote_ingress_equiv PROPERTIES C_STANDARD 99)
target_link_options(test_lighting_remote_ingress_equiv PRIVATE "-Wl,--wrap=chunk_set_light")
add_test(NAME test_lighting_remote_ingress_equiv
		 COMMAND test_lighting_remote_ingress_equiv --golden
		 "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/lighting_remote_r17.bin.gz")

# Far-ring keep-up probe (2026-09-03 reach-sweep gate 4): world_far_missing_count's
# annulus scan (receive via world_build_far_section / evict via world_unload_section,
# recentering, Chebyshev bounds) plus far_ring_keepup.h's pure missing_max/settle_ms
# running-max state machine (no world/network dependency for that half).
add_executable(test_far_ring_keepup
	tests/test_far_ring_keepup.c
)
target_link_libraries(test_far_ring_keepup cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_far_ring_keepup PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_far_ring_keepup PROPERTIES C_STANDARD 99)
target_link_libraries(test_far_ring_keepup
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_far_ring_keepup COMMAND test_far_ring_keepup)

# Real-Wii main-thread HANG guard: a corrupt/cyclic chunk chain must abandon the pass
# (world_ilist_overrun_count) instead of spinning forever in world_build_chunks, and the
# section-scan token budget must not underflow size_t. The m-lib node contracts are
# M_ASSERT and the shipping build compiles them out with -DNDEBUG; under those contracts
# the corruption is not even constructible, so this ONE target neutralises M_ASSERT to
# reproduce the shipping configuration. -UNDEBUG (applied to every test_ target below)
# still keeps the project's own assert() -- and therefore this test's checks -- live.
add_executable(test_world_chunk_walk_guard
	tests/test_world_chunk_walk_guard.c
)
# Narrowed 2026-08-27: the M_ASSERT-void seam applies to world.c (the TU
# whose m-lib walks this guard exercises) via cavex_test_world_massert_off
# plus this target's own TU below; the rest of the engine is the shared
# asserts-on test pool.
target_link_libraries(test_world_chunk_walk_guard cavex_test_core
	cavex_test_world_massert_off cavex_test_tickq cavex_test_input)
# Function-style, so it must go through compile OPTIONS: target_compile_definitions
# silently drops these (CMake warns "dropping a preprocessor definition"). The test's own
# `#ifndef M_ASSERT -> #error` guard fails the build if it ever stops arriving.
target_compile_options(test_world_chunk_walk_guard PRIVATE
	"-DM_ASSERT(...)=((void)0)")
target_include_directories(test_world_chunk_walk_guard PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_world_chunk_walk_guard PROPERTIES C_STANDARD 99)
target_link_libraries(test_world_chunk_walk_guard
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_world_chunk_walk_guard COMMAND test_world_chunk_walk_guard)

# Beta 1.7.3 biome classifier (temperature/humidity -> biome + surface blocks).
add_executable(test_biome
	source/worldgen/biome.c
	tests/test_biome.c
)
target_link_libraries(test_biome m)
target_include_directories(test_biome PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
add_test(NAME test_biome COMMAND test_biome)

# Beta 1.7.3 cave carver (MapGenCaves) + the MathHelper sin/cos lookup table.
add_executable(test_cave
	source/worldgen/java_random.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	tests/test_cave.c
)
target_include_directories(test_cave PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_cave m)
add_test(NAME test_cave COMMAND test_cave)

# Beta 1.7.3 ore vein (WorldGenMinable) -- the populate phase's reusable
# stone-replacing primitive (coal/iron/gold/redstone/diamond/lapis/dirt/gravel).
add_executable(test_minable
	source/worldgen/java_random.c
	source/worldgen/beta_math.c
	source/worldgen/minable.c
	tests/test_minable.c
)
target_include_directories(test_minable PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_minable m)
add_test(NAME test_minable COMMAND test_minable)

# Beta 1.7.3 ore-vein PASS (worldgen_ores) -- the eight stone-replacing veins
# in decorate order, driving worldgen_minable.
add_executable(test_ores
	source/worldgen/java_random.c
	source/worldgen/beta_math.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	tests/test_ores.c
)
target_include_directories(test_ores PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_ores m)
add_test(NAME test_ores COMMAND test_ores)

# Beta 1.7.3 WorldGenLakes (worldgen_lake) -- water/lava pond carving, the first
# visible decorate feature. Links populate.c (pulls in minable/beta_math via
# worldgen_ores).
add_executable(test_lake
	source/worldgen/java_random.c
	source/worldgen/beta_math.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	tests/test_lake.c
)
target_include_directories(test_lake PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_lake m)
add_test(NAME test_lake COMMAND test_lake)

# Beta 1.7.3 WorldGenClay (worldgen_clay) -- water-gated SAND->CLAY blob.
add_executable(test_clay
	source/worldgen/java_random.c
	source/worldgen/beta_math.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	tests/test_clay.c
)
target_include_directories(test_clay PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_clay m)
add_test(NAME test_clay COMMAND test_clay)

# Beta 1.7.3 WorldGenDungeons (worldgen_dungeon) -- cobble room + loot RNG +
# spawner.
add_executable(test_dungeon
	source/worldgen/java_random.c
	source/worldgen/beta_math.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	tests/test_dungeon.c
)
target_include_directories(test_dungeon PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_dungeon m)
add_test(NAME test_dungeon COMMAND test_dungeon)

# Beta 1.7.3 dungeon chest LOOT -- worldgen_dungeon materialises getDungeonLootItem
# rolls through wgen_world.set_chest; verifies faithful items + RNG/buffer-neutrality.
add_executable(test_dungeon_loot
	source/worldgen/java_random.c
	source/worldgen/beta_math.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	tests/test_dungeon_loot.c
)
target_include_directories(test_dungeon_loot PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_dungeon_loot m)
add_test(NAME test_dungeon_loot COMMAND test_dungeon_loot)

# Dungeon mob-spawner burst core (source/network/spawner.c) -- deterministic,
# headless: spot-check + Beta mob mapping + proves a floor-mounted spawner emits.
add_executable(test_spawner
	source/network/spawner.c
	source/network/spawner_mob_id.c
	tests/test_spawner.c
)
target_compile_definitions(test_spawner PRIVATE PLATFORM_PC)
target_include_directories(test_spawner PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_spawner m)
add_test(NAME test_spawner COMMAND test_spawner)

# FH-17: the runtime MobSpawner tile-entity store (source/network/
# server_spawner.c) against a real struct server_local layout -- proves the
# store actually drives spawn type (upsert -> lookup -> spawner_mob_from_id),
# plus overwrite-by-position/truncation/table-full/free lifecycle. Minimal
# link (spawner_mob_id.c + server_spawner.c only -- NOT spawner.c's burst
# core, which this test never calls and which would drag in block_is_normal_
# cube/rand_gen_* for nothing): these functions touch only server_local's
# spawner_te_pos[]/spawner_tes[] arrays, so no world/thread/region/block-
# registry machinery is needed.
add_executable(test_server_spawner
	source/network/spawner_mob_id.c
	source/network/server_spawner.c
	tests/test_server_spawner.c
)
target_compile_definitions(test_server_spawner PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_server_spawner PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_server_spawner m)
add_test(NAME test_server_spawner COMMAND test_server_spawner)

add_executable(test_spawner_capacity tests/test_spawner_capacity.c)
target_link_libraries(test_spawner_capacity cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
    ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_spawner_capacity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_spawner_capacity PROPERTIES C_STANDARD 99)
add_test(NAME test_spawner_capacity COMMAND test_spawner_capacity)
set_tests_properties(test_spawner_capacity PROPERTIES TIMEOUT 240 RESOURCE_LOCK cw_heavy)


# Chest/sign tile-entity NBT (de)serialization round-trip (Stage 1 of the
# chest-store redesign). Links the data layer plus cNBT, so the test can drive
# the *real* serializer (nbt_dump_binary -> nbt_parse), the exact path
# region_archive_set/get_blocks take -- not just the in-memory build/parse.
add_executable(test_tile_entities_nbt
	source/network/tile_entities_nbt.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_tile_entities_nbt.c

	source/item/item_nbt.c)
target_compile_definitions(test_tile_entities_nbt PRIVATE PLATFORM_PC
	CAVEWII_TILE_ENTITIES_NBT_TEST)
target_include_directories(test_tile_entities_nbt PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_tile_entities_nbt m ZLIB::ZLIB)
add_test(NAME test_tile_entities_nbt COMMAND test_tile_entities_nbt)

# Chest/sign table <-> per-chunk bridge (Stage 2 lifecycle helpers). Pure logic
# on a plain te_store view -- no server_local, no NBT, no region I/O -- so it
# unit-tests the block->chunk filter (incl. negatives), the overwrite-by-position
# de-dup, and free-on-evict directly.
add_executable(test_tile_entities_store
	source/block/piston_moving.c
	source/network/tile_entities_store.c
	source/network/tile_entities_nbt.c
	tests/test_tile_entities_store.c

	source/item/item_nbt.c
	source/cNBT/buffer.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
)
target_compile_definitions(test_tile_entities_store PRIVATE PLATFORM_PC
	CAVEWII_TILE_ENTITIES_STORE_TEST)
target_include_directories(test_tile_entities_store PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_tile_entities_store m)
add_test(NAME test_tile_entities_store COMMAND test_tile_entities_store)

# A1b: same-coordinate world tile contents and chunk save/unload isolation.
add_executable(test_world_tile_isolation
	source/block/piston_moving.c
	source/network/tile_entities_store.c
	source/network/tile_entities_nbt.c
	tests/test_world_tile_isolation.c
	source/item/item_nbt.c
	source/cNBT/buffer.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c)
target_compile_definitions(test_world_tile_isolation PRIVATE PLATFORM_PC)
target_include_directories(test_world_tile_isolation PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_world_tile_isolation m)
add_test(NAME test_world_tile_isolation COMMAND test_world_tile_isolation)

# IMP-TE-CAPS: growable tile-entity tables through the production view --
# growth past the old 64/256 keeps every row, row choice equals a large fixed
# table, a dense imported chunk inserts/gathers/releases whole, OOM refuses
# losslessly.
add_executable(test_te_pool_growth
	source/block/piston_moving.c
	source/network/tile_entities_store.c
	source/network/tile_entities_nbt.c
	tests/test_te_pool_growth.c
	source/item/item_nbt.c
	source/cNBT/buffer.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c)
target_compile_definitions(test_te_pool_growth PRIVATE PLATFORM_PC)
target_include_directories(test_te_pool_growth PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_te_pool_growth m)
add_test(NAME test_te_pool_growth COMMAND test_te_pool_growth)
# A1b GREEN: the test builds the production server_world_tile_store(w) view
# (owner-keyed shared tables); WILL_FAIL removed with the implementation.

# Beta 1.7.3 water-flow core (source/block/fluid_spread.c) -- pure logic over a
# mock voxel grid run to a fixed point: flat Manhattan spread (dies at 8),
# fall-down, dry-up, and infinite-source formation. No server_world.
add_executable(test_fluid_spread
	source/block/fluid_spread.c
	tests/test_fluid_spread.c
)
target_compile_definitions(test_fluid_spread PRIVATE PLATFORM_PC)
target_include_directories(test_fluid_spread PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_fluid_spread m)
add_test(NAME test_fluid_spread COMMAND test_fluid_spread)

# Full region-file round-trip (Stage 2): set_blocks -> real .mcr on disk + zlib
# -> get_blocks. The corruption check the design doc's risk note demands --
# asserts a chunk's TERRAIN survives a write that also carries a populated
# TileEntities list, byte for byte, not just that the chests round-trip.
add_executable(test_region_tile_entities
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_region_tile_entities.c

	source/item/item_nbt.c)
target_compile_definitions(test_region_tile_entities PRIVATE PLATFORM_PC
	CAVEWII_TILE_ENTITIES_NBT_TEST)
target_include_directories(test_region_tile_entities PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_region_tile_entities m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_region_tile_entities COMMAND test_region_tile_entities)

# Dynamic Level.Entities codec and real region-file round trip. Covers all
# server runtime entity kinds plus per-record malformed-NBT isolation.
add_executable(test_region_entities
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_region_entities.c

	source/item/item_nbt.c)
target_compile_definitions(test_region_entities PRIVATE PLATFORM_PC)
target_include_directories(test_region_entities PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_region_entities m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_region_entities COMMAND test_region_entities)

# IMP-ENTITY-PRESERVE: imported Beta paintings/minecarts/boats + mobs at
# non-float-exact double positions round-trip through the real region save
# (unchanged, one removed, one moved, unowned-tag preserving fallback).
add_executable(test_imported_entity_roundtrip
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	source/item/item_nbt.c
	tests/test_imported_entity_roundtrip.c)
target_compile_definitions(test_imported_entity_roundtrip PRIVATE PLATFORM_PC)
target_include_directories(test_imported_entity_roundtrip PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_imported_entity_roundtrip m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_imported_entity_roundtrip COMMAND test_imported_entity_roundtrip)

# lava-aabb-0924/H1: autosave entity storm -- the real region save
# (region_archive_set_chunk -> chunk_entities_preserve) over one chunk holding a
# storm of native item records (legacy no-Air, departed, one foreign record that
# defeats the codec-complete fast path), twice, under an allocator-hammer thread;
# plus the current==NULL fail-closed exit and its heartbeat-visible failure
# stage. Same standalone source set as test_region_entities + platform/time.c.
add_executable(test_autosave_entity_storm
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	source/item/item_nbt.c
	tests/test_autosave_entity_storm.c)
target_compile_definitions(test_autosave_entity_storm PRIVATE PLATFORM_PC)
target_include_directories(test_autosave_entity_storm PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_autosave_entity_storm m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_autosave_entity_storm COMMAND test_autosave_entity_storm)
# H2 review fix (11): the storm test asserts wall-clock budgets; never share the
# host with the rest of the suite.
set_tests_properties(test_autosave_entity_storm PROPERTIES RUN_SERIAL TRUE)

# lava-aabb-0924/H3: two entities at one exact position (an item sink, same-tick
# death drops) through the real region save: RED before H3 (stage entity_preserve
# / sub DUP_OWNER, the chunk never saved again, the reload showed the stale
# record), GREEN now that the owner match honours the TAG_LONG runtime id the
# codec has always written (and the legacy TAG_INT of older cards).
add_executable(test_entity_same_pos_save
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	source/item/item_nbt.c
	tests/test_entity_same_pos_save.c)
target_compile_definitions(test_entity_same_pos_save PRIVATE PLATFORM_PC)
target_include_directories(test_entity_same_pos_save PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_entity_same_pos_save m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_entity_same_pos_save COMMAND test_entity_same_pos_save)

# lava-aabb-0924/H2: chunk_entities_preserve owner-index equivalence -- the
# production O(n log n) merge vs the pre-H2 linear-scan oracle (one shared body,
# one swapped search) must emit identical bytes and identical fail sub-codes on
# randomized + adversarial input; WRITE_MAX timed through the production path.
add_executable(test_entity_preserve_equiv
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	source/item/item_nbt.c
	tests/test_entity_preserve_equiv.c)
target_compile_definitions(test_entity_preserve_equiv PRIVATE PLATFORM_PC
	CAVEWII_PRESERVE_TEST) # review fix (3): constant-hash collision switch
target_include_directories(test_entity_preserve_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_entity_preserve_equiv m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_entity_preserve_equiv COMMAND test_entity_preserve_equiv)

# java-load-1023: every Beta-named entity / tile-entity NBT tag has the exact
# Beta 1.7.3 tag type (a TAG_INT where Beta reads getShort crashed vanilla's
# Entity.readFromNBT and lost every entity); old int-typed CaveWii saves load.
add_executable(test_entity_nbt_beta_types
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	source/item/item_nbt.c
	tests/test_entity_nbt_beta_types.c)
target_compile_definitions(test_entity_nbt_beta_types PRIVATE PLATFORM_PC)
target_include_directories(test_entity_nbt_beta_types PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_entity_nbt_beta_types m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_entity_nbt_beta_types COMMAND test_entity_nbt_beta_types)

# save-enc-1030: chunk saves use Beta's own encoding -- LastUpdate = world
# time, empty lists typed 1, class tags only on their own mob class, double
# Pos/Motion -- and pre-save-enc-1030 native records stay deletable.
add_executable(test_save_beta_encoding
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	source/item/item_nbt.c
	tests/test_save_beta_encoding.c)
target_compile_definitions(test_save_beta_encoding PRIVATE PLATFORM_PC)
target_include_directories(test_save_beta_encoding PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_link_libraries(test_save_beta_encoding m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_save_beta_encoding COMMAND test_save_beta_encoding)

# Production lifecycle: gather -> region save -> eviction -> restore, then a
# process-style restart. Uses the full server/entity stack to verify fresh ids
# and arrow-owner remapping rather than only the archive-neutral records.
add_executable(test_entity_persistence_env
	tests/test_entity_persistence_env.c
)
target_link_libraries(test_entity_persistence_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_entity_persistence_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_entity_persistence_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_entity_persistence_env
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m
)
add_test(NAME test_entity_persistence_env COMMAND test_entity_persistence_env)

# McRegion per-chunk timestamp table (sector 1, bytes 4096..8191): fresh table
# is all-zero, every chunk write stamps big-endian epoch seconds at
# 4096+4*(rx+rz*32) exactly like Beta RegionFile.write, rewrites are monotonic,
# rollbacks keep the old stamp, and the stamp is post-commit best-effort -- an
# injected stamp failure leaves the chunk write committed (one-shot hooks via
# CAVEWII_REGION_ARCHIVE_TEST). Closes the 2026-08-07 save-forensics gap.
add_executable(test_region_timestamps
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_region_timestamps.c

	source/item/item_nbt.c)
target_compile_definitions(test_region_timestamps PRIVATE
	PLATFORM_PC CAVEWII_REGION_ARCHIVE_TEST)
target_include_directories(test_region_timestamps PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_region_timestamps
	m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_region_timestamps COMMAND test_region_timestamps)

# Region-file corruption validation and crash-safe copy-on-write commit. Test-
# only one-shot failure hooks exercise payload/index write and flush boundaries.
add_executable(test_region_archive_safety
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_region_archive_safety.c

	source/item/item_nbt.c)
target_compile_definitions(test_region_archive_safety PRIVATE
	PLATFORM_PC CAVEWII_REGION_ARCHIVE_TEST)
target_include_directories(test_region_archive_safety PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_region_archive_safety
	m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_region_archive_safety COMMAND test_region_archive_safety)

# SAVE BATCH equivalence + crash points (2026-09-07 far_walk pace regression):
# batched vs per-write header publication must store identical chunk records
# (and an identical container in the append-only case), no journal sidecar may
# survive, and every manufactured on-disk state around the header flip must
# recover to a COMPLETE old or new state. Same source set + test hooks as
# test_region_archive_safety above.
add_executable(test_region_index_journal_equiv
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_region_index_journal_equiv.c

	source/item/item_nbt.c)
target_compile_definitions(test_region_index_journal_equiv PRIVATE
	PLATFORM_PC CAVEWII_REGION_ARCHIVE_TEST)
target_include_directories(test_region_index_journal_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_region_index_journal_equiv
	m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_region_index_journal_equiv
		 COMMAND test_region_index_journal_equiv)

# T1 equivalence test for the region-record read-coalescing refactor (2026-08-24
# world-load-stall investigation): several chunks in one region, byte-for-byte
# decode identity through the coalesced reader, plus an exact fread-count pin
# (region_archive_io_stats) proving the read shape actually coalesced.
add_executable(test_region_multichunk_read_equiv
	source/network/entity_archive.c
	source/network/region_archive.c
	source/network/spawner_mob_id.c
	source/network/tile_entities_nbt.c
	source/platform/thread.c
	source/platform/time.c
	source/platform/mem2.c
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_region_multichunk_read_equiv.c

	source/item/item_nbt.c)
target_compile_definitions(test_region_multichunk_read_equiv PRIVATE
	PLATFORM_PC CAVEWII_REGION_ARCHIVE_TEST)
target_include_directories(test_region_multichunk_read_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_region_multichunk_read_equiv
	m ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_region_multichunk_read_equiv COMMAND test_region_multichunk_read_equiv)

# Cross-session region-boundary load regression (2026-06-12 persistence audit:
# load_chunk must open the OWNING region on demand, never regenerate a saved
# chunk). Drives the REAL server_world load path, so it builds the full engine
# the way soak_stream does (the only other target compiling server_world.c).
add_executable(test_region_boundary tests/test_region_boundary.c)
target_link_libraries(test_region_boundary cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_region_boundary PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_region_boundary PROPERTIES C_STANDARD 99)
target_link_libraries(test_region_boundary ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_region_boundary COMMAND test_region_boundary)

# S2 two-phase retirement (docs/p2_storage_ruling_fable_2026-09-09.md): an
# evicting save inside a batch retains its committed record until the helper
# publishes it; failure retries from the retained copy, a full ring refuses.
add_executable(test_region_evict_retain tests/test_region_evict_retain.c)
target_link_libraries(test_region_evict_retain cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_region_evict_retain PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_region_evict_retain PROPERTIES C_STANDARD 99)
target_link_libraries(test_region_evict_retain ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_region_evict_retain COMMAND test_region_evict_retain)

# T1 equivalence + regression pin for the 2026-09-01 region-probe-order fix:
# server_world_load_chunk must probe the region LRU (in-memory, zero I/O)
# BEFORE paying server_world_region_file_status's stat(2), not after. Drives
# the real load path like test_region_boundary; pins byte-identical loads
# AND an exact server_world_region_stat_count() delta (0 for a second chunk
# out of an already-resident region, vs 1 for the region's first load).
add_executable(test_region_load_stat_lru tests/test_region_load_stat_lru.c)
target_link_libraries(test_region_load_stat_lru cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_region_load_stat_lru PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_region_load_stat_lru PROPERTIES C_STANDARD 99)
target_link_libraries(test_region_load_stat_lru ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_region_load_stat_lru COMMAND test_region_load_stat_lru)

# Beta 1.7.3 map DATA layer (gap audit #10, mapdata-slice, part 1 of 2):
# MapData struct + its own "data/map_<id>.dat" save file, and
# ItemMap.updateMapData's pure scan math (center offsets, stripe gate, scale
# math, dimension guard, Nether column pick). No ItemMap/colour-palette/
# render -- those are part 2. See MAPDATA_SLICE_NOTES.md. Named "_golden"
# per CODESTYLE 4.1 (expected values hand-transcribed from the decompile;
# no full server harness) -- renamed from the originally-misregistered
# test_mapdata_env 2026-08-13 post-review. CAVEWII_MAP_DATA_ARCHIVE_TEST
# enables map_data_archive.c's test-only rename-failure fault injection.
add_executable(test_mapdata_golden tests/test_mapdata_golden.c)
target_link_libraries(test_mapdata_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mapdata_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mapdata_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_mapdata_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mapdata_golden COMMAND test_mapdata_golden)

# Beta 1.7.3 map item, part 2a (Batch F, lane 2): the "ItemMap" seam part 1
# (above) left -- crafting recipe, onCreated/onUpdate dispatch, the
# colour-scan core (histogram/majority-vote/water-shading/brightness/
# dither), the block->MapColor table, and idcounts.dat persistence. Full
# engine harness (world+player+inventory), so links ${_cavex_srcs} like
# test_region_boundary/test_sign_placement. See item_map.h/.c.
add_executable(test_map_item tests/test_map_item.c)
target_link_libraries(test_map_item cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_map_item PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_map_item PROPERTIES C_STANDARD 99)
target_link_libraries(test_map_item ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_map_item COMMAND test_map_item)
add_executable(test_map_painting_world_routing tests/test_map_painting_world_routing.c)
target_link_libraries(test_map_painting_world_routing cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_map_painting_world_routing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_map_painting_world_routing PROPERTIES C_STANDARD 99)
add_test(NAME test_map_painting_world_routing COMMAND test_map_painting_world_routing)
set_tests_properties(test_map_painting_world_routing PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)

# Beta 1.7.3 map item, part 2b (held-map render), display-conversion half
# ONLY: MapItemRenderer.renderMap's per-pixel colour-byte -> RGBA8888 fill
# loop (item_map_colors_to_rgba, item_map_render_convert.c). Pure function
# over raw byte buffers, but it calls item_map_color_rgb_for_index (part 2a,
# item_map.c) for the real production palette rather than a duplicated
# table, and item_map.c is only ever built as part of the full engine (its
# own onCreated/onUpdate dispatch pulls render_item.c, map_data_archive.c,
# map_idcounts.c, server_world.c transitively) -- checked, not assumed: it
# has no standalone-linkable form anywhere else in this tree either (see
# test_map_item/test_mapdata_golden/test_region_boundary, all ${_cavex_srcs}
# for the same reason). So this target follows that same established
# precedent rather than a genuinely minimal link list. item_map_render_
# convert.c itself is registered in the main `cavex` source list above
# (next to item_map.c), so it is already part of ${_cavex_srcs} here.
add_executable(test_item_map_render_convert tests/test_item_map_render_convert.c)
target_link_libraries(test_item_map_render_convert cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_item_map_render_convert PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_item_map_render_convert PROPERTIES C_STANDARD 99)
target_link_libraries(test_item_map_render_convert ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_item_map_render_convert COMMAND test_item_map_render_convert)

# Batch-0 save-integrity fixture (2026-08-07 hardware-video "circular track
# disappeared" caveat): a player-built rail ring spanning the x=0 region
# boundary and a z chunk boundary must round-trip byte-exactly through
# place -> evict -> reload -> edit -> cold-restart -> re-evict, all via the
# production evictor/save/load paths, superflat generator armed so a
# regression fabricates (and is caught) rather than failing to load.
add_executable(test_rail_ring_persistence tests/test_rail_ring_persistence.c)
target_link_libraries(test_rail_ring_persistence cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_rail_ring_persistence PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_rail_ring_persistence PROPERTIES C_STANDARD 99)
target_link_libraries(test_rail_ring_persistence ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_rail_ring_persistence COMMAND test_rail_ring_persistence)

# Rail onNeighbourBlockChange support-drop (was entirely missing): a rail whose
# supporting block (below, or the ascending side for a slope 2-5) is removed
# must drop as an item and become air. Pins the below-only + ascending-side
# metadata map (incl. a wrong-side negative control) and the isPowered shape
# mask (rail_shape) for a powered rail carrying its power bit.
add_executable(test_rail_support_drop_env tests/test_rail_support_drop_env.c)
target_link_libraries(test_rail_support_drop_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_rail_support_drop_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_rail_support_drop_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_rail_support_drop_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_rail_support_drop_env COMMAND test_rail_support_drop_env)

# Batch 1b (2026-08-07 caveats): Nether-native mobs -- the dimension-split
# hostile spawn table (BiomeGenHell ghast/pig-zombie 10/10; the video's
# Nether-skeleton bug), pig zombie neutrality/group-anger/Anger persistence,
# ghast flight + fireball attack cycle, fireball explosion + punch-deflection.
add_executable(test_nether_mobs_env tests/test_nether_mobs_env.c)
target_link_libraries(test_nether_mobs_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_nether_mobs_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_nether_mobs_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_nether_mobs_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_nether_mobs_env COMMAND test_nether_mobs_env)

# Ghast spawn-channel fix (2026-09-01): the independent GHAST_SPAWN_CAP
# channel in server_local_spawn_mobs (docs/deviations.md "Nether ghast
# sightings starved by the Wii hostile cap"). S1 proves overworld equivalence
# (capped pass never touches rand_src -- Nether-only gate); S2/S3 measure
# ghast presence + no-channel-bleed over many fresh Nether populations on the
# real natural-spawn path (server_local_test_spawn_mobs), real worldgen_hell
# terrain.
add_executable(test_ghast_spawn_env tests/test_ghast_spawn_env.c)
target_link_libraries(test_ghast_spawn_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_ghast_spawn_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_ghast_spawn_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_ghast_spawn_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_ghast_spawn_env COMMAND test_ghast_spawn_env)
# P1 NETHER-LAG (critique 10-28): per-column streaming cost, Nether vs overworld,
# on the real load_chunk (generate + populate) and light-drain paths.
add_executable(test_nether_stream_cost tests/test_nether_stream_cost.c)
target_link_libraries(test_nether_stream_cost cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_nether_stream_cost PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_nether_stream_cost PROPERTIES C_STANDARD 99)
target_link_libraries(test_nether_stream_cost ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_nether_stream_cost COMMAND test_nether_stream_cost)
# nether-drain-1030 (T1): the shipping Beta light relax settles the same
# streamed Nether/overworld ring (light arrays, queue peak, relax census) as
# the frozen reference relax.
add_executable(test_nether_light_hash tests/test_nether_light_hash.c)
target_link_libraries(test_nether_light_hash cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_nether_light_hash PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_nether_light_hash PROPERTIES C_STANDARD 99)
target_link_libraries(test_nether_light_hash ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_nether_light_hash COMMAND test_nether_light_hash)
# world-reload-1030 (Bucket A): a world built, destroyed and built again in
# one process settles to byte-identical blocks and light.
add_executable(test_world_reload_light_determinism tests/test_world_reload_light_determinism.c)
target_link_libraries(test_world_reload_light_determinism cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_world_reload_light_determinism PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_world_reload_light_determinism PROPERTIES C_STANDARD 99)
target_link_libraries(test_world_reload_light_determinism ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_world_reload_light_determinism COMMAND test_world_reload_light_determinism)
# I03 (board #8): spawn eligibility on the candidate own Random
add_executable(test_spawn_candidate_rng tests/test_spawn_candidate_rng.c)
target_link_libraries(test_spawn_candidate_rng cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_spawn_candidate_rng PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_spawn_candidate_rng PROPERTIES C_STANDARD 99)
target_link_libraries(test_spawn_candidate_rng ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_spawn_candidate_rng COMMAND test_spawn_candidate_rng)

# 2026-08-08 hardware-video caveat: the spider frozen on snow. Pins the pair of
# fixes -- the Beta EntityLiving.getCanSpawnHere box clearance on every natural
# spawn (block/liquid/entity clauses, strict intersectsWith) and the Beta
# calculateOffset embedded-box exclusion in entity_intersection_threshold
# (an embedded mob slides out instead of freezing forever) -- plus goldens
# proving the ordinary outside-approach clipping is byte-unchanged.
add_executable(test_mob_spawn_clearance tests/test_mob_spawn_clearance.c)
target_link_libraries(test_mob_spawn_clearance cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mob_spawn_clearance PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mob_spawn_clearance PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_spawn_clearance ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mob_spawn_clearance COMMAND test_mob_spawn_clearance)

# 2026-08-08 hardware-video caveat: the in-bed camera. Pins camera_attach's
# Beta orientCamera sleep lock -- four-direction pillow->foot yaw, pitch
# levelled, pointer-immunity while asleep, pre-sleep look restored on wake,
# and the recorded bed-destroyed micro-delta.
# F8: the local player's Beta blockExists gate -- entity_tick must do nothing
# at all while the column under the body is not resident, then resume ordinary
# gravity the tick after it installs. Same harness shape as test_bed_camera
# (real entity_local_player.c over a real client world).
add_executable(test_local_player_column_guard tests/test_local_player_column_guard.c)
target_link_libraries(test_local_player_column_guard cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_local_player_column_guard PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_local_player_column_guard PROPERTIES C_STANDARD 99)
target_link_libraries(test_local_player_column_guard ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_local_player_column_guard COMMAND test_local_player_column_guard)

add_executable(test_bed_camera tests/test_bed_camera.c)
target_link_libraries(test_bed_camera cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_bed_camera PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_bed_camera PROPERTIES C_STANDARD 99)
target_link_libraries(test_bed_camera ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_bed_camera COMMAND test_bed_camera)

# Region-handle durability: a sync-failed LRU victim remains retryable, a lost
# close-failed handle keeps every later barrier fail-closed, and a brand-new
# throwaway archive must sync before its chunk becomes clean. Needs the one-shot
# fail injection (CAVEWII_REGION_ARCHIVE_TEST) and the full server_world harness.
# P03: capacity-driven new fixture; preserve the original four-slot source.
add_executable(test_region_evict_failure tests/test_region_evict_failure_capacity.c)
target_link_libraries(test_region_evict_failure cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_region_evict_failure PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_region_evict_failure PROPERTIES C_STANDARD 99)
target_link_libraries(test_region_evict_failure ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_region_evict_failure COMMAND test_region_evict_failure)

# M4 redstone: E2E chunk-load guard -- saved repeater/torch self-schedulers must
# NOT receive a blanket reload tick, while fire is the scheduled-only positive
# control. Drives incremental 3x3 residency, capacity retry, region save/load,
# and the reconstructed queue without set_block/client RPC setup.
add_executable(test_redstone_repeater_reload_env tests/test_redstone_repeater_reload_env.c)
target_link_libraries(test_redstone_repeater_reload_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_repeater_reload_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_repeater_reload_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_repeater_reload_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_repeater_reload_env COMMAND test_redstone_repeater_reload_env)

# Nether lava-spring scheduling guard (M6): the flowing lava spring WorldGenHellLava
# drops under the roof must be armed by schedule_chunk_ticks (so it falls/flows), while
# the still-lava sea stays skipped. Same cross-session production-load harness as the
# repeater-reload guard above, on the Nether DIM-1 region path.
add_executable(test_nether_lava_schedule_env tests/test_nether_lava_schedule_env.c)
target_link_libraries(test_nether_lava_schedule_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_nether_lava_schedule_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_nether_lava_schedule_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_nether_lava_schedule_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_nether_lava_schedule_env COMMAND test_nether_lava_schedule_env)

# Beta fluid lifecycle adapter golden: drives real set_block/onBlockAdded/
# neighbour notify/scheduled queue + entity drops. Pure test_fluid_spread pins
# the algorithm; this target pins production predicate and lifecycle wiring.
add_executable(test_fluid_lifecycle_env tests/test_fluid_lifecycle_env.c)
target_link_libraries(test_fluid_lifecycle_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_fluid_lifecycle_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_fluid_lifecycle_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_fluid_lifecycle_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_fluid_lifecycle_env COMMAND test_fluid_lifecycle_env)

# Far-ring slot-pool backpressure: with every far slot held, one production server
# tick must RETURN (defer the batch) instead of parking in the slot acquire -- the
# server half of the two-way bounded-buffer deadlock. Runs the tick on its own
# thread and waits with a bound so a regression asserts instead of hanging ctest.
add_executable(test_far_slot_deadlock_env tests/test_far_slot_deadlock_env.c)
target_link_libraries(test_far_slot_deadlock_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_far_slot_deadlock_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_far_slot_deadlock_env PROPERTIES C_STANDARD 11)
target_link_libraries(test_far_slot_deadlock_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_far_slot_deadlock_env COMMAND test_far_slot_deadlock_env)

# W1-c (far-reach program Wave 1, docs/far_reach_program_2026-09-03.md
# "Server-side scan cost"): golden order-equivalence proof + iteration-count
# measurement for the cached far-pick frontier vs the old full-rescan loop.
add_executable(test_far_pick_frontier_golden tests/test_far_pick_frontier_golden.c)
target_link_libraries(test_far_pick_frontier_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_far_pick_frontier_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_far_pick_frontier_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_far_pick_frontier_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_far_pick_frontier_golden COMMAND test_far_pick_frontier_golden)

# T2 far-send share (2026-09-04, lane/far-send-share): the near-owed share gate,
# the leftover-budget rule, and the promotion-debt guard in server_local.c's far
# send block. Same link shape as test_far_pick_frontier_golden above -- it drives
# the production far-pick frontier too.
add_executable(test_far_send_share tests/test_far_send_share.c)
target_link_libraries(test_far_send_share cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_far_send_share PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_far_send_share PROPERTIES C_STANDARD 99)
target_link_libraries(test_far_send_share ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_far_send_share COMMAND test_far_send_share)

# M4 redstone: E2E wire-driven repeater -- drives a real torch->wire->repeater circuit through
# the production wire flood + scheduled queue (a drain thread spins clin_drain_discard so the
# server-thread set_block RPCs never block). Guards the critical scheduling fixes: the wire-driven
# delay must be the metadata delay (rises at +4, not the collapsed +1) and a stable idle repeater
# must not spuriously toggle. The first server_world-linked test that drives server_world_set_block.
add_executable(test_redstone_repeater_circuit_env tests/test_redstone_repeater_circuit_env.c)
target_link_libraries(test_redstone_repeater_circuit_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_repeater_circuit_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_repeater_circuit_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_repeater_circuit_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_repeater_circuit_env COMMAND test_redstone_repeater_circuit_env)

# Faithfulness golden: the redstone-torch inverter across three wire topologies (A continuous->OFF,
# B repeater+wire->LIT [verified faithful vs the 740c583 decompile + the mc173 oracle, 2026-06-19],
# B' repeater-direct->OFF). Locks the connection-aware wire power model so a future "fix" of topology B
# (which is correct Beta, NOT a bug) cannot silently break the continuous-wire NOT-gate or the piston query.
add_executable(test_redstone_torch_inverter_topologies tests/test_redstone_torch_inverter_topologies.c)
target_link_libraries(test_redstone_torch_inverter_topologies cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_torch_inverter_topologies PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_torch_inverter_topologies PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_torch_inverter_topologies ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_torch_inverter_topologies COMMAND test_redstone_torch_inverter_topologies)

add_executable(test_redstone_torch_support_drop_env tests/test_redstone_torch_support_drop_env.c)
target_link_libraries(test_redstone_torch_support_drop_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_torch_support_drop_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_torch_support_drop_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_torch_support_drop_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_torch_support_drop_env COMMAND test_redstone_torch_support_drop_env)
# PAR-WIRE-POP: BlockRedstoneWire.onNeighborBlockChange support-drop (dust pops as an item when the block below is mined).
add_executable(test_redstone_wire_support_drop_env tests/test_redstone_wire_support_drop_env.c)
target_link_libraries(test_redstone_wire_support_drop_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_wire_support_drop_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_wire_support_drop_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_wire_support_drop_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_wire_support_drop_env COMMAND test_redstone_wire_support_drop_env)
add_executable(test_redstone_wire_removal_order_golden tests/test_redstone_wire_removal_order_golden.c)
target_link_libraries(test_redstone_wire_removal_order_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_wire_removal_order_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_wire_removal_order_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_wire_removal_order_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_wire_removal_order_golden COMMAND test_redstone_wire_removal_order_golden)

# next-gaps-0924/G5 (Astra route c): LOSSLESS redstone propagation in the originating tick -- the
# uncapped deferred-notify set (255/256/257/... marks, rise + fall), deep support-removal cascades
# past the old depth-12 skip, worklist saturation, and allocation-failure suspend/resume; compares
# every wire against the Beta fixpoint and the torch/repeater/piston timeline per tick.
add_executable(test_redstone_lossless_golden tests/test_redstone_lossless_golden.c)
target_link_libraries(test_redstone_lossless_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_lossless_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_lossless_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_lossless_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_lossless_golden COMMAND test_redstone_lossless_golden)

# FH-03: BlockLadder.onNeighborBlockChange support-drop -- a ladder whose mounting wall cell
# is removed must pop (block -> air, item dropped), per-metadata (2/3/4/5 = +Z/-Z/+X/-X).
add_executable(test_ladder_pop tests/test_ladder_pop.c)
target_link_libraries(test_ladder_pop cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_ladder_pop PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_ladder_pop PROPERTIES C_STANDARD 99)
target_link_libraries(test_ladder_pop ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_ladder_pop COMMAND test_ladder_pop)

# Beta 1.7.3 BlockFlower.canBlockStay self-removal, inherited by BlockSapling
# (block_sapling.c): support-loss and darkness both pop a sapling as a dropped
# item, on BOTH the random-tick path (BlockSapling.updateTick's super call)
# and the neighbour-change path -- a valid support+light sapling stays put on
# either. Same live server_world + dict_entity_size rig as test_ladder_pop.
add_executable(test_sapling_canblockstay tests/test_sapling_canblockstay.c)
target_link_libraries(test_sapling_canblockstay cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_sapling_canblockstay PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_sapling_canblockstay PROPERTIES C_STANDARD 99)
target_link_libraries(test_sapling_canblockstay ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_sapling_canblockstay COMMAND test_sapling_canblockstay)

# Beta 1.7.3 BlockSnow.canSnowStay/canPlaceBlockAt support-loss removal
# (block_snow.c had no .onNeighbourBlockChange at all -- a snow layer whose
# support was destroyed kept floating in mid-air). Support-loss pop is SILENT
# (BlockSnow.quantityDropped() == 0), unlike a player-mined layer. Same live
# server_world + dict_entity_size rig as test_sapling_canblockstay.
add_executable(test_snow_canblockstay tests/test_snow_canblockstay.c)
target_link_libraries(test_snow_canblockstay cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_snow_canblockstay PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_snow_canblockstay PROPERTIES C_STANDARD 99)
target_link_libraries(test_snow_canblockstay ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_snow_canblockstay COMMAND test_snow_canblockstay)

# FH-07: Beta's BlockLockedChest back at id 95 (identity, auto-facing, updateTick
# self-despawn) and the NET-NEW iron chest relocated to 200 -- both registry sides.
add_executable(test_locked_chest tests/test_locked_chest.c)
target_link_libraries(test_locked_chest cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_locked_chest PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_locked_chest PROPERTIES C_STANDARD 99)
target_link_libraries(test_locked_chest ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_locked_chest COMMAND test_locked_chest)

# FH-07 save version 3: iron chest 95 -> 200 region migration -- per-instance
# Chest-TE provenance proof, CaveWii/Beta-import postures, two-pass item-id
# evidence escalation, idempotency.
add_executable(test_iron_chest_migrate tests/test_iron_chest_migrate.c)
target_link_libraries(test_iron_chest_migrate cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_iron_chest_migrate PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_iron_chest_migrate PROPERTIES C_STANDARD 99)
target_link_libraries(test_iron_chest_migrate ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_iron_chest_migrate COMMAND test_iron_chest_migrate)

# FH-17 save version 4: dungeon mob spawner metadata -> MobSpawner tile entity
# region migration -- per-instance TE provenance proof (unconditional, no
# CaveWii/Beta-import posture split needed, see region_archive.h), idempotency.
add_executable(test_mob_spawner_migrate tests/test_mob_spawner_migrate.c)
target_link_libraries(test_mob_spawner_migrate cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mob_spawner_migrate PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mob_spawner_migrate PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_spawner_migrate ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mob_spawner_migrate COMMAND test_mob_spawner_migrate)

# PAR-BED-META: save version 5 converts legacy CaveWii bed metadata
# (index<<1 over {+Z,-Z,+X,-X}) to Beta's meta&3/0x4/0x8 layout once, only for
# CaveWii-created worlds; marker-guarded crash retry; Beta imports untouched.
add_executable(test_bed_meta_migrate tests/test_bed_meta_migrate.c)
target_link_libraries(test_bed_meta_migrate cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_bed_meta_migrate PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_bed_meta_migrate PROPERTIES C_STANDARD 99)
target_link_libraries(test_bed_meta_migrate ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_bed_meta_migrate COMMAND test_bed_meta_migrate)

# IMP-MIGRATE-FAST (T1): the save-version chain fed by one read-only file-order
# survey must leave every world byte-identical to the staged three-pass chain
# (Beta import, legacy CaveWii, mixed/refused, crash-mid-stage, version-3,
# corrupt-record fixtures; optional private real worlds via
# CAVEWII_IMPORT_EQUIV_WORLDS), with identical per-stage stats.
add_executable(test_import_migrate_fast_equiv tests/test_import_migrate_fast_equiv.c)
target_link_libraries(test_import_migrate_fast_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_import_migrate_fast_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_import_migrate_fast_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_import_migrate_fast_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_import_migrate_fast_equiv COMMAND test_import_migrate_fast_equiv)

# ui-build-1026/T09a: bounded local convert-only lifecycle; no world entry.
add_executable(test_convert_only tests/test_convert_only.c)
target_link_libraries(test_convert_only cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_convert_only PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_convert_only PROPERTIES C_STANDARD 99)
target_link_libraries(test_convert_only ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_convert_only COMMAND test_convert_only)

# dropin-1029: real Beta worlds load drop-in (no convert tag), the save-version chain writes
# level.dat ONCE (Beta's level.dat_old survives), region bytes identical, and Beta wall signs
# 4/5 accepted (ItemSign/BlockSign 2..5). Extra real worlds via CW_DROPIN_EXTRA_WORLDS.
add_executable(test_dropin_beta_import tests/test_dropin_beta_import.c)
target_link_libraries(test_dropin_beta_import cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_dropin_beta_import PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_dropin_beta_import PROPERTIES C_STANDARD 99)
target_link_libraries(test_dropin_beta_import ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_dropin_beta_import COMMAND test_dropin_beta_import
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# hw1029c/HEAP: a real Beta 1.7.3 world (multi-MB region files) loads within the
# malloc-heap range of a CavEX-written one. The test wraps malloc/calloc/realloc/
# free (ld --wrap) to charge every engine allocation to its call site and asserts
# the high-water. CW_DROPIN_HEAP_WORLD=<dir> (or argv[1]) runs a larger real world.
add_executable(test_dropin_heap_budget tests/test_dropin_heap_budget.c)
target_link_libraries(test_dropin_heap_budget cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_dropin_heap_budget PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_dropin_heap_budget PROPERTIES C_STANDARD 99)
target_link_options(test_dropin_heap_budget PRIVATE
	-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free)
target_link_libraries(test_dropin_heap_budget ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_dropin_heap_budget COMMAND test_dropin_heap_budget
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# hw1029c (data loss): a Beta map with no Player tag keeps its spawn chunk's
# tile entities -- the load's spawn probe runs after the world-boundary TE /
# entity resets, so the chests/signs/furnaces it loads stay live and the unload
# checkpoint saves them (unknown tags verbatim) instead of refusing the chunk.
add_executable(test_dropin_spawn_chunk_te tests/test_dropin_spawn_chunk_te.c)
target_link_libraries(test_dropin_spawn_chunk_te cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_dropin_spawn_chunk_te PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_dropin_spawn_chunk_te PROPERTIES C_STANDARD 99)
add_test(NAME test_dropin_spawn_chunk_te COMMAND test_dropin_spawn_chunk_te)
set_tests_properties(test_dropin_spawn_chunk_te PROPERTIES TIMEOUT 240)

# hw1029c (a): loading a chunk never changes a furnace's block id (Beta
# TileEntityFurnace.readFromNBT only recomputes currentItemBurnTime); a furnace
# saved lit with BurnTime 0 stays lit, mid-burn keeps burning, idle stays idle.
add_executable(test_furnace_load_beta tests/test_furnace_load_beta.c)
target_link_libraries(test_furnace_load_beta cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_furnace_load_beta PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_furnace_load_beta PROPERTIES C_STANDARD 99)
target_link_libraries(test_furnace_load_beta ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_furnace_load_beta COMMAND test_furnace_load_beta)

# PLAYER-HEALTH-NBT (beta-trial-1025): level.dat Player.Health is Beta
# half-hearts (EntityLiving.java:558/565); imports, pre-fix CaveWii saves and
# the exact CaveWiiHealth round trip through the production chain.
add_executable(test_player_nbt_beta_types tests/test_player_nbt_beta_types.c)
target_link_libraries(test_player_nbt_beta_types cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_player_nbt_beta_types PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_player_nbt_beta_types PROPERTIES C_STANDARD 99)
target_link_libraries(test_player_nbt_beta_types ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_player_nbt_beta_types COMMAND test_player_nbt_beta_types)

# convert-notice-1024 (maintainer Option B): the one-time "this world will be
# converted" notice on world select for a Minecraft Beta world -- predicate,
# the three approved lines, Back safe/highlighted and writes nothing, Continue
# loads once, CavEX/stamped worlds and autoload skip it, and the shared
# two-line confirm layout unchanged.
add_executable(test_import_convert_notice tests/test_import_convert_notice.c)
target_link_libraries(test_import_convert_notice cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_import_convert_notice PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_import_convert_notice PROPERTIES C_STANDARD 99)
target_link_libraries(test_import_convert_notice ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_import_convert_notice COMMAND test_import_convert_notice)

# FH-08: zombie spawn egg relocated off Beta's item-id space (360 -> the
# NET-NEW 500) -- registry, foreign-360-safety, identity, and functional
# (onItemPlace still spawns exactly one MONSTER_ZOMBIE) coverage. No save
# migration test needed: the item was never obtainable in any released build,
# so no legacy save can hold one under the old id (see docs/deviations.md).
add_executable(test_egg_zombie_item tests/test_egg_zombie_item.c)
target_link_libraries(test_egg_zombie_item cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_egg_zombie_item PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_egg_zombie_item PROPERTIES C_STANDARD 99)
target_link_libraries(test_egg_zombie_item ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_egg_zombie_item COMMAND test_egg_zombie_item)

# FH-01 RESOLVED AS ALREADY-CORRECT-EXCEPT-best (docs/fork_hack_audit_2026-08-21.md):
# Beta [740c583 Block.blockStrength:327-333] digs redstone ore at hardness/30 for
# iron/diamond (canHarvestBlock, NO per-tier bonus -- oreRedstone is omitted from
# ItemPickaxe.blocksEffectiveAgainst, flat 1.0F) and hardness/100 for hand/wood/
# stone/gold. Port encoding: .digging.min=IRON (drop+harvest gate, was already
# right), .best=ANY (caps qualifying tiers to divider 1; the inherited best=IRON
# wrongly gave iron/diamond a 6x bonus -- the old //TODO: too fast was accurate).
# Also pins diamond ore's tier-dependent speed + drop gate as a regression guard.
add_executable(test_redstone_ore_dig tests/test_redstone_ore_dig.c)
target_link_libraries(test_redstone_ore_dig cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_ore_dig PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_ore_dig PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_ore_dig ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_ore_dig COMMAND test_redstone_ore_dig
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Field fix (2026-09-01, ice dig speed + harvest water conversion): Material.ice
# never calls setNoHarvest() [740c583: Material.java], so canHarvestBlock(ice) is
# ALWAYS true and Block.blockStrength always takes the fast /30 branch -- the old
# `.digging.min = TOOL_TIER_WOOD` wrongly routed bare-hand mining through the /100
# "wrong tool" penalty branch (2500ms/50 ticks instead of Beta's 750ms/15 ticks).
# `.min = TOOL_TIER_ANY` (this codebase's existing hand-harvestable-but-tool-
# effective convention, block_dirt.c/block_log.c/block_grass.c/block_sand.c) fixes
# it while keeping the real pickaxe speed bonus (ice IS an ItemPickaxe.
# blocksEffectiveAgainst member). Also adds BlockIce.harvestBlock's water
# conversion (block below solid or liquid -> flowing water), wired as a direct
# call from the SRPC_BLOCK_DIG player-dig path (block_ice_harvest,
# server_local.c) rather than through block->onBreak, which also fires from
# TNT explosions and piston/support-loss drop paths that Beta's harvestBlock
# never touches.
add_executable(test_ice_dig tests/test_ice_dig.c)
target_link_libraries(test_ice_dig cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_ice_dig PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_ice_dig PROPERTIES C_STANDARD 99)
target_link_libraries(test_ice_dig ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_ice_dig COMMAND test_ice_dig
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Batch H (docs/parity_audit_2026-08-25.md, items 1-3), 2026-08-25: obsidian
# hardness 10000->15000 + .best MAX->ANY (was ~12x too fast even with the
# required diamond pickaxe); the 9-block .best WOOD/MAX->ANY sweep (furnace
# x2/dispenser/spawner/iron door/bricks/stone plate/workbench/glowstone, each
# omitted from its tool's blocksEffectiveAgainst array); wooden pressure
# plate's tool-free flat harvest (Material.wood is never gated by
# setNoHarvest). Speed matrix is a pure tool_dig_delay_ms check (mirrors
# test_redstone_ore_dig.c); one live-server drop-gate check closes item 3.
add_executable(test_dig_speed_batch_h tests/test_dig_speed_batch_h.c)
target_link_libraries(test_dig_speed_batch_h cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_dig_speed_batch_h PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_dig_speed_batch_h PROPERTIES C_STANDARD 99)
target_link_libraries(test_dig_speed_batch_h ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_dig_speed_batch_h COMMAND test_dig_speed_batch_h
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# push-0921/B10: Beta 1.7.3's two ENVIRONMENTAL dig-speed penalties
# (EntityPlayer.getCurrentPlayerStrVsBlock: strength /= 5.0F inside water
# material, /= 5.0F again while !onGround) -- a 5x/25x simulation gap that the
# scripted MP run exposed as mc173's "incoherent break time" refusals. Pure
# tool_dig_delay_env_ms goldens over {block x tool} x {ground/air} x
# {dry/water}, hand-derived from the decompile, plus the mc173 acceptance-gate
# inequality; no server, no world.
add_executable(test_dig_speed_parity tests/test_dig_speed_parity.c)
target_link_libraries(test_dig_speed_parity cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_dig_speed_parity PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_dig_speed_parity PROPERTIES C_STANDARD 99)
target_link_libraries(test_dig_speed_parity ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_dig_speed_parity COMMAND test_dig_speed_parity
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# push-0921/B18 (review B17 H1): the ten-stage crack overlay must age on the
# dig's OWN clock. render_block.c recomputed its bucket width from the
# two-argument (dry, on-ground) delay while the dig moved to
# tool_dig_delay_env_ms, so a submerged or airborne dig sat fully cracked for
# most of its length. The rule now lives once, in tool_dig_crack_stage();
# this pins overlay/dig agreement across the four environments and all three
# tool arms (including the cannot-harvest arm, which takes NO penalty).
add_executable(test_dig_crack_overlay tests/test_dig_crack_overlay.c)
target_link_libraries(test_dig_crack_overlay cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_dig_crack_overlay PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_dig_crack_overlay PROPERTIES C_STANDARD 99)
target_link_libraries(test_dig_crack_overlay ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_dig_crack_overlay COMMAND test_dig_crack_overlay
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# parity-render2-0924/G1: the crack overlay on an alpha-tested face (glass).
# render_block_cracks_owned drew with depth EQUAL, so cracks survived only where
# the face itself wrote depth -- never on a glass pane's see-through texels
# (push-0921/h1-capture runs 2-4: a full submerged glass dig, zero cracks).
# Beta's drawBlockBreaking keeps LEQUAL and pulls the crack toward the eye with
# glPolygonOffset(-3,-3); this engine's cross-platform equivalent is the
# gfx_depth_range remap. Links the real render_block.c against a recording gfx
# sink (the test_redstone_render_count link pattern) and a 24-bit depth-test
# oracle: passes on the face's own depth, on a farther stored depth (the RED
# case), never through a nearer wall, bias toward the eye and bounded, every
# state restored, opaque blocks unchanged.
add_executable(test_crack_overlay_depth
	source/graphics/render_block.c
	tests/test_crack_overlay_depth.c
)
target_compile_definitions(test_crack_overlay_depth PRIVATE PLATFORM_PC)
target_include_directories(test_crack_overlay_depth PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_crack_overlay_depth m)
add_test(NAME test_crack_overlay_depth COMMAND test_crack_overlay_depth)

# parity_audit_2026-08-25 lower-tier trio, decompile-verified before fixing:
# gold ore harvest gate STONE->IRON (ItemPickaxe.canHarvestBlock level>=2),
# Block of Iron IRON->STONE (level>=1), farmland shovel class
# (ItemSpade.blocksEffectiveAgainst includes tilledField). Beta-derived
# tick-count matrix per tool tier + live-server drop-gate checks (a stone
# pickaxe must break-but-not-drop gold ore, must harvest the iron block).
add_executable(test_dig_tier_gates tests/test_dig_tier_gates.c)
target_link_libraries(test_dig_tier_gates cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_dig_tier_gates PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_dig_tier_gates PROPERTIES C_STANDARD 99)
target_link_libraries(test_dig_tier_gates ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_dig_tier_gates COMMAND test_dig_tier_gates
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Door placement faithfulness (item_door_wood.c/item_door_iron.c fix, lane/items):
# ItemDoor.onItemUse's yaw-derived facing + neighbour hinge-flip, replacing the
# old on_side switch that packed a fixed metadata (defaulting to 6 -- a facing
# AND the OPEN bit) regardless of player facing. Four cardinal yaws, the
# adjacent-door mirror flip, the solid-neighbour-count flip, and the
# TOP-face-only placement gate; iron door reuses the identical algorithm.
add_executable(test_door_placement tests/test_door_placement.c)
target_link_libraries(test_door_placement cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_door_placement PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_door_placement PROPERTIES C_STANDARD 99)
target_link_libraries(test_door_placement ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_door_placement COMMAND test_door_placement
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Bed placement faithfulness (item_bed.c fix, lane/items): ItemBed.onItemUse's
# yaw-derived direction with NO fallback search, replacing the old fixed
# +Z,-Z,+X,-X search-order that ignored player facing. Four cardinal yaws
# (translated to this project's own historic {+Z,-Z,+X,-X} metadata<<1
# table), the no-fallback-search regression, missing-support refusals for
# both halves, and the TOP-face-only placement gate.
add_executable(test_bed_placement tests/test_bed_placement.c)
target_link_libraries(test_bed_placement cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_bed_placement PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_bed_placement PROPERTIES C_STANDARD 99)
target_link_libraries(test_bed_placement ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_bed_placement COMMAND test_bed_placement
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# lever-piston-0925: floor lever orientation = 5 + World.rand.nextInt(2)
# (BlockLever.onBlockPlaced:37-38), exactly one world-stream draw per floor placement.
add_executable(test_lever_place_golden tests/test_lever_place_golden.c)
target_link_libraries(test_lever_place_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_lever_place_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_lever_place_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_lever_place_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_lever_place_golden COMMAND test_lever_place_golden
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# lever-piston-0925: piston-push step acceptance compared in double
# (Entity.moveEntity:398-402), pure header pmove_step_compare.h.
add_executable(test_piston_step_compare_golden tests/test_piston_step_compare_golden.c)
target_include_directories(test_piston_step_compare_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_piston_step_compare_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_piston_step_compare_golden m)
add_test(NAME test_piston_step_compare_golden COMMAND test_piston_step_compare_golden)

# Held-map colour transport (held-map render slice part 1, 2026-08-26):
# CRPC_MAP_DATA delivery byte-for-byte, change gating (no re-send when the
# scan is unchanged), 100-round pool hygiene through the 2-slot pool, and
# the clin_drain_discard slot release.
add_executable(test_map_transport tests/test_map_transport.c)
target_link_libraries(test_map_transport cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_map_transport PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_map_transport PROPERTIES C_STANDARD 99)
target_link_libraries(test_map_transport ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_map_transport COMMAND test_map_transport)

# FH-14 (docs/fork_hack_audit_2026-08-21.md:166): redstone wire digging hardness
# 1050 -> Beta's instant-break 0.0F, encoded as the port's nominal-50 one-tick
# convention (not block_repeater.c's literal 0, which tool.c's <=0 sentinel makes
# indistinguishable from truly unbreakable -- see block_redstone_wire.c's comment).
add_executable(test_redstone_wire_dig tests/test_redstone_wire_dig.c)
target_link_libraries(test_redstone_wire_dig cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_wire_dig PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_wire_dig PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_wire_dig ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_wire_dig COMMAND test_redstone_wire_dig
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# FH-12: item pickup delay is a REAL per-instance Beta EntityItem
# .delayBeforeCanPickup field, not the flat `age >= 2*4` universal gate.
# Pins the four decompile-verified values (block-break drops = 10, dispenser
# eject = 0, player Q-drop = 40, none of it persisted) against real spawn call
# sites (server_local_spawn_block_drops_after_break, server_local_dispenser_
# dispense, server_local_drop_hotbar_item) and a real save/evict/reload.
add_executable(test_item_pickup_delay tests/test_item_pickup_delay.c)
target_link_libraries(test_item_pickup_delay cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_item_pickup_delay PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_item_pickup_delay PROPERTIES C_STANDARD 99)
target_link_libraries(test_item_pickup_delay ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_item_pickup_delay COMMAND test_item_pickup_delay)

# Wire-place self-flood (Beta onBlockAdded): a lone redstone DUST placed beside a pre-existing power
# source must latch full strength on placement, driven through the REAL item_redstone.onItemPlace path.
add_executable(test_redstone_wire_place_flood_env tests/test_redstone_wire_place_flood_env.c)
target_link_libraries(test_redstone_wire_place_flood_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_wire_place_flood_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_wire_place_flood_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_wire_place_flood_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_wire_place_flood_env COMMAND test_redstone_wire_place_flood_env)

# Stone button (Beta BlockButton, id 77) + the door's var5-gated redstone rewiring: press/
# release timing over the scheduled-tick queue, wire latching, support-drops, top-half power,
# hand-open persistence vs the power-cut close quirk, iron-door hand-proofing.
add_executable(test_redstone_button_door_env tests/test_redstone_button_door_env.c)
target_link_libraries(test_redstone_button_door_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_button_door_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_button_door_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_button_door_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_button_door_env COMMAND test_redstone_button_door_env)

# Dispenser-slice goldens: direction table + spawn point (pure), reservoir-
# sampling pick/decrement (pure, pinned RNG), empty-click SFX 1001, generic-
# item eject RNG order (one nextDouble then three nextGaussians, pinned),
# arrow/egg/snowball heading + entity type, the two-cell redstone trigger +
# scheduled-tick re-check, and a real save/evict/reload persistence round
# trip. See DISPENSER_SLICE_NOTES.md.
add_executable(test_dispenser_env tests/test_dispenser_env.c)
target_link_libraries(test_dispenser_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_dispenser_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_dispenser_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_dispenser_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_dispenser_env COMMAND test_dispenser_env)

# Beta 1.7.3 selection/collision bounds for the complete critique-1028 audit.
add_executable(test_block_bounds_golden tests/test_block_bounds_golden.c)
target_link_libraries(test_block_bounds_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_block_bounds_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_block_bounds_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_block_bounds_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_block_bounds_golden COMMAND test_block_bounds_golden)

# Trapdoor onNeighbourBlockChange (support-drop + redstone open/close): the handler was dead
# (every dispatch site passes block_info.neighbours == NULL, but the old code gated on it).
# Rewritten on the block_tnt.c/block_ladder.c pattern (live world reads); this pins the
# support-drop metadata->wall map (incl. a wrong-side negative control) and the redstone
# open/close round-trip via a real lever, over a live superflat server_world.
add_executable(test_trapdoor_env tests/test_trapdoor_env.c)
target_link_libraries(test_trapdoor_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_trapdoor_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_trapdoor_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_trapdoor_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_trapdoor_env COMMAND test_trapdoor_env)

add_executable(test_place_fence_torch_trapdoor tests/test_place_fence_torch_trapdoor.c)
target_link_libraries(test_place_fence_torch_trapdoor cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_place_fence_torch_trapdoor PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_place_fence_torch_trapdoor PROPERTIES C_STANDARD 99)
target_link_libraries(test_place_fence_torch_trapdoor ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_place_fence_torch_trapdoor COMMAND test_place_fence_torch_trapdoor)

# next-gaps-0924/W2-R1: Beta Block.onBlockClicked (dig-start) + Block.onEntityWalking (footstep)
# goldens over a live server_local: redstone ore/note/lever/button/trapdoor/doors/cake/TNT/stairs
# clicks, player + mob + fence-below walk dispatch, farmland trample for every walker (R1c),
# dig-start dedup + stamped-TNT neighbour inertness + pig-rider gate (R1d).
add_executable(test_block_click_walk_env tests/test_block_click_walk_env.c)
target_link_libraries(test_block_click_walk_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_block_click_walk_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_block_click_walk_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_block_click_walk_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_block_click_walk_env COMMAND test_block_click_walk_env
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# canBlockStay/support-drop + growth-gate residuals (Beta BlockPressurePlate/BlockCactus/
# BlockReed/BlockFlower): plate phantom-power drop, cactus/reed counter persistence under a
# ceiling, cactus solid-side pop cascade, flower/rose soil pop + farmland soil.
add_executable(test_block_stay_growth_env tests/test_block_stay_growth_env.c)
target_link_libraries(test_block_stay_growth_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_block_stay_growth_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_block_stay_growth_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_block_stay_growth_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_block_stay_growth_env COMMAND test_block_stay_growth_env)

# Real-Wii redstone freeze repro: forced torch-clock stress plus rapid torch place/break through
# the production server_world paths. Diagnostic, not a Beta golden; it must at least prove the
# inverter toggled, then finish without watchdog aborts or scheduled-tick drops.
add_executable(test_redstone_torch_clock_storm_env tests/test_redstone_torch_clock_storm_env.c)
target_link_libraries(test_redstone_torch_clock_storm_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_torch_clock_storm_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_torch_clock_storm_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_torch_clock_storm_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_torch_clock_storm_env COMMAND test_redstone_torch_clock_storm_env)

# Real-Wii redstone freeze repro smoke: close odd-inverter/repeater feedback
# loops via the real redstone-dust item path while dropped torch items are live.
# Diagnostic, not a Beta golden; it asserts non-zero but bounded notify/wire work.
add_executable(test_redstone_closed_loop_freeze_env tests/test_redstone_closed_loop_freeze_env.c)
target_link_libraries(test_redstone_closed_loop_freeze_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_closed_loop_freeze_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_closed_loop_freeze_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_closed_loop_freeze_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_closed_loop_freeze_env COMMAND test_redstone_closed_loop_freeze_env)

# Real-Wii redstone freeze repro: close an interrupted dust loop via the real item placer with
# dropped torch items and entity pickup ticks live. This covers the refined hardware trigger that
# the forced-clock test deliberately does not model.
add_executable(test_redstone_loop_close_drops_env tests/test_redstone_loop_close_drops_env.c)
target_link_libraries(test_redstone_loop_close_drops_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_loop_close_drops_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_loop_close_drops_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_loop_close_drops_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_loop_close_drops_env COMMAND test_redstone_loop_close_drops_env)

# Real-Wii freeze interaction pre-filter: interleave the loop-completion redstone
# repro with active client relight churn and assert both production work budgets.
add_executable(test_redstone_lighting_churn_env tests/test_redstone_lighting_churn_env.c)
target_link_libraries(test_redstone_lighting_churn_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_redstone_lighting_churn_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_redstone_lighting_churn_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_redstone_lighting_churn_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_redstone_lighting_churn_env COMMAND test_redstone_lighting_churn_env)

# Vendored cNBT int/long array deep-copy + compare correctness (lean target: only the two cNBT files).
add_executable(test_cnbt_arrays tests/test_cnbt_arrays.c source/cNBT/nbt_treeops.c source/cNBT/nbt_util.c)
target_link_libraries(test_cnbt_arrays m)
target_compile_definitions(test_cnbt_arrays PRIVATE PLATFORM_PC)
target_include_directories(test_cnbt_arrays PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_cnbt_arrays PROPERTIES C_STANDARD 99)
add_test(NAME test_cnbt_arrays COMMAND test_cnbt_arrays)

add_executable(test_cnbt_limits
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_cnbt_limits.c
)
target_link_libraries(test_cnbt_limits m)
target_compile_definitions(test_cnbt_limits PRIVATE PLATFORM_PC)
target_include_directories(test_cnbt_limits PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_cnbt_limits PROPERTIES C_STANDARD 99)
target_link_libraries(test_cnbt_limits ZLIB::ZLIB)
add_test(NAME test_cnbt_limits COMMAND test_cnbt_limits)

# T1 equivalence test for the 2026-09-01 cNBT buffer pre-size fix
# (source/cNBT/buffer.c buffer_reserve_exact + nbt_dump_binary_hint/
# nbt_dump_compressed_hint, wired into region_archive.c's chunk-save path):
# proves the new size-hint entry points produce byte-identical output to the
# unhinted defaults (hint too small, exact, and zero), and that
# buffer_reserve_exact actually pre-sizes in one allocation.
add_executable(test_nbt_buffer_size_hint
	source/cNBT/buffer.c
	source/cNBT/nbt_loading.c
	source/cNBT/nbt_parsing.c
	source/cNBT/nbt_treeops.c
	source/cNBT/nbt_util.c
	tests/test_nbt_buffer_size_hint.c
)
target_link_libraries(test_nbt_buffer_size_hint m)
target_compile_definitions(test_nbt_buffer_size_hint PRIVATE PLATFORM_PC)
target_include_directories(test_nbt_buffer_size_hint PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_nbt_buffer_size_hint PROPERTIES C_STANDARD 99)
target_link_libraries(test_nbt_buffer_size_hint ZLIB::ZLIB)
add_test(NAME test_nbt_buffer_size_hint COMMAND test_nbt_buffer_size_hint)

# Faithfulness golden: flowing fluids schedule at Beta tickRate (water=5, lava=30), not the default 1.
add_executable(test_fluid_tick_rate tests/test_fluid_tick_rate.c)
target_link_libraries(test_fluid_tick_rate cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_fluid_tick_rate PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_fluid_tick_rate PROPERTIES C_STANDARD 99)
target_link_libraries(test_fluid_tick_rate ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_fluid_tick_rate COMMAND test_fluid_tick_rate)

# Faithfulness golden: Beta crop growth is a probabilistic light-gated roll, not a deterministic +1/tick.
add_executable(test_crops_growth tests/test_crops_growth.c)
target_link_libraries(test_crops_growth cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_crops_growth PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_crops_growth PROPERTIES C_STANDARD 99)
target_link_libraries(test_crops_growth ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_crops_growth COMMAND test_crops_growth)

# next-gaps-0924/W1D: Beta crop/sapling day-night growth light and RNG gate.
add_executable(test_crop_sapling_light_golden tests/test_crop_sapling_light_golden.c)
target_link_libraries(test_crop_sapling_light_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_crop_sapling_light_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_crop_sapling_light_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_crop_sapling_light_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_crop_sapling_light_golden COMMAND test_crop_sapling_light_golden)
# next-gaps-0924/GROWTH-A: Beta BlockCrops inherits BlockFlower.canBlockStay (light >= 8 || sky,
# on farmland): dark crops pop on random tick AND neighbour change, and BlockCrops.updateTick keeps
# going after the pop (post-pop growth draw). Leaves' missing-chunk verdict is already pinned by
# test_leaf_decay_golden (e). [BlockFlower.java:22-41, BlockCrops.java:18-31, World.java:521-531]
add_executable(test_growth_leaves_crops_golden tests/test_growth_leaves_crops_golden.c)
target_link_libraries(test_growth_leaves_crops_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_growth_leaves_crops_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_growth_leaves_crops_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_growth_leaves_crops_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_growth_leaves_crops_golden COMMAND test_growth_leaves_crops_golden)
# Beta growth draw order and random-tick versus neighbour-change stay checks.
add_executable(test_growth_misc_golden tests/test_growth_misc_golden.c)
target_link_libraries(test_growth_misc_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_growth_misc_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_growth_misc_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_growth_misc_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_growth_misc_golden COMMAND test_growth_misc_golden)
# next-gaps-0924/W1E: Beta leaf decay golden -- flag on log/leaf removal (metadata bit 8),
# ItemLeaves placement (damage | 8), the 32^3 connected-leaf walk (log within 4 leaf steps),
# the World.rand draw sequence of removeLeaves, and the checkChunksExist stale-centre quirk.
# [740c583 BlockLeaves.java:33-149, BlockLog.java:23-42, ItemLeaves.java:10-12]
add_executable(test_leaf_decay_golden tests/test_leaf_decay_golden.c)
target_link_libraries(test_leaf_decay_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_leaf_decay_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_leaf_decay_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_leaf_decay_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_leaf_decay_golden COMMAND test_leaf_decay_golden)

# next-gaps-0924/W1E-b: T1 equivalence -- the 11^3 int8_t leaf-decay scratch against an independent
# transcription of Beta's 32^3 int[] updateTick (BlockLeaves.java:52-144) over random + adversarial
# layouts and chunk-gated stale-centre sequences: identical verdicts, metadata, World.rand draws.
add_executable(test_leaf_decay_scratch_equiv tests/test_leaf_decay_scratch_equiv.c)
target_link_libraries(test_leaf_decay_scratch_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_leaf_decay_scratch_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_leaf_decay_scratch_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_leaf_decay_scratch_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_leaf_decay_scratch_equiv COMMAND test_leaf_decay_scratch_equiv)

# next-gaps-0924/W1E-c: populate-time leaf flagging -- a tree trunk overwriting its own canopy fires
# BlockLeaves.onBlockRemoval (WorldGenTrees.java:56-65, Chunk.java:285-286): the 3x3 column of leaves
# around the trunk top carries metadata bit 8, nothing else does; chunk gate at the 2x2 populate window.
add_executable(test_leaf_populate_flag_golden tests/test_leaf_populate_flag_golden.c)
target_link_libraries(test_leaf_populate_flag_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_leaf_populate_flag_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_leaf_populate_flag_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_leaf_populate_flag_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_leaf_populate_flag_golden COMMAND test_leaf_populate_flag_golden)

# Spawner light-opacity parity (lane C 2026-09-27): BlockMobSpawner.isOpaqueCube
# false -> Block.lightOpacity 0 (Block.java:151-152); heightmap + skylight column.
add_executable(test_spawner_opacity tests/test_spawner_opacity.c)
target_link_libraries(test_spawner_opacity cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_spawner_opacity PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_spawner_opacity PROPERTIES C_STANDARD 99)
target_link_libraries(test_spawner_opacity ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_spawner_opacity COMMAND test_spawner_opacity)

# laneC/TREE-HEIGHTMAP (2026-09-26): tree attempts read World.getHeightValue,
# whose heightMap Chunk.setBlockID keeps current on every populate write; the
# live quad view must answer with the same value over real terrain.
add_executable(test_tree_heightmap_live tests/test_tree_heightmap_live.c)
target_link_libraries(test_tree_heightmap_live cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_tree_heightmap_live PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_tree_heightmap_live PROPERTIES C_STANDARD 99)
target_link_libraries(test_tree_heightmap_live ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_tree_heightmap_live COMMAND test_tree_heightmap_live)

# next-gaps-0924/W1E-d: leaves are opaque to canopy placement (Block.java:151 fills opaqueCubeLookup[18]
# from BlockLeaves.isOpaqueCube before graphicsLevel is assigned; WorldGenTrees:55 & co skip opaque
# cells): overlapping mixed-species trees keep the first tree's species; block ids + RNG identical.
add_executable(test_tree_leaves_opaque_golden tests/test_tree_leaves_opaque_golden.c)
target_link_libraries(test_tree_leaves_opaque_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_tree_leaves_opaque_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_tree_leaves_opaque_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_tree_leaves_opaque_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_tree_leaves_opaque_golden COMMAND test_tree_leaves_opaque_golden)
# next-gaps-0924/F1: Beta World.getBlockLightValue golden -- slab/stairs/farmland neighbour max and
# rain/thunder skylightSubtracted for crops/saplings/grass, the hostile-spawn thunder override 10, bounds.
# [740c583 World.java:537-583,991-1007; EntityMob.java:69-86; BlockCrops:20; BlockSapling:15; BlockGrass:32-43]
add_executable(test_block_light_value_golden tests/test_block_light_value_golden.c)
target_link_libraries(test_block_light_value_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_block_light_value_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_block_light_value_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_block_light_value_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_block_light_value_golden COMMAND test_block_light_value_golden)
# next-gaps-0924/P7: Beta sleep spawning golden -- performSleepSpawning on the completing sleep tick,
# Peaceful/guest gates, BlockBed.getNearestEmptyChunkCoordinates placement (skip 1) and wake cell (skip 0).
# [740c583 World.java:1740-1752; SpawnerAnimals.java:169-246; BlockBed.java:158-182; EntityPlayer.java:632-646]
add_executable(test_sleep_spawn_golden tests/test_sleep_spawn_golden.c)
target_link_libraries(test_sleep_spawn_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_sleep_spawn_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_sleep_spawn_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_sleep_spawn_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_sleep_spawn_golden COMMAND test_sleep_spawn_golden)
# A1d review limit L1: Beta performSleepSpawning tries EVERY player (SpawnerAnimals.java:169-246);
# CaveWii tries only the host. RED -> WILL_FAIL until the sleep spawn iterates every Overworld
# player (owner TBD: A2 or lane A; card text in tmp/jobs/split-dimensions-plan-0924/out/A1d-result.md).
add_executable(test_sleep_spawn_all_sleepers tests/test_sleep_spawn_all_sleepers.c)
target_link_libraries(test_sleep_spawn_all_sleepers cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_sleep_spawn_all_sleepers PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_sleep_spawn_all_sleepers PROPERTIES C_STANDARD 99)
target_link_libraries(test_sleep_spawn_all_sleepers ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_sleep_spawn_all_sleepers COMMAND test_sleep_spawn_all_sleepers)
set_tests_properties(test_sleep_spawn_all_sleepers PROPERTIES TIMEOUT 300)
# next-gaps-0924/w2: Beta skylightSubtracted cached once per tick (World.tick:1757-1759) -- readers see the
# tick-start value; the sleeper's isDaytime reads the previous tick's (Minecraft.runTick:1147/1152).
add_executable(test_skylight_tick_golden tests/test_skylight_tick_golden.c)
target_link_libraries(test_skylight_tick_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_skylight_tick_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_skylight_tick_golden PROPERTIES C_STANDARD 99)
add_test(NAME test_skylight_tick_golden COMMAND test_skylight_tick_golden)
set_tests_properties(test_skylight_tick_golden PROPERTIES TIMEOUT 300)
# next-gaps-0924/w5: the debug.sleep_spawn_stress hardware fixture really runs the completing sleep tick.
add_executable(test_sleep_spawn_stress_fixture tests/test_sleep_spawn_stress_fixture.c)
target_link_libraries(test_sleep_spawn_stress_fixture cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_sleep_spawn_stress_fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_sleep_spawn_stress_fixture PROPERTIES C_STANDARD 99)
add_test(NAME test_sleep_spawn_stress_fixture COMMAND test_sleep_spawn_stress_fixture)
set_tests_properties(test_sleep_spawn_stress_fixture PROPERTIES TIMEOUT 300)

# next-gaps-0924/W1E-e: WorldGenBigTree leaf discs skip existing leaves (WorldGenBigTree.java:114-118 setBlock
# -> Chunk.setBlockID:279 same-id no-op): a big oak over a birch canopy keeps the birch species, and the
# runtime (lifecycle) growth path equals the populate-writer path cell for cell -- no extra removal flags.
add_executable(test_bigtree_leaves_keep_golden tests/test_bigtree_leaves_keep_golden.c)
target_link_libraries(test_bigtree_leaves_keep_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_bigtree_leaves_keep_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_bigtree_leaves_keep_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_bigtree_leaves_keep_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_bigtree_leaves_keep_golden COMMAND test_bigtree_leaves_keep_golden)

# Beta block-behavior parity gaps fixed from the 2026-06-19 audit: reed water-adjacency, cake eating,
# farmland hydration.
add_executable(test_block_behavior_parity tests/test_block_behavior_parity.c)
target_link_libraries(test_block_behavior_parity cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_block_behavior_parity PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_block_behavior_parity PROPERTIES C_STANDARD 99)
target_link_libraries(test_block_behavior_parity ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_block_behavior_parity COMMAND test_block_behavior_parity)

# Faithfulness golden: Beta BlockFire spread (catch/encourage tables, age death, neighbour ignition,
# TNT prime, air spread) -- the rewritten block_fire.c updateTick.
add_executable(test_fire tests/test_fire.c)
target_link_libraries(test_fire cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_fire PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_fire PROPERTIES C_STANDARD 99)
target_link_libraries(test_fire ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_fire COMMAND test_fire)

add_executable(test_fire_dispenser_parity tests/test_fire_dispenser_parity.c)
target_link_libraries(test_fire_dispenser_parity cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_fire_dispenser_parity PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_fire_dispenser_parity PROPERTIES C_STANDARD 99)
target_link_libraries(test_fire_dispenser_parity ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_fire_dispenser_parity COMMAND test_fire_dispenser_parity)

# L211: production mounted-contact exclusion and split splash/wolf ownership.
add_executable(test_l211_portal_split tests/test_l211_portal_split.c)
target_link_libraries(test_l211_portal_split cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB
	glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_l211_portal_split PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_l211_portal_split PROPERTIES C_STANDARD 99)
add_executable(test_portal_dwell_equiv tests/test_portal_dwell_equiv.c)
target_link_libraries(test_portal_dwell_equiv cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB
	glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_portal_dwell_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_portal_dwell_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_portal_dwell_equiv COMMAND test_portal_dwell_equiv)
foreach(_kind RANGE 0 2)
	foreach(_guest RANGE 0 1)
		foreach(_nether RANGE 0 1)
			add_test(NAME test_l211_portal_${_kind}_${_guest}_${_nether}
				COMMAND test_l211_portal_split portal ${_kind} ${_guest} ${_nether})
		endforeach()
	endforeach()
endforeach()
foreach(_case splash wolf empty armed walks scan)
	foreach(_host_nether RANGE 0 1)
		add_test(NAME test_l211_${_case}_${_host_nether}
			COMMAND test_l211_portal_split ${_case} ${_host_nether})
	endforeach()
endforeach()

add_test(NAME test_l211_wolf_payload_off COMMAND test_l211_portal_split wolf_payload_off 0)

# Faithfulness golden: Beta nether-portal ignition (BlockFire.onBlockAdded ->
# BlockPortal.tryToCreatePortal via the set_block fire chokepoint) + the portal pane's
# onNeighborBlockChange self-break rules. M5 portal-travel part 1.
add_executable(test_portal_travel tests/test_portal_travel.c)
target_link_libraries(test_portal_travel cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_portal_travel PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_portal_travel PROPERTIES C_STANDARD 99)
target_link_libraries(test_portal_travel ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_portal_travel COMMAND test_portal_travel)

# Crash-consistency golden for Nether death ownership. Exercises the durable
# drop journal across allocation failure, partial region writes, failed journal
# removal, startup recovery, and an empty transaction.
add_executable(test_death_drop_journal_env
	tests/test_death_drop_journal_env.c
)
target_link_libraries(test_death_drop_journal_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_death_drop_journal_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_death_drop_journal_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_death_drop_journal_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_death_drop_journal_env COMMAND test_death_drop_journal_env)

# Deviation-#18 closure: the death->respawn split. Lethal damage commits only
# the dead state; the drop/journal/respawn transaction waits for
# server_local_respawn_request (the death screen's Respawn button). Covers the
# dead-terminal guards, drops-at-death-position, respawn-time bed resolution
# and the quit-while-dead level.dat round-trip.
add_executable(test_death_screen_env
	tests/test_death_screen_env.c
)
target_link_libraries(test_death_screen_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_death_screen_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_death_screen_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_death_screen_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_death_screen_env COMMAND test_death_screen_env)

# IMP-QUIT-UNSAVABLE: an unsavable chunk at Save and quit answers the
# "Couldn't save part of the world" dialog state instead of hanging; Try again
# succeeds once the cause clears; Quit anyway saves every other chunk and quits.
add_executable(test_quit_unsavable_env tests/test_quit_unsavable_env.c)
target_link_libraries(test_quit_unsavable_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_quit_unsavable_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_quit_unsavable_env PROPERTIES C_STANDARD 99)
add_test(NAME test_quit_unsavable_env COMMAND test_quit_unsavable_env)

# CREATIVE MODE phase 1 (net-new, maintainer exception 2026-08-25,
# docs/creative_mode.md): the server damage funnel under creative on/off --
# invulnerability gates + the default-path-unchanged equivalence check.
add_executable(test_creative_mode_env
	tests/test_creative_mode_env.c
)
target_link_libraries(test_creative_mode_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_creative_mode_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_creative_mode_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_creative_mode_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_creative_mode_env COMMAND test_creative_mode_env)

# View bobbing (Beta 1.7.3 parity, render/feel layer): pins the ported
# constants of EntityRenderer.setupViewBobbing + the per-tick sway inputs
# against the decompile citations (pure header, no client link).
add_executable(test_view_bobbing
	tests/test_view_bobbing.c
)
target_link_libraries(test_view_bobbing m)
add_test(NAME test_view_bobbing COMMAND test_view_bobbing)

# Batch J hurt feedback (Beta 1.7.3 parity, render/feel layer): pins the
# MP-client health-delta hurt derivation (EntityPlayerSP.setHealth), the
# heart-blink phase, the jitter rand seed (Java int wrap) and the hurt/death
# camera roll math of hud_hurt.h against the decompile citations (pure
# header, no client link -- camera_bob.h/test_view_bobbing precedent).
add_executable(test_hud_hurt
	tests/test_hud_hurt.c
)
target_link_libraries(test_hud_hurt m)
add_test(NAME test_hud_hurt COMMAND test_hud_hurt)

# Creative HUD (maintainer ruling 2026-09-03, docs/creative_mode.md "Creative HUD --
# no hearts/armor/air"): focused link of the extracted hearts+armor+air row
# (hud_survival_row.c, verbatim from the pre-split screen_ingame_render2D block) plus
# the real java_random.c (the heart-jitter rand must stay bit-exact) -- every leaf
# gfx/inventory symbol is mocked to a trace. Reads screen_ingame.c as text to pin the
# real call site (`if(!gstate.config_creative) hud_survival_row_draw(...)`), so
# WORKING_DIRECTORY must be the repo root (same convention as test_watchdog_phase).
add_executable(test_hud_survival_row
	source/game/gui/hud_survival_row.c
	source/worldgen/java_random.c
	tests/test_hud_survival_row.c
)
target_compile_definitions(test_hud_survival_row PRIVATE PLATFORM_PC)
target_include_directories(test_hud_survival_row PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_hud_survival_row m)
add_test(NAME test_hud_survival_row COMMAND test_hud_survival_row
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Creative phase-2 item picker (NET-NEW, docs/creative_mode.md): the list
# module's include-predicate/ordering contract and the SRPC_CREATIVE_GIVE
# handler's gates (creative-off no-op, registry validation, hotbar-first,
# full-inventory no-op).
add_executable(test_creative_picker tests/test_creative_picker.c)
target_link_libraries(test_creative_picker cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_creative_picker PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_creative_picker PROPERTIES C_STANDARD 99)
target_link_libraries(test_creative_picker ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_creative_picker COMMAND test_creative_picker)

# Group A of the 2026-06-19 mob-AI parity audit: the on_ground ordering fix. Integration
# goldens that the prior pure-helper tests could not reach -- ground friction slip
# selection (entity_apply_gravity_and_friction) and the now-reachable spider pounce
# (driven through a real server_tick_monster over a superflat server_world).
add_executable(test_spider_leap_env tests/test_spider_leap_env.c)
target_link_libraries(test_spider_leap_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_spider_leap_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_spider_leap_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_spider_leap_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_spider_leap_env COMMAND test_spider_leap_env)

# Group D of the 2026-06-19 mob-AI parity audit: arrow drag-then-gravity integration order
# + the Beta 0.3 hit-box expansion, driven through the REAL arrow tick over a superflat world.
add_executable(test_arrow_env tests/test_arrow_env.c)
target_link_libraries(test_arrow_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_arrow_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_arrow_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_arrow_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_arrow_env COMMAND test_arrow_env)

# laneC/C-DEADTARGET 2026-09-26: creatures stop attacking a dead target
# (EntityCreature.updatePlayerActionState:19-33 drop/re-acquire, isEntityAlive).
add_executable(test_attack_dead_target_golden tests/test_attack_dead_target_golden.c)
target_link_libraries(test_attack_dead_target_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_attack_dead_target_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_attack_dead_target_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_attack_dead_target_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_attack_dead_target_golden COMMAND test_attack_dead_target_golden)
# I02 (board #8): 626edc43 per-entity RNG gate, now GREEN (no WILL_FAIL)
add_executable(test_entity_rng_streams tests/test_entity_rng_streams.c)
target_link_libraries(test_entity_rng_streams cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_entity_rng_streams PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_entity_rng_streams PROPERTIES C_STANDARD 99)
target_link_libraries(test_entity_rng_streams ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_entity_rng_streams COMMAND test_entity_rng_streams)

# G10 (i05-1005): World.tick TickUpdates before doRandomUpdateTicks [CFR World.java:1372-1374]
add_executable(test_world_tick_order_golden tests/test_world_tick_order_golden.c)
target_link_libraries(test_world_tick_order_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_world_tick_order_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_world_tick_order_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_world_tick_order_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_world_tick_order_golden COMMAND test_world_tick_order_golden)

# G9 gate (i05-1005): Beta queues light and drains after World.tick [CFR Chunk.java:141-239; MinecraftServer:340-350].
add_executable(test_light_drain_order_golden tests/test_light_drain_order_golden.c)
target_link_libraries(test_light_drain_order_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_light_drain_order_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_light_drain_order_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_light_drain_order_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
# B1 (ASTRA-1005B line 7): one ctest per consumer case (pre- and post-drain
# values and consumer outcomes); GREEN since the server light queue landed (B5).
# __settled counts only the post-drain checks.
foreach(_g9_case roof_place roof_remove torch_place torch_remove multi_write crop_rng ice_melt spawn_light two_worlds)
	add_test(NAME test_light_drain_order_golden__${_g9_case} COMMAND test_light_drain_order_golden ${_g9_case})
endforeach()
add_test(NAME test_light_drain_order_golden__settled COMMAND test_light_drain_order_golden settled)

# G9 B4/B5 (ASTRA-G9-RULING 5-9): queue equivalence (sync == batched == LIFO
# drain, byte-exact), blast, reservation fault, pins, provision deferral,
# shutdown drain.

add_executable(test_light_no_downgrade tests/test_light_no_downgrade.c)
target_link_libraries(test_light_no_downgrade cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_light_no_downgrade PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_light_no_downgrade PROPERTIES C_STANDARD 99)
target_compile_options(test_light_no_downgrade PRIVATE -UNDEBUG)
target_compile_definitions(test_light_no_downgrade PRIVATE ${CAVEX_TEST_ENGINE_DEFINES})
add_test(NAME test_light_no_downgrade COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_light_no_downgrade.py $<TARGET_FILE:test_light_no_downgrade>)
set_tests_properties(test_light_no_downgrade PROPERTIES TIMEOUT 300)
add_executable(test_worldgen_jar_parity tests/test_worldgen_jar_parity.c)
target_link_libraries(test_worldgen_jar_parity cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_worldgen_jar_parity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_worldgen_jar_parity PROPERTIES C_STANDARD 99)
target_compile_options(test_worldgen_jar_parity PRIVATE -UNDEBUG)
target_compile_definitions(test_worldgen_jar_parity PRIVATE ${CAVEX_TEST_ENGINE_DEFINES})
add_test(NAME test_worldgen_jar_parity COMMAND test_worldgen_jar_parity ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/lighting/g9_option_b/fixtures/no-downgrade/original-jar.bin.gz)
set_tests_properties(test_worldgen_jar_parity PROPERTIES TIMEOUT 300)

add_executable(test_light_queue_equiv tests/test_light_queue_equiv.c
	tools/wii_stage_server/source/sha256.c) # the debt golden's digest
target_link_libraries(test_light_queue_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_light_queue_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
	${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source
)
set_target_properties(test_light_queue_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_light_queue_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_light_queue_equiv__history
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/lighting/g9_option_b/compare/run_observer.py
	$<TARGET_FILE:test_light_queue_equiv>)
add_test(NAME test_light_queue_equiv__nether_setters_beta
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/lighting/g9_option_b/compare/production_setters.py
	$<TARGET_FILE:test_light_queue_equiv>)
add_test(NAME test_light_queue_equiv__world_rng_load32_beta
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/lighting/g9_option_b/compare/production_rng.py
	$<TARGET_FILE:test_light_queue_equiv>)
add_test(NAME test_light_queue_equiv__nether_runtime_lava_beta
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/lighting/g9_option_b/compare/production_lava.py
	$<TARGET_FILE:test_light_queue_equiv>)
foreach(_g9_q equiv explosion fault lifecycle provision shutdown phases drain_loss drain_stop_update contain checkpoint beta_oracle consumers workloads hw_workload packet_delivery packet_delivery_nether
	generated_nether_beta nether_population_height_beta automatic_population_beta automatic_population_hold nether_provider_rng_beta generation_height_order_beta nether_plain_rollback g9x_dirty_reload g9x_snow_oom g9x_observer_reuse g9x_region_budget g9x_memory_progress g9x_publication g9x_drain_t1_equiv)
	add_test(NAME test_light_queue_equiv__${_g9_q} COMMAND test_light_queue_equiv ${_g9_q})
endforeach()
# G9X-2: the case's CFR-derived assertions PLUS the same script replayed
# through the instrumented original JAR (live JVM), compared step by step.
add_test(NAME test_light_queue_equiv__g9x_callbacks
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/lighting/g9_option_b/compare/setter_checks.py
	--callbacks $<TARGET_FILE:test_light_queue_equiv>)
# ASTRA-G9B R7: historical producer/model/inputs in isolation. Changing
# production is no longer required to reproduce the canonical debt output.
add_test(NAME test_light_queue_equiv__generation_parity_debt_golden
	COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/lighting/g9_option_b/historical_debt/reproduce.py
	--repo ${CMAKE_CURRENT_SOURCE_DIR}
	--cache ${CMAKE_CURRENT_BINARY_DIR}/g9-historical-debt)
# G9X-7 (2026-10-15, G9-EXACT-PLAN item 20): exact Beta generation lighting
# is release-blocking (maintainer ruling 2026-10-12 pm). The original-JAR
# history gate passes exactly, so it is a normal blocking test: no
# known_fail/metric exclusion label (the G9-OPTION-B card label only).
add_test(NAME test_light_queue_equiv__generation_beta_parity
	COMMAND test_light_queue_equiv generation_beta_parity)
set_tests_properties(test_light_queue_equiv__generation_beta_parity
	PROPERTIES LABELS "G9-OPTION-B")

# G9 R19 (ASTRA-G9-R19-IO): save IO off the server tick. The equivalence test
# compares the synchronous reference, the shipping helper and the opt-in
# deferred worker byte for byte (full save directory + per-file ordered
# mutation trace) at every barrier; the durability test drives deterministic
# stalls, queue exhaustion, injected faults and a power cut at every
# persistence transition, reopening each image twice.
foreach(_g9io test_g9_save_io_equiv test_g9_save_io_durability test_g9_shadow_cap_overflow)
	add_executable(${_g9io} tests/${_g9io}.c)
	target_link_libraries(${_g9io} cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
	target_include_directories(${_g9io} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/source
		${CMAKE_CURRENT_SOURCE_DIR}/third_party
		${CMAKE_CURRENT_SOURCE_DIR}/tests)
	set_target_properties(${_g9io} PROPERTIES C_STANDARD 99)
	target_link_libraries(${_g9io} ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
endforeach()
add_test(NAME test_g9_shadow_cap_overflow COMMAND test_g9_shadow_cap_overflow)
set_tests_properties(test_g9_shadow_cap_overflow PROPERTIES TIMEOUT 30)
foreach(_g9io_case world session)
	add_test(NAME test_g9_save_io_equiv__${_g9io_case}
		COMMAND test_g9_save_io_equiv ${_g9io_case})
	set_tests_properties(test_g9_save_io_equiv__${_g9io_case} PROPERTIES TIMEOUT 600)
endforeach()
foreach(_g9io_case faults stalls powercut)
	add_test(NAME test_g9_save_io_durability__${_g9io_case}
		COMMAND test_g9_save_io_durability ${_g9io_case})
	set_tests_properties(test_g9_save_io_durability__${_g9io_case} PROPERTIES TIMEOUT 900)
endforeach()
# SPEC R12: scripted journey save -> quit -> fresh-process reload, and the
# journey save-and-quit killed at every traced persistence transition.
add_executable(test_save_reload_journey tests/test_save_reload_journey.c)
target_link_libraries(test_save_reload_journey cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_save_reload_journey PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_save_reload_journey PROPERTIES C_STANDARD 99)
target_link_libraries(test_save_reload_journey ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
foreach(_jr_case journey powercut)
	add_test(NAME test_save_reload_journey__${_jr_case}
		COMMAND test_save_reload_journey ${_jr_case})
	set_tests_properties(test_save_reload_journey__${_jr_case} PROPERTIES TIMEOUT 1800)
endforeach()
# The required save-path regressions, re-run with the deferred worker forced on
# (CW_G9_SAVE_IO=1, a region_archive.c test-build seam read before the first
# handle opens): the existing suites double as its regression net.
add_test(NAME test_region_index_journal_equiv__deferred COMMAND test_region_index_journal_equiv)
add_test(NAME test_region_archive_safety__deferred COMMAND test_region_archive_safety)
add_test(NAME test_death_drop_journal_env__deferred COMMAND test_death_drop_journal_env)
add_test(NAME test_owner_swap_dimension_checkpoint_reload__deferred
	COMMAND test_owner_swap_dimension_checkpoint_reload reload)
add_test(NAME test_autosave_entity_storm__deferred COMMAND test_autosave_entity_storm)
add_test(NAME test_a2_save_bytes_equiv__deferred COMMAND test_a2_save_bytes_equiv)
add_test(NAME test_light_queue_equiv__checkpoint_deferred COMMAND test_light_queue_equiv checkpoint)
add_test(NAME test_light_queue_equiv__hw_workload_deferred COMMAND test_light_queue_equiv hw_workload)
set_tests_properties(
	test_region_index_journal_equiv__deferred test_region_archive_safety__deferred
	test_death_drop_journal_env__deferred
	test_owner_swap_dimension_checkpoint_reload__deferred
	test_autosave_entity_storm__deferred test_a2_save_bytes_equiv__deferred
	test_light_queue_equiv__checkpoint_deferred
	test_light_queue_equiv__hw_workload_deferred
	PROPERTIES ENVIRONMENT "CW_G9_SAVE_IO=1" TIMEOUT 600)

# I07 lane 3 (G9 drain 0.2-1.1 s ticks on Wii): ONE harness, two engines --
# the production pools measure the real server_world_light_drain on the
# perf_stress_5m light_queue_stress worst case (bench_g9_drain, NDEBUG), the
# test pools pin its ordered write trace + counts + settled field to a frozen
# golden (test_g9_drain_equiv), so a T1 drain change proves byte-exactness.
foreach(target test_g9_drain_equiv bench_g9_drain)
	add_executable(${target} tests/bench_g9_drain.c
		tools/wii_stage_server/source/sha256.c)
	target_include_directories(${target} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/source
		${CMAKE_CURRENT_SOURCE_DIR}/third_party
		${CMAKE_CURRENT_SOURCE_DIR}/tests
		${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source
	)
	target_link_libraries(${target}
		${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
	set_target_properties(${target} PROPERTIES C_STANDARD 99)
endforeach()
target_link_libraries(test_g9_drain_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_link_libraries(bench_g9_drain cavex_prod_core cavex_prod_input)
target_compile_options(bench_g9_drain PRIVATE -DNDEBUG)
add_test(NAME test_g9_drain_equiv COMMAND test_g9_drain_equiv)

# Single-workload-thread CPU checkpoint benchmark; observe the existing
# stage brackets at link time without adding engine/Wii instrumentation.
add_executable(bench_g9_checkpoint tests/bench_g9_checkpoint.c)
target_include_directories(bench_g9_checkpoint PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(bench_g9_checkpoint cavex_prod_core cavex_prod_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_compile_options(bench_g9_checkpoint PRIVATE -DNDEBUG)
target_link_options(bench_g9_checkpoint PRIVATE
	-Wl,--wrap=g9_probe_begin -Wl,--wrap=g9_probe_end -Wl,--wrap=deflate -Wl,--wrap=region_archive_set_chunk_cached)
set_target_properties(bench_g9_checkpoint PROPERTIES C_STANDARD 99)

# Same observer, full production stress driver via threadless test entry seam.
add_executable(dump_g9_checkpoint_live tests/bench_g9_checkpoint.c)
target_include_directories(dump_g9_checkpoint_live PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(dump_g9_checkpoint_live PRIVATE CAVEWII_SERVER_LOCAL_TEST)
target_compile_options(dump_g9_checkpoint_live PRIVATE -DNDEBUG)
target_link_libraries(dump_g9_checkpoint_live PRIVATE
	cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_link_options(dump_g9_checkpoint_live PRIVATE
	-Wl,--wrap=g9_probe_begin -Wl,--wrap=g9_probe_end
	-Wl,--wrap=server_world_save_chunk_coherent -Wl,--wrap=clin_rpc_send -Wl,--wrap=deflate -Wl,--wrap=region_archive_set_chunk_cached
	-Wl,--wrap=inflate -Wl,--wrap=nbt_parse -Wl,--wrap=nbt_clone -Wl,--wrap=nbt_dump_binary_hint
	-Wl,--wrap=item_nbt_scope_writable -Wl,--wrap=item_nbt_clone_lossless -Wl,--wrap=tile_entities_build
	-Wl,--wrap=chunk_entities_build -Wl,--wrap=tile_entities_preserve -Wl,--wrap=chunk_entities_preserve
	-Wl,--wrap=nbt_free -Wl,--wrap=fopen)
set_target_properties(dump_g9_checkpoint_live PROPERTIES C_STANDARD 99)

# g9-ckpt-1009 (ASTRA-G9-R19-IO step 3): the settled-trial checkpoint's
# plane-copy snapshot / word-diff restore (T1) paired against the frozen
# per-cell reference: worlds, queues, RNG, ordered writes + publications,
# census, COW views and every saved region byte, plus the host split.
add_executable(test_g9_checkpoint_reuse_equiv tests/test_g9_checkpoint_reuse_equiv.c)
target_include_directories(test_g9_checkpoint_reuse_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_link_libraries(test_g9_checkpoint_reuse_equiv
	cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
set_target_properties(test_g9_checkpoint_reuse_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_g9_checkpoint_reuse_equiv COMMAND test_g9_checkpoint_reuse_equiv)
set_tests_properties(test_g9_checkpoint_reuse_equiv PROPERTIES TIMEOUT 900)

add_executable(test_g9_checkpoint_encode_equiv tests/test_g9_checkpoint_encode_equiv.c)
target_include_directories(test_g9_checkpoint_encode_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_g9_checkpoint_encode_equiv
	cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_link_options(test_g9_checkpoint_encode_equiv PRIVATE
	-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=mem2_alloc_strict)
set_target_properties(test_g9_checkpoint_encode_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_g9_checkpoint_encode_equiv COMMAND test_g9_checkpoint_encode_equiv)

# g9-savemiss-1009: exact checkpoint read-back memo vs the real disk read,
# all three IO modes, default codec and opt-in levels 1..5.
add_executable(test_g9_readback_memo_equiv tests/test_g9_readback_memo_equiv.c)
target_include_directories(test_g9_readback_memo_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_g9_readback_memo_equiv
	cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
set_target_properties(test_g9_readback_memo_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_g9_readback_memo_equiv COMMAND test_g9_readback_memo_equiv)
foreach(_zl RANGE 1 5)
	add_test(NAME test_g9_readback_memo_equiv__zlib${_zl} COMMAND test_g9_readback_memo_equiv)
	set_tests_properties(test_g9_readback_memo_equiv__zlib${_zl} PROPERTIES
		ENVIRONMENT "CW_G9_SAVE_ZLIB_LEVEL=${_zl}" TIMEOUT 900)
endforeach()

add_executable(test_g9_save_zlib tests/test_g9_save_zlib.c)
target_include_directories(test_g9_save_zlib PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_g9_save_zlib
	cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
set_target_properties(test_g9_save_zlib PROPERTIES C_STANDARD 99)
target_link_options(test_g9_save_zlib PRIVATE -Wl,--wrap=fwrite)
add_test(NAME test_g9_save_zlib COMMAND test_g9_save_zlib)
foreach(_zl RANGE 1 5)
	foreach(_suite test_g9_checkpoint_encode_equiv test_region_archive_safety test_region_index_journal_equiv)
		add_test(NAME ${_suite}__zlib${_zl} COMMAND ${_suite})
		set_tests_properties(${_suite}__zlib${_zl} PROPERTIES
			ENVIRONMENT "CW_G9_SAVE_ZLIB_LEVEL=${_zl}" TIMEOUT 900)
	endforeach()
	foreach(_case faults stalls powercut)
		add_test(NAME test_g9_save_io_durability__zlib${_zl}_${_case}
			COMMAND test_g9_save_io_durability ${_case})
		set_tests_properties(test_g9_save_io_durability__zlib${_zl}_${_case} PROPERTIES
			ENVIRONMENT "CW_G9_SAVE_ZLIB_LEVEL=${_zl}" TIMEOUT 900)
	endforeach()
	foreach(_case world session)
		add_test(NAME test_g9_save_io_equiv__zlib${_zl}_${_case}
			COMMAND test_g9_save_io_equiv ${_case})
		set_tests_properties(test_g9_save_io_equiv__zlib${_zl}_${_case} PROPERTIES
			ENVIRONMENT "CW_G9_SAVE_ZLIB_LEVEL=${_zl}" TIMEOUT 900)
	endforeach()
endforeach()
# g9-trial-1009 (T1): the light kernel paired against the frozen bf92f3e33
# flood on the cases the roof/torch golden does not reach (y edges, seams
# next to non-resident columns, Nether, probe census, priority lease, the
# checkpoint trial and its rollback); ordered writes + counts + arrays.
add_executable(test_g9_trial_kernel_equiv tests/test_g9_trial_kernel_equiv.c)
target_include_directories(test_g9_trial_kernel_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_g9_trial_kernel_equiv
	cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
set_target_properties(test_g9_trial_kernel_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_g9_trial_kernel_equiv COMMAND test_g9_trial_kernel_equiv)

# ASTRA-G9-R5 point 9/13: the perf_stress_5m budget gate reads the per-window
# rt/wt maxima (lqd v2), proven on the retained real g9_1 tape.
add_test(NAME test_g9_window_max_collector COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_g9_window_max_collector.py)
set_tests_properties(test_g9_window_max_collector PROPERTIES SKIP_RETURN_CODE 77) # R18: private G9 tape absent in the public export

# REL-MP-GATE: mp_lan_walk_10m/mp_connect_smoke FAIL mp_session_ended when the session ends or the
# world unloads after the login window, proven on the retained hw1018 (RED)
# and hw1014_e5 (GREEN) mp_lan_walk_10m tape excerpts.
add_test(NAME test_mp_session_gate_collector COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mp_session_gate_collector.py)

# REL-16-collector: soak_idle_play PC grader (SoakIdlePlayEvidence) on fake-time
# heartbeat series, the REL-16a core rules; fail-closed controls.
add_test(NAME test_soak_idle_play_collector COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_soak_idle_play_collector.py)

# REL-18e: allowlisted clean export (tools/clean_export.py) on a synthetic git
# repo: exact file set, IP/dev bait excluded, refusals, manifest verify.
add_test(NAME test_clean_export COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_clean_export.py)

# REL-18e2: exact export tree + release zip scanner (tools/release_scan.py) on
# synthetic trees: planted quotes/credentials/bait/missing records/hash drift.
add_test(NAME test_release_scan COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_scan.py)

# R18-E1: the REAL docs/release-allowlist.json exports from HEAD (sparse shared
# clone) with every required input and pin; maintainer 2026-10-30 docs/artwork scope.
add_test(NAME test_release_allowlist COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_allowlist.py)
set_tests_properties(test_release_allowlist PROPERTIES SKIP_RETURN_CODE 77)

# Tutorial-world toolchain (tools/tutorial_world): the Python NBT/McRegion/light
# writer's unit tests, which then write the demo save as the `tutorial_world`
# fixture; the C test loads that save through the real region_archive reader
# (blocks, metadata, light, tile entities) and re-saves it through the game's
# own writer. Not labelled cw_tools: a loader change must still run the C half.
if(Python3_Interpreter_FOUND)
	add_test(NAME test_tutorial_worldgen_lib COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tutorial_worldgen_lib.py
		--fixture-out ${CMAKE_CURRENT_BINARY_DIR}/tutorial_world_fixture)
	set_tests_properties(test_tutorial_worldgen_lib PROPERTIES
		FIXTURES_SETUP tutorial_world)
	add_executable(test_tutorial_world_load
		source/network/entity_archive.c
		source/network/region_archive.c
		source/network/spawner_mob_id.c
		source/network/tile_entities_nbt.c
		source/platform/thread.c
		source/platform/time.c
		source/platform/mem2.c
		source/cNBT/buffer.c
		source/cNBT/nbt_loading.c
		source/cNBT/nbt_parsing.c
		source/cNBT/nbt_treeops.c
		source/cNBT/nbt_util.c
		source/item/item_nbt.c
		tests/test_tutorial_world_load.c)
	target_compile_definitions(test_tutorial_world_load PRIVATE PLATFORM_PC)
	target_include_directories(test_tutorial_world_load PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/source
		${CMAKE_CURRENT_SOURCE_DIR}/third_party
		${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_link_libraries(test_tutorial_world_load m ZLIB::ZLIB
		${CMAKE_THREAD_LIBS_INIT})
	add_test(NAME test_tutorial_world_load COMMAND test_tutorial_world_load
		${CMAKE_CURRENT_BINARY_DIR}/tutorial_world_fixture)
	set_tests_properties(test_tutorial_world_load PROPERTIES
		FIXTURES_REQUIRED tutorial_world)
	# tutorial-1021: the shipped tutorial world build script (whole 1024-chunk
	# world in a tmp dir: problems, lit walkable cells, signs, chests, determinism).
	add_test(NAME test_tutorial_build COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tutorial_build.py)
	set_tests_properties(test_tutorial_build PROPERTIES TIMEOUT 900)
	# The SHIPPED world (the one tools/make_release.sh packages): build it into
	# the binary dir (~30 s), then load every chunk, check sign/chest tile
	# entities against the builder's --summary, move a pen animal and break a
	# chest through the game's own region reader/writer.
	add_test(NAME test_tutorial_world_real_build COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tools/tutorial_world/build_tutorial.py
		--out "${CMAKE_CURRENT_BINARY_DIR}/tutorial_world_real/CaveWii Tutorial"
		--summary ${CMAKE_CURRENT_BINARY_DIR}/tutorial_world_real/summary.txt)
	set_tests_properties(test_tutorial_world_real_build PROPERTIES
		FIXTURES_SETUP tutorial_world_real TIMEOUT 600)
	add_test(NAME test_tutorial_world_real_load COMMAND test_tutorial_world_load
		--real ${CMAKE_CURRENT_BINARY_DIR}/tutorial_world_real)
	set_tests_properties(test_tutorial_world_real_load PROPERTIES
		FIXTURES_REQUIRED tutorial_world_real)
endif()

# ASTRA-G9-R5 points 5-6 / order 2 (point 11): the server's RPC publication
# as a bounded, owned, ordered pending output over a nonblocking transport.
add_executable(test_rpc_pending_output tests/test_rpc_pending_output.c)
target_link_libraries(test_rpc_pending_output cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_rpc_pending_output PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_rpc_pending_output PROPERTIES C_STANDARD 99)
target_link_libraries(test_rpc_pending_output ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
foreach(_rpc_c stalled saturation mixed generation exhaustion shutdown ownership simulation fifo_bypass terminal_kept cancel oom_recovery deadline chunk_busy telemetry_race burst pack_barrier pack_exhaustion)
	add_test(NAME test_rpc_pending_output__${_rpc_c} COMMAND test_rpc_pending_output ${_rpc_c})
endforeach()

# next-gaps-0924/W3-a2r: world-free Beta block point-ray geometry.
add_executable(test_block_ray source/entity/block_ray.c tests/test_block_ray.c)
target_include_directories(test_block_ray PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_block_ray m)
add_test(NAME test_block_ray COMMAND test_block_ray)

# Group E of the 2026-06-19 mob-AI parity audit: player i-frame excess-damage rule +
# Beta 3D distance-scaled blast knockback, over a real server_local + superflat world.
add_executable(test_player_combat_env tests/test_player_combat_env.c)
target_link_libraries(test_player_combat_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_player_combat_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_player_combat_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_player_combat_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_player_combat_env COMMAND test_player_combat_env)

# Cactus SIDE-contact parity (2026-09-07, maintainer far_walk observation): the
# Beta Entity.moveEntity post-move cell sweep replacing the eye/feet point
# sample, plus BlockCactus's 1/16 collision-box insets (X/Z and the TOP one that
# makes standing on a cactus hurt), over a real server_local + superflat world.
add_executable(test_cactus_contact_damage tests/test_cactus_contact_damage.c)
target_link_libraries(test_cactus_contact_damage cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_cactus_contact_damage PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_cactus_contact_damage PROPERTIES C_STANDARD 99)
target_link_libraries(test_cactus_contact_damage ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_cactus_contact_damage COMMAND test_cactus_contact_damage)

# Beta 1.7.3 in-world item drop ('Q'): golden over a real server_local + superflat world.
# Pins EntityPlayer.dropCurrentItem semantics -- drop one from the current slot / drop the
# stack / empty-hand no-op / eye-0.18 spawn / exact Beta throw velocity within scatter bounds.
add_executable(test_player_item_drop tests/test_player_item_drop.c)
target_link_libraries(test_player_item_drop cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_player_item_drop PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_player_item_drop PROPERTIES C_STANDARD 99)
target_link_libraries(test_player_item_drop ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_player_item_drop COMMAND test_player_item_drop)

# Beta 1.7.3 sneak ledge-stop: golden over a real server world + a floating platform. Pins
# Entity.moveEntity's onGround && isSneaking edge clamp (per-axis + diagonal + no-false-clamp).
add_executable(test_player_sneak_env tests/test_player_sneak_env.c)
target_link_libraries(test_player_sneak_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_player_sneak_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_player_sneak_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_player_sneak_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_player_sneak_env COMMAND test_player_sneak_env)

# Near resident evict must never block the server thread on a full client RPC
# inbox. Fills the client token pool, then proves CRPC_UNLOAD_CHUNK is queued and
# later flushed through the production near-unload primitive.
add_executable(test_near_evict_defer tests/test_near_evict_defer.c)
target_link_libraries(test_near_evict_defer cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_near_evict_defer PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_near_evict_defer PROPERTIES C_STANDARD 99)
target_link_libraries(test_near_evict_defer ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_near_evict_defer COMMAND test_near_evict_defer)

# Beta 1.7.3 bone meal (ItemDye.onItemUse dmg 15) on crops -> metadata 7; also covers the new
# item onUseOnBlock dispatch hook. Golden over a real server world.
add_executable(test_bonemeal tests/test_bonemeal.c)
target_link_libraries(test_bonemeal cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_bonemeal PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_bonemeal PROPERTIES C_STANDARD 99)
target_link_libraries(test_bonemeal ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_bonemeal COMMAND test_bonemeal)

# Beta 1.7.3 hoe tilling (ItemHoe.onItemUse) on the onUseOnBlock hook: dirt any-face,
# grass top+air-above, no-op otherwise, hoe never consumed. Golden over a real server world.
add_executable(test_hoe tests/test_hoe.c)
target_link_libraries(test_hoe cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_hoe PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_hoe PROPERTIES C_STANDARD 99)
target_link_libraries(test_hoe ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_hoe COMMAND test_hoe)

# Beta 1.7.3 flint & steel (ItemFlintAndSteel.onItemUse): light fire in the adjacent air cell
# regardless of the clicked block's flammability (the prior flammable gate was a deviation).
add_executable(test_flint_steel tests/test_flint_steel.c)
target_link_libraries(test_flint_steel cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_flint_steel PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_flint_steel PROPERTIES C_STANDARD 99)
target_link_libraries(test_flint_steel ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_flint_steel COMMAND test_flint_steel)

# Beta 1.7.3 TOOL DURABILITY (handoff 2026-07-29 P0(d)+P1(b)). Two defects, one test: the 25
# tier constants stored Beta's maxDamage PLUS ONE, and the wear was never applied at all
# (item_stack_damage had 3 call sites tree-wide, none of them block-break or melee). Pins the
# EnumToolMaterial constants, the strict-`>` break boundary, the per-(class x path) wear rule
# table, and -- the trap docs/briefs/01_durability_combat.md got WRONG -- that a break which
# drops nothing STILL wears, because Beta's harvest gate guards only the drop.
add_executable(test_tool_durability_env tests/test_tool_durability_env.c)
target_link_libraries(test_tool_durability_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_tool_durability_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_tool_durability_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_tool_durability_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
# WORKING_DIRECTORY = the source root: check [9] scans server_local.c by relative path,
# because client_interface.h exposes no receive/peek API to intercept the RPC queue with.
add_test(NAME test_tool_durability_env COMMAND test_tool_durability_env
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Dispatch-compatible name for the same golden.  The implementation-bearing
# source remains test_tool_durability_env.c so the two names cannot drift.
add_executable(test_tool_wear_env tests/test_tool_wear_env.c)
target_link_libraries(test_tool_wear_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_tool_wear_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_tool_wear_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_tool_wear_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_tool_wear_env COMMAND test_tool_wear_env
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Beta 1.7.3 ItemArmor/InventoryPlayer/EntityPlayer damage ordering over the
# real server damage paths.  The armor helper is intentionally independent of
# the RPC layer so its integer arithmetic remains directly testable.
add_executable(test_armor_mitigation_env tests/test_armor_mitigation_env.c)
target_link_libraries(test_armor_mitigation_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_armor_mitigation_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_armor_mitigation_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_armor_mitigation_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_armor_mitigation_env COMMAND test_armor_mitigation_env)

# Beta 1.7.3 weather (World.updateWeather / updateBlocksAndPlayCaveSounds /
# canBlockBeRainedOn / findTopSolidBlock, WorldInfo, BlockFire/BlockFarmland's
# rain branches, BlockSnow/BlockIce melt, EntityLightningBolt,
# EntityCreeper/EntityPig.onStruckByLightning) -- WEATHER_DISPATCH.md.
# Class-guard for uninitialized server_local fields: the app's struct lives on
# main()'s stack, so init must establish every free()-safe invariant itself
# (the 2026-08-14 weather-bolt world-load crash). Poisons with 0xAA, then runs
# real init + the load path's weather teardown.
add_executable(test_server_struct_poison_env tests/test_server_struct_poison_env.c)
target_link_libraries(test_server_struct_poison_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_server_struct_poison_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_server_struct_poison_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_server_struct_poison_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_server_struct_poison_env COMMAND test_server_struct_poison_env)

add_executable(test_weather_timers_env tests/test_weather_timers_env.c)
target_link_libraries(test_weather_timers_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_weather_timers_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_weather_timers_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_weather_timers_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_weather_timers_env COMMAND test_weather_timers_env)

add_executable(test_weather_snow_ice_env tests/test_weather_snow_ice_env.c)
target_link_libraries(test_weather_snow_ice_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_weather_snow_ice_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_weather_snow_ice_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_weather_snow_ice_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_weather_snow_ice_env COMMAND test_weather_snow_ice_env)

add_executable(test_lightning_env tests/test_lightning_env.c)
target_link_libraries(test_lightning_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_lightning_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_lightning_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_lightning_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_lightning_env COMMAND test_lightning_env)

# next-gaps-0924/W1F: Beta 1.7.3 difficulty goldens -- Peaceful regen / hostile
# removal / spawn skip, EntityMob+skeleton-arrow damage scaling, lightning fire
# gate, config persistence (GameSettings/EntityPlayer/EntityMob/EntityGhast).
add_executable(test_difficulty_golden tests/test_difficulty_golden.c)
target_link_libraries(test_difficulty_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_difficulty_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_difficulty_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_difficulty_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_difficulty_golden COMMAND test_difficulty_golden)

# next-gaps-0924/W3-s1: Beta EntityLiving.heal sets heartsLife = heartsHalvesLife/2 -- every
# heal path (ItemFood, BlockCake, EntityWolf feeding, Peaceful regen) closes the hurt window.
add_executable(test_heal_hurt_window_golden tests/test_heal_hurt_window_golden.c)
target_link_libraries(test_heal_hurt_window_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_heal_hurt_window_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_heal_hurt_window_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_heal_hurt_window_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_heal_hurt_window_golden COMMAND test_heal_hurt_window_golden)

# next-gaps-0924/W3-s4: the world-spawn fallback push-up is Beta Entity.preparePlayerToSpawn --
# World.getCollidingBoundingBoxes block boxes (not plain air) on the POPULATED spawn chunk.
add_executable(test_spawn_fallback_collision_golden tests/test_spawn_fallback_collision_golden.c)
target_link_libraries(test_spawn_fallback_collision_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_spawn_fallback_collision_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_spawn_fallback_collision_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_spawn_fallback_collision_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_spawn_fallback_collision_golden COMMAND test_spawn_fallback_collision_golden)

# Beta 1.7.3 buckets (ItemBucket.onItemRightClick): empty bucket fills from a water/lava SOURCE
# (material + metadata 0), filled buckets empty into the canonical adjacent cell. Golden over a
# real server world + player inventory; pins the source-metadata check + the FRONT/BACK swap fix.
add_executable(test_bucket tests/test_bucket.c)
target_link_libraries(test_bucket cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_bucket PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_bucket PROPERTIES C_STANDARD 99)
target_link_libraries(test_bucket ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_bucket COMMAND test_bucket)

# Beta 1.7.3 shears block-break drop (ItemShears/BlockLeaves.harvestBlock): leaves broken with
# shears drop the leaf block (metadata & 3), else the normal sapling roll. Pure unit test over the
# item_shears_block_drop helper -- no world/inventory, so a minimal 2-object link.
add_executable(test_shears tests/test_shears.c source/item/items/item_shears.c)
target_compile_definitions(test_shears PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_shears PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_shears PROPERTIES C_STANDARD 99)
target_link_libraries(test_shears m)
add_test(NAME test_shears COMMAND test_shears)

# M4 pistons (phase 2b): E2E production-path test for the piston MOVE -- the synchronous dispatch, the
# in-RAM moving-TE store, and the T+2 settle driver over a real superflat server_world. Validates the
# engine wiring the stub goldens can't: the real power-query side semantics, the id-36 settle window,
# the tile-entity/bedrock/12-limit refusals, the BUD quirk, and the sticky pull.
add_executable(test_piston_move_env tests/test_piston_move_env.c)
target_link_libraries(test_piston_move_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_piston_move_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_piston_move_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_piston_move_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_piston_move_env COMMAND test_piston_move_env)

add_executable(test_offered_guest_presence tests/test_offered_guest_presence.c)
target_link_libraries(test_offered_guest_presence
	cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_offered_guest_presence PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_offered_guest_presence PROPERTIES C_STANDARD 99)
target_link_libraries(test_offered_guest_presence ${CMAKE_THREAD_LIBS_INIT}
	ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_offered_guest_presence COMMAND test_offered_guest_presence)
set_tests_properties(test_offered_guest_presence PROPERTIES TIMEOUT 120)

# Both split actors act and complete pistons in separate dimensions.
add_executable(test_piston_world_isolation tests/test_piston_world_isolation.c)
target_link_libraries(test_piston_world_isolation cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_piston_world_isolation PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_piston_world_isolation PROPERTIES C_STANDARD 99)
target_link_libraries(test_piston_world_isolation ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
foreach(_pwi_case host_nether guest_nether)
	add_test(NAME test_piston_world_isolation_${_pwi_case}
		COMMAND test_piston_world_isolation ${_pwi_case})
	set_tests_properties(test_piston_world_isolation_${_pwi_case}
		PROPERTIES TIMEOUT 60)
endforeach()

# M4 pistons (phase 5): E2E for the head-removal behaviours -- mining an extended head drops+clears the
# powered base (Beta BlockPistonExtension.onBlockRemoval) and an orphaned head self-removes when its base
# is gone (onNeighborBlockChange), through the production dig path + neighbour-notify cascade.
add_executable(test_piston_head_break_env tests/test_piston_head_break_env.c)
target_link_libraries(test_piston_head_break_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_piston_head_break_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_piston_head_break_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_piston_head_break_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_piston_head_break_env COMMAND test_piston_head_break_env)

# M4 pistons (phase 3): E2E for NBT PERSISTENCE -- an in-flight piston move must survive
# save -> evict -> reload and settle at the correct remaining tick (the Beta 'Piston' progress
# round-trip via te_store gather/insert + the region file), instead of orphaning into a phantom.
add_executable(test_piston_reload_env tests/test_piston_reload_env.c)
target_link_libraries(test_piston_reload_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_piston_reload_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_piston_reload_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_piston_reload_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_piston_reload_env COMMAND test_piston_reload_env)

# HEADLESS repro for the on-device "place a base piston on the ground -> hard HANG"
# bug. Drives the REAL client chunk mesher worker (chunk_mesher_send -> the per-face
# render_block_full emit -> the real displaylist + displaylist_finalize) on a chunk
# containing a base/sticky/head piston on solid ground, with a wall-clock watchdog
# so a genuine hang fails the test instead of wedging CI. Re-derives the Wii
# displaylist_finalize stream invariant (written == vtxcnt*stride) on PC against the
# real finalized lists. Normal builds must not truncate/stale; CASE E injects five
# allocation failures and proves incomplete replacement lists stay out of rendering.
# A clean pass localizes the hang to the GX finalize/draw path (no PC equivalent),
# not the mesher's C emit logic.
add_executable(test_piston_chunk_mesh_env tests/test_piston_chunk_mesh_env.c)
target_link_libraries(test_piston_chunk_mesh_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_piston_chunk_mesh_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_piston_chunk_mesh_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_piston_chunk_mesh_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_piston_chunk_mesh_env COMMAND test_piston_chunk_mesh_env)

# C1 (candidate, hardware-unconfirmed, 2026-09-02): chunk_mesher_shutdown() proof.
# Drives the REAL chunk_mesher worker (chunk_mesher_init -> chunk_mesher_send on
# real chunks -> chunk_mesher_shutdown) on platform/thread.c's pthread-backed PC
# implementation. Submits a chunk backlog without waiting for any of it, then
# asserts chunk_mesher_shutdown() returns within a bounded wall-clock time (no
# hang), that a redundant call is idempotent, and that chunk_mesher_receive()
# stays harmless afterward. Same harness shape as test_piston_chunk_mesh_env
# above (clin drain thread; the client world is independent of any server).
add_executable(test_chunk_mesher_shutdown_env tests/test_chunk_mesher_shutdown_env.c)
target_link_libraries(test_chunk_mesher_shutdown_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_chunk_mesher_shutdown_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_chunk_mesher_shutdown_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_chunk_mesher_shutdown_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_chunk_mesher_shutdown_env COMMAND test_chunk_mesher_shutdown_env)

# T1-d (2026-09-03 far-ring frame-drop root-cause lane): golden proof that
# CHUNK_MESHER_ENQUEUE_CAP is pure pacing -- a burst past the cap defers
# (never drops) the excess, and every chunk's final mesh is byte-identical
# to the same chunk meshed without contention, only submitted later. Same
# harness shape as test_chunk_mesher_shutdown_env above.
add_executable(test_chunk_mesher_enqueue_cap_env tests/test_chunk_mesher_enqueue_cap_env.c)
target_link_libraries(test_chunk_mesher_enqueue_cap_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_chunk_mesher_enqueue_cap_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_chunk_mesher_enqueue_cap_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_chunk_mesher_enqueue_cap_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_chunk_mesher_enqueue_cap_env COMMAND test_chunk_mesher_enqueue_cap_env)

# P2-b / T1-e (2026-09-03 round-2 far-ring frame-drop root-cause doc, "#2
# Post-load spikes"): golden proof that the cooperative slice added to
# chunk_mesher_build (thread_msleep(1) after each finalized list once the
# build runs past CHUNK_MESHER_SLICE_MS) is pure pacing -- forcing the slice
# to fire after EVERY finalized list (via the CAVEWII_DISPLAYLIST_TEST-gated
# chunk_mesher_test_set_slice_ms seam) produces byte-identical mesh output to
# the same chunk built with the slice forced never to fire. Same harness
# shape as test_chunk_mesher_enqueue_cap_env above.
add_executable(test_chunk_mesher_slice_env tests/test_chunk_mesher_slice_env.c)
target_link_libraries(test_chunk_mesher_slice_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_chunk_mesher_slice_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_chunk_mesher_slice_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_chunk_mesher_slice_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_chunk_mesher_slice_env COMMAND test_chunk_mesher_slice_env)

# DC-GRASS G1 (design-crit-1010): Beta 1.7.3 grass parity regressions -- real
# tex_atlas_block on a SYNTHETIC terrain.png (written to a pid fixture dir) +
# the REAL chunk_mesher worker; faces composited from the emitted lists in
# chunk_render draw order. Same harness shape as test_chunk_mesher_slice_env.
add_executable(test_grass_render tests/test_grass_render.c)
target_link_libraries(test_grass_render cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_grass_render PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_grass_render PROPERTIES C_STANDARD 99)
target_link_libraries(test_grass_render ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_grass_render COMMAND test_grass_render)

# design-crit-1010 DC-GRASS G3: PC terrain atlas sampler policy. Compiles the
# real source/platform/pc/texture.c (#included by the test) against a GL 1.1
# recorder defined in the test -- no OpenGL link -- so it reads the actual
# filter/level state requested on load, cache restore and pack reload.
add_executable(test_grass_filter
	source/platform/texture_mip.c
	tests/test_grass_filter.c
)
target_link_libraries(test_grass_filter m)
target_include_directories(test_grass_filter PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	$<TARGET_PROPERTY:GLEW::GLEW,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_definitions(test_grass_filter PRIVATE PLATFORM_PC)
set_target_properties(test_grass_filter PROPERTIES C_STANDARD 99)
add_test(NAME test_grass_filter COMMAND test_grass_filter)

# design-crit-1010 DC-GRASS G2 (T1 byte-exact proof): every NON-grass block's
# emitted chunk display-list bytes and atlas slot/cell texels stay identical
# to the golden recorded from base_ref faeed2b58 (pre-G2). Re-record only from
# a pre-G2 tree: test_mesher_nongrass_equiv <golden> --record.
add_executable(test_mesher_nongrass_equiv tests/test_mesher_nongrass_equiv.c)
target_link_libraries(test_mesher_nongrass_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mesher_nongrass_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_mesher_nongrass_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_mesher_nongrass_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mesher_nongrass_equiv COMMAND test_mesher_nongrass_equiv
	${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/grass/mesher_nongrass_equiv.golden)

# Beta leaves follow render.fancy_graphics (2026-10-30, round-2 "spruce leaf
# noise"): fast = opaque tile 53/133 + culled inner faces (default), fancy =
# cut-out + every face (BlockLeaves.setGraphicsLevel); fast-tile fallback for
# packs without it; prints the taiga/forest mesh cost of both (MEASURE).
add_executable(test_leaves_graphics_level tests/test_leaves_graphics_level.c)
target_link_libraries(test_leaves_graphics_level cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_leaves_graphics_level PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_leaves_graphics_level PROPERTIES C_STANDARD 99)
target_link_libraries(test_leaves_graphics_level ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_leaves_graphics_level COMMAND test_leaves_graphics_level)

# H-2 (2026-09-03 maintainer ruling, responsiveness lane): golden + scheduling
# proof that world_build_chunks' band priority (band 0 urgent, band 1 near,
# band 2 far) is pure ordering -- band 2 is fully starved while any band-1
# chunk is unmeshed, band 0 jumps ahead of an equally-dirty band-1 chunk under
# a scarce token budget, and every mesh produced under gating is byte-
# identical to the same chunk meshed with no gating at all. Same harness
# shape as test_chunk_mesher_enqueue_cap_env above.
add_executable(test_mesher_band_priority tests/test_mesher_band_priority.c)
target_link_libraries(test_mesher_band_priority cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mesher_band_priority PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mesher_band_priority PROPERTIES C_STANDARD 99)
target_link_libraries(test_mesher_band_priority ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mesher_band_priority COMMAND test_mesher_band_priority)

# REL-MP-COREFIRST (2026-10-22): the remote-loading focus band is pure
# scheduling order -- focus off (local/in-game) sends in exactly the hash-walk
# order, focus on sends the 3x3 core then nearer rings first, the core meshes
# within ceil(core/8)+1 frames under an edit storm, and every mesh is
# byte-identical across both orders and to a standalone re-mesh.
add_executable(test_mesher_core_first tests/test_mesher_core_first.c)
target_link_libraries(test_mesher_core_first cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mesher_core_first PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mesher_core_first PROPERTIES C_STANDARD 99)
target_link_libraries(test_mesher_core_first ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mesher_core_first COMMAND test_mesher_core_first)
set_tests_properties(test_mesher_core_first PROPERTIES TIMEOUT 300)

# T2 SPIKE (opt-in, PC-measured only; hardware-unconfirmed -- see
# docs/far_ring_geomip_design_2026-08-26.md): end-to-end equivalence + merge proof
# for the far-ring geomip merge, driving the REAL client chunk_mesher worker (same
# harness shape as test_piston_chunk_mesh_env above). CASE A pins the flag-off
# (default) emitted vertex count to an exact golden; CASE B proves geomip-on
# merges flat terrain 4x; CASE C proves the screen-space error budget holds back
# one over-error patch while merging the rest.
add_executable(test_far_geomip_flagoff_equiv tests/test_far_geomip_flagoff_equiv.c)
target_link_libraries(test_far_geomip_flagoff_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_far_geomip_flagoff_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_far_geomip_flagoff_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_far_geomip_flagoff_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_far_geomip_flagoff_equiv COMMAND test_far_geomip_flagoff_equiv)

# Stage-2 LOD S2 far-storage compression (docs/stage2_lod_plan_2026-09-03.md;
# world.h's enum world_far_storage; chunk.h's struct far_column_skin): the
# faithfulness proof that "lod.far_storage":"skin" emits byte-identical
# display lists to the default "voxel" storage, driving the REAL client
# chunk_mesher worker (same harness shape as test_far_geomip_flagoff_equiv
# above). CASE A is a seed x column-pattern matrix (stepped heights, trees,
# water, snow, ice, an all-air column); CASE B adds a real near-chunk seam;
# CASE C asserts is_far_lod + the open-sky self-consistency invariant; CASE D
# is a far-evict/far<->full-promotion churn test; CASE E is an informational
# PC-only gather-cost signal (not a hardware cert -- see CLAUDE.md's perf rules).
add_executable(test_far_storage_equiv tests/test_far_storage_equiv.c)
target_link_libraries(test_far_storage_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_far_storage_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_far_storage_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_far_storage_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_far_storage_equiv COMMAND test_far_storage_equiv)

# T2 SPIKE (opt-in, PC-measured only; hardware-unconfirmed): measures the far-ring
# geomip merge against the REAL client chunk_mesher worker over a deterministic
# staircase terrain, flag-off vs three error budgets. Not a pass/fail gate (like
# bench_mesher, bench_worldgen, ...) -- always exits 0; read its stdout. Excluded
# from the default `ctest` run the same way (no add_test) so its numbers don't
# flap the suite on a noisy CI runner.
add_executable(bench_far_geomip tests/bench_far_geomip.c)
target_link_libraries(bench_far_geomip cavex_prod_core cavex_prod_input)
target_include_directories(bench_far_geomip PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(bench_far_geomip PROPERTIES C_STANDARD 99)
target_link_libraries(bench_far_geomip ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)

# Stage-2 LOD S2 far-storage compression: microbenchmark for
# chunk_get_block's per-cell cost on an ordinary near/full chunk (the
# instrument that caught + proved the far_skin-branch hoist fix -- see
# chunk_mesher.c's chunk_mesher_send and the S2 lane commits). Not a ctest
# target -- read its stdout like bench_mesher/bench_far_geomip.
add_executable(bench_chunk_get_block tests/bench_chunk_get_block.c)
target_link_libraries(bench_chunk_get_block cavex_prod_core cavex_prod_input)
target_include_directories(bench_chunk_get_block PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(bench_chunk_get_block PROPERTIES C_STANDARD 99)
target_link_libraries(bench_chunk_get_block ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)

# Production furnace-fuel guard: boots the real block registry and asserts
# env_fuel_ticks (Beta getItemBurnTime) per id -- catches a block whose getMaterial
# wrongly == MATERIAL_WOOD becoming furnace fuel (the torch + pumpkin regressions).
add_executable(test_furnace_fuel_env tests/test_furnace_fuel_env.c)
target_link_libraries(test_furnace_fuel_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_furnace_fuel_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_furnace_fuel_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_furnace_fuel_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_furnace_fuel_env COMMAND test_furnace_fuel_env)

add_executable(test_furnace_break_scatter tests/test_furnace_break_scatter.c)
target_link_libraries(test_furnace_break_scatter cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_furnace_break_scatter PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/tests
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_furnace_break_scatter PROPERTIES C_STANDARD 99)
target_link_libraries(test_furnace_break_scatter ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_furnace_break_scatter COMMAND test_furnace_break_scatter)

# Beta 1.7.3 tree decorate step (worldgen_trees) -- oak/birch/taiga/taiga-alt
# placement + per-species wood/leaf metadata + byte-exact RNG consumption.
add_executable(test_tree
	source/worldgen/java_random.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	tests/test_tree.c
)
target_include_directories(test_tree PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_tree m)
add_test(NAME test_tree COMMAND test_tree)

# Beta 1.7.3 BlockSapling.growTree (worldgen_grow_sapling): species selection
# (oak/birch/spruce + 1-in-10 big oak), correct wood/leaf metadata, deterministic
# geometry, and restore-on-fail. Pure worldgen entry over a flat buffer + seeded
# java_random (the byte-exact geometry guard behind the runtime bone-meal/random-
# tick growth, which can't be byte-exact -- see source/worldgen/tree_runtime.h).
add_executable(test_grow_tree
	source/worldgen/java_random.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	tests/test_grow_tree.c
)
target_include_directories(test_grow_tree PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_grow_tree m)
add_test(NAME test_grow_tree COMMAND test_grow_tree)

# Beta 1.7.3 small decorations (worldgen_decorate) -- flowers/grass/mushrooms/
# sugar cane/pumpkins/cacti/springs placement + byte-exact RNG consumption.
add_executable(test_decorate
	source/worldgen/java_random.c
	source/worldgen/decorate.c
	tests/test_decorate.c
)
target_include_directories(test_decorate PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_decorate m)
add_test(NAME test_decorate COMMAND test_decorate)

# Faithfulness guard: brick (and other crafted-only) blocks NEVER generate
# naturally. Sweeps worldgen_populate + worldgen_dungeon and asserts none appear.
add_executable(test_no_natural_bricks
	source/worldgen/java_random.c
	source/worldgen/populate.c
	source/worldgen/decorate.c
	source/worldgen/biome.c
	source/worldgen/tree.c
	source/worldgen/minable.c
	source/worldgen/beta_math.c
	tests/test_no_natural_bricks.c
)
target_include_directories(test_no_natural_bricks PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_no_natural_bricks m)
add_test(NAME test_no_natural_bricks COMMAND test_no_natural_bricks)

# WORLDGEN/REED (2026-09-26): real-terrain seeded sweep -- every natural sugar
# cane obeys Beta BlockReed.canPlaceBlockAt at write time (reed/grass/dirt below,
# Material.water 8|9 beside the base). Same worldgen link set as test_far_skin_equiv.
add_executable(test_reed_placement_sweep
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/minable.c
	source/worldgen/populate.c
	source/worldgen/tree.c
	source/worldgen/decorate.c
	source/worldgen/worldgen.c
	source/worldgen/worldgen_hell.c
	source/platform/mem2.c
	source/platform/time.c
	tests/test_reed_placement_sweep.c
)
target_compile_definitions(test_reed_placement_sweep PRIVATE PLATFORM_PC)
target_include_directories(test_reed_placement_sweep PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_reed_placement_sweep m)
add_test(NAME test_reed_placement_sweep COMMAND test_reed_placement_sweep)

# DECOR/1 (2026-09-26): seeded sweep over REAL terrain -- every populate write
# (lakes/dungeons/clay/ores/trees/plants/springs/snow) is checked against its
# Beta 1.7.3 placement rule at write time. Every clause is strict, including
# REED_ON_SAND (its EXPECTED-FAIL carve-out was removed at G4 assembly once
# codex/reed-0926 landed); any violation is a hard FAIL. Coverage table:
# tmp/jobs/split-dimensions-plan-0924/out/DECOR-COVERAGE-0926.md.
add_executable(test_decoration_placement_sweep
	source/worldgen/java_random.c
	source/worldgen/noise.c
	source/worldgen/simplex.c
	source/worldgen/biome.c
	source/worldgen/beta_math.c
	source/worldgen/cave.c
	source/worldgen/worldgen.c
	source/worldgen/worldgen_hell.c
	source/worldgen/populate.c
	source/worldgen/decorate.c
	source/worldgen/tree.c
	source/worldgen/minable.c
	source/platform/mem2.c
	tests/test_decoration_placement_sweep.c
)
target_include_directories(test_decoration_placement_sweep PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_decoration_placement_sweep PRIVATE PLATFORM_PC)
target_link_libraries(test_decoration_placement_sweep m)
add_test(NAME test_decoration_placement_sweep
	COMMAND test_decoration_placement_sweep)

add_executable(test_world_redraw_sparse
	source/world_redraw.c
	tests/test_world_redraw_sparse.c
)
target_link_libraries(test_world_redraw_sparse m)
target_include_directories(test_world_redraw_sparse PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_world_redraw_sparse PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED
)
target_compile_options(test_world_redraw_sparse PRIVATE
	-ffunction-sections -fdata-sections
)
target_link_options(test_world_redraw_sparse PRIVATE
	-Wl,--gc-sections
)
add_test(NAME test_world_redraw_sparse COMMAND test_world_redraw_sparse)

add_executable(test_chunk_generation
	tests/test_chunk_generation.c
)
target_link_libraries(test_chunk_generation m)
target_include_directories(test_chunk_generation PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_chunk_generation PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED
)
add_test(NAME test_chunk_generation COMMAND test_chunk_generation)

add_executable(test_scheduled_tick_queue
	source/network/server_world_tick_queue.c
	tests/test_scheduled_tick_queue.c
)
target_link_libraries(test_scheduled_tick_queue m)
target_include_directories(test_scheduled_tick_queue PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_scheduled_tick_queue PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED
)
add_test(NAME test_scheduled_tick_queue COMMAND test_scheduled_tick_queue)

# M4 redstone (R2): scheduled-tick (x,y,z,block_type) DEDUP -- Beta's scheduledTickSet.contains
# gate. Duplicate notifies collapse to one pending entry (cannot overflow the ring + drop a clock
# tick); distinct pos/id stay separate; the first-inserted entry/time is kept; explosions exempt.
add_executable(test_scheduled_tick_dedup_golden
	source/network/server_world_tick_queue.c
	source/platform/mem2.c
	tests/test_scheduled_tick_dedup_golden.c
)
target_link_libraries(test_scheduled_tick_dedup_golden m)
target_include_directories(test_scheduled_tick_dedup_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_scheduled_tick_dedup_golden PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED
)
add_test(NAME test_scheduled_tick_dedup_golden COMMAND test_scheduled_tick_dedup_golden)

add_executable(test_profile_random_ticks_default
	tests/test_profile_random_ticks.c
)
target_link_libraries(test_profile_random_ticks_default m)
target_include_directories(test_profile_random_ticks_default PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_compile_definitions(test_profile_random_ticks_default PRIVATE
	EXPECTED_RANDOM_TICKS=80
	EXPECTED_MAX_EXPLOSIONS=4
)
add_test(NAME test_profile_random_ticks_default
	COMMAND test_profile_random_ticks_default
)

add_executable(test_profile_random_ticks_low
	tests/test_profile_random_ticks.c
)
target_link_libraries(test_profile_random_ticks_low m)
target_include_directories(test_profile_random_ticks_low PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_compile_definitions(test_profile_random_ticks_low PRIVATE
	FCAVEX_PROFILE_WII_LOW EXPECTED_RANDOM_TICKS=20
	EXPECTED_MAX_EXPLOSIONS=2
)
add_test(NAME test_profile_random_ticks_low
	COMMAND test_profile_random_ticks_low
)

add_executable(test_explosion_caps
	source/network/server_world_tick_queue.c
	tests/test_explosion_caps.c
)
target_link_libraries(test_explosion_caps m)
target_include_directories(test_explosion_caps PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_explosion_caps PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED
)
add_test(NAME test_explosion_caps COMMAND test_explosion_caps)

# B7 regression: a blast that breaks MORE than 512 unique blocks must break
# all of them. The old explode_now deduped with an O(N^2) scan over a fixed
# broken[512] and silently discarded every cell past the cap; the fix is an
# O(1) coordinate hash set + blast-sized transient staging. Full server_local
# env fixture (dirt monolith + power-16 blast + before/after scan; power 10
# only yields ~420 unique cells here, under the old cap); the >512 assertion
# fails on the pre-fix code by construction.
add_executable(test_explosion_large_tnt tests/test_explosion_large_tnt.c)
target_link_libraries(test_explosion_large_tnt cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_explosion_large_tnt PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_explosion_large_tnt PROPERTIES C_STANDARD 99)
target_link_libraries(test_explosion_large_tnt ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_explosion_large_tnt COMMAND test_explosion_large_tnt)

add_executable(test_explosion_blast_abort tests/test_explosion_blast_abort.c)
target_link_libraries(test_explosion_blast_abort cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_explosion_blast_abort PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_explosion_blast_abort PROPERTIES C_STANDARD 99)
target_link_libraries(test_explosion_blast_abort ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_explosion_blast_abort COMMAND test_explosion_blast_abort)

# next-gaps-0924/G2: Beta Explosion.doExplosionA grid rays + resistance, the
# all-entity pass with World.func_675_a exposure (mob/item damage + impulse).
add_executable(test_explosion_beta_golden tests/test_explosion_beta_golden.c)
target_link_libraries(test_explosion_beta_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_explosion_beta_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_explosion_beta_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_explosion_beta_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_explosion_beta_golden COMMAND test_explosion_beta_golden)

# V6 F1 round 4: main's actual owner dispatch and real physics/credit lifetimes.
add_executable(test_f1_r4 tests/test_f1_r4.c
	source/network/mp_codec.c source/network/mp_session.c
	source/network/server_remote.c source/network/server_remote_transport.c
	source/network/server_remote_protocol.c source/network/server_remote_entities.c
	source/network/server_remote_chunks.c source/platform/net_socket.c)
target_link_libraries(test_f1_r4 cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_f1_r4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_f1_r4 PROPERTIES C_STANDARD 99)
if(WIN32)
	target_link_libraries(test_f1_r4 ws2_32)
endif()
foreach(_cw_f1case replay reset leave dispatch transfer remote frame sleeping terrain fatal)
	add_test(NAME test_f1_r4_${_cw_f1case} COMMAND test_f1_r4 ${_cw_f1case})
	set_tests_properties(test_f1_r4_${_cw_f1case} PROPERTIES TIMEOUT 60)
endforeach()
foreach(_cw_f1count 32 33 34 40 101 250)
	add_test(NAME test_f1_r4_pressure_${_cw_f1count} COMMAND test_f1_r4 pressure ${_cw_f1count})
	set_tests_properties(test_f1_r4_pressure_${_cw_f1count} PROPERTIES TIMEOUT 60)
endforeach()
if(Python3_FOUND)
	add_test(NAME test_f1_memory_inventory COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_f1_memory_inventory.py)
	add_test(NAME test_f1_r5_stale_tag COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_f1_stale_tag.py $<TARGET_FILE:test_f1_r4>)
	set_tests_properties(test_f1_r5_stale_tag PROPERTIES TIMEOUT 60)
endif()

# lane C 2026-09-27: explosion vs knockBack vertical impulse (Explosion.
# doExplosionA :100-103 after EntityLiving.knockBack :392-405).
add_executable(test_explosion_knockback_vertical tests/test_explosion_knockback_vertical.c)
target_link_libraries(test_explosion_knockback_vertical cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_explosion_knockback_vertical PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_explosion_knockback_vertical PROPERTIES C_STANDARD 99)
target_link_libraries(test_explosion_knockback_vertical ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
foreach(_cw_ekv up down tnt)
	add_test(NAME test_explosion_knockback_vertical_${_cw_ekv}
		COMMAND test_explosion_knockback_vertical ${_cw_ekv})
endforeach()

# next-gaps-0924/W1C: Beta EntityItem ground friction (ice), lava bounce +
# lava/fire burn to death, landing (no rebound) -- EntityItem.java:45-95.
add_executable(test_item_env_parity_golden tests/test_item_env_parity_golden.c)
target_link_libraries(test_item_env_parity_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_item_env_parity_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_item_env_parity_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_item_env_parity_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_item_env_parity_golden COMMAND test_item_env_parity_golden)

add_executable(test_item_entity_parity tests/test_item_entity_parity.c)
target_link_libraries(test_item_entity_parity cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_item_entity_parity PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_item_entity_parity PROPERTIES C_STANDARD 99)
target_link_libraries(test_item_entity_parity ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_item_entity_parity COMMAND test_item_entity_parity)

# lane C #14 (2026-09-26): stuck-arrow pickup for either same-world actor --
# EntityPlayer.onLivingUpdate:196-205 expand(1,0,1) scan (health > 0, own world)
# + EntityArrow.onCollideWithPlayer:257-266 (inGround, player-fired, shake 0,
# inventory accepts); host first, cross-world arrows never visited.
add_executable(test_arrow_pickup_actors tests/test_arrow_pickup_actors.c)
target_link_libraries(test_arrow_pickup_actors cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_arrow_pickup_actors PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_arrow_pickup_actors PROPERTIES C_STANDARD 99)
target_link_libraries(test_arrow_pickup_actors ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_arrow_pickup_actors COMMAND test_arrow_pickup_actors)

# lane C #18 (2026-09-26): player-body -> minecart shove for either same-world
# body -- EntityLiving.onLivingUpdate:643-651 sweep + EntityMinecart.
# applyEntityCollision:646-714 (rider skipped, seated body no counter-shove).
add_executable(test_minecart_body_collision tests/test_minecart_body_collision.c)
target_link_libraries(test_minecart_body_collision cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_body_collision PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_body_collision PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_body_collision ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_body_collision COMMAND test_minecart_body_collision)

# next-gaps-0924/W2 MOVE P3: Beta water-flow push -- World.handleMaterialAcceleration:
# 1418-1450 (+ checkChunksExist:327-348) and BlockFluid.getFlowVector:86-166 on the
# player walk (host / split guest / MP client), the mob tick and the item tick (server
# + client prediction); straight, diagonal, still, falling and chunk-seam goldens.
add_executable(test_water_flow_push_golden tests/test_water_flow_push_golden.c)
target_link_libraries(test_water_flow_push_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_water_flow_push_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_water_flow_push_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_water_flow_push_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_water_flow_push_golden COMMAND test_water_flow_push_golden)
set_tests_properties(test_water_flow_push_golden PROPERTIES TIMEOUT 300)

# next-gaps-0924/W2 MOVE P4: Beta entity-entity push -- Entity.applyEntityCollision:668-691
# from EntityLiving.onLivingUpdate:641-651 over the Beta-ordered entity query
# (World.java:2007-2023, Chunk.java:484-506): impulse values bit-exact, two/three-mob
# repeated-call scenes, the strict 0.8 gate, item filter, server + client player loops.
add_executable(test_entity_push_golden tests/test_entity_push_golden.c)
target_link_libraries(test_entity_push_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_entity_push_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_entity_push_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_entity_push_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_entity_push_golden COMMAND test_entity_push_golden)
set_tests_properties(test_entity_push_golden PROPERTIES TIMEOUT 300)

# next-gaps-0924/GOLDEN-0925: focused first-divergence assertions for the five
# whole-sim goldens moved by the W2-MOVE parity commits (Astra ruling 8).
add_executable(test_golden_move_divergence tests/test_golden_move_divergence.c)
target_link_libraries(test_golden_move_divergence cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_golden_move_divergence PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_golden_move_divergence PROPERTIES C_STANDARD 99)
target_link_libraries(test_golden_move_divergence ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_golden_move_divergence COMMAND test_golden_move_divergence)
set_tests_properties(test_golden_move_divergence PROPERTIES TIMEOUT 300)

# next-gaps-0924/W2 MOVE P12: Beta cobweb slow -- BlockWeb.onEntityCollidedWithBlock:10-12 via
# the post-move cell sweep, consumed by Entity.moveEntity:262-268 (0.25 / 0.05F, motion zeroed)
# on the player walk, the mob tick and the item tick (server + client prediction).
add_executable(test_cobweb_golden tests/test_cobweb_golden.c)
target_link_libraries(test_cobweb_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_cobweb_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_cobweb_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_cobweb_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_cobweb_golden COMMAND test_cobweb_golden)
set_tests_properties(test_cobweb_golden PROPERTIES TIMEOUT 300)

# next-gaps-0924/W2 MOVE P4b (Astra item 13): the LOCAL Beta entity query (per-section
# id lists, Chunk.entities[] order) is sequence-identical to the exhaustive ordered
# snapshot across scrambles/erasures, with the host player; prints the per-query cost.
add_executable(test_entity_query_local_equiv tests/test_entity_query_local_equiv.c)
target_link_libraries(test_entity_query_local_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_entity_query_local_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_entity_query_local_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_entity_query_local_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_entity_query_local_equiv COMMAND test_entity_query_local_equiv)
set_tests_properties(test_entity_query_local_equiv PROPERTIES TIMEOUT 300)

add_executable(test_scheduled_tick_budget
	source/network/server_world_tick_queue.c
	tests/test_scheduled_tick_budget.c
)
target_link_libraries(test_scheduled_tick_budget m)
target_include_directories(test_scheduled_tick_budget PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_scheduled_tick_budget PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED
)
add_test(NAME test_scheduled_tick_budget COMMAND test_scheduled_tick_budget)

add_executable(test_item_pickup
	source/network/inventory_logic.c
	tests/test_item_pickup.c
)
target_include_directories(test_item_pickup PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_item_pickup PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED
)
target_link_libraries(test_item_pickup m)
add_test(NAME test_item_pickup COMMAND test_item_pickup)

add_executable(test_svin_reliable
	source/network/server_interface.c
	source/platform/thread.c
	source/platform/time.c
	tests/test_svin_reliable.c
)
target_link_libraries(test_svin_reliable m)
target_include_directories(test_svin_reliable PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_svin_reliable PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED
)
target_link_libraries(test_svin_reliable pthread)
add_test(NAME test_svin_reliable COMMAND test_svin_reliable)

add_executable(test_scheduled_tick_parity
	source/network/server_world_block_filter.c
	tests/test_scheduled_tick_parity.c
)
target_link_libraries(test_scheduled_tick_parity m)
target_include_directories(test_scheduled_tick_parity PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_scheduled_tick_parity PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED
)
add_test(NAME test_scheduled_tick_parity COMMAND test_scheduled_tick_parity)

add_executable(test_tnt_fuse
	source/worldgen/beta_math.c
	tests/test_tnt_fuse.c
)
target_include_directories(test_tnt_fuse PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(test_tnt_fuse m)
add_test(NAME test_tnt_fuse COMMAND test_tnt_fuse)

# Beta primed-TNT lifecycle golden: ignition removes the block and spawns an
# entity whose 80->0 post-decrement fuse is independent of a completely full
# scheduled-fluid ring; update 81 detonates. Reproduces the real-Wii permanent
# snow-TNT regression through the production block/world/entity paths.
add_executable(test_tnt_primed_env tests/test_tnt_primed_env.c)
target_link_libraries(test_tnt_primed_env cavex_test_core cavex_test_world
	cavex_test_tickq_tnt cavex_test_input)
# The test TU asserts against the SAME cap the tickq_tnt flavor compiles the
# engine with; both say 2 on purpose.
target_compile_definitions(test_tnt_primed_env PRIVATE
	FCAVEX_MAX_EXPLOSIONS_PER_TICK=2)
target_include_directories(test_tnt_primed_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_tnt_primed_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_tnt_primed_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_tnt_primed_env COMMAND test_tnt_primed_env)
add_executable(test_tnt_evidence_lifecycle tests/test_tnt_evidence_lifecycle.c)
target_link_libraries(test_tnt_evidence_lifecycle cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_tnt_evidence_lifecycle PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_tnt_evidence_lifecycle PROPERTIES C_STANDARD 99)
target_link_libraries(test_tnt_evidence_lifecycle ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_tnt_evidence_lifecycle COMMAND test_tnt_evidence_lifecycle)

# motion-proof-0927 (lane C, mob motion units S2 proof): the mob_motion_2m
# fixture. test_mob_motion_probe = the pure checkers on good AND bad traces;
# test_mob_motion_rig_env = the production rig through real server updates to
# a terminal PASS; test_mob_motion_evidence = the collector gate + cost summary.
add_executable(test_mob_motion_probe tests/test_mob_motion_probe.c
	source/entity/mob_motion_probe.c)
target_include_directories(test_mob_motion_probe PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_mob_motion_probe PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_motion_probe m)
add_test(NAME test_mob_motion_probe COMMAND test_mob_motion_probe)
add_executable(test_mob_motion_rig_env tests/test_mob_motion_rig_env.c)
target_link_libraries(test_mob_motion_rig_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mob_motion_rig_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_mob_motion_rig_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_motion_rig_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mob_motion_rig_env COMMAND test_mob_motion_rig_env)
set_tests_properties(test_mob_motion_rig_env PROPERTIES TIMEOUT 400)
add_test(NAME test_mob_motion_evidence COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mob_motion_evidence.py)

# M4 animals env test: SRPC_ENTITY_USE validation + cow milking + milk-bucket
# empty, sheep shear/dye (drops, durability, sheared render flag, archive round
# trip, death-drop gate), chicken egg-lay timing, the cake recipe's in-grid
# container-item return, and the thrown egg (0-damage hit, pinned chick rolls,
# block-hit despawn) -- all through the production server dispatch/tick paths.
add_executable(test_animal_interact_env tests/test_animal_interact_env.c)
target_link_libraries(test_animal_interact_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_animal_interact_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_animal_interact_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_animal_interact_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_animal_interact_env COMMAND test_animal_interact_env)

# DROPS-WORLD: each real mob drop site in both storage slots, plain passes.
add_executable(test_drops_world_routing tests/test_drops_world_routing.c)
target_link_libraries(test_drops_world_routing cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_drops_world_routing PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_drops_world_routing PROPERTIES C_STANDARD 99)
foreach(_cw_drop cow chicken pig_zombie ghast spider sheep skeleton creeper egg shear pig pig_burning squid zombie sand gravel sand_timeout leaves unloaded stale)
	foreach(_cw_drop_slot 0 1)
		add_test(NAME test_drops_world_routing__${_cw_drop}_${_cw_drop_slot}
			COMMAND test_drops_world_routing ${_cw_drop} ${_cw_drop_slot})
		set_tests_properties(test_drops_world_routing__${_cw_drop}_${_cw_drop_slot}
			PROPERTIES TIMEOUT 60)
	endforeach()
endforeach()

# Saddle-slice env test (SADDLE_DISPATCH.md): ItemSaddle.hitEntity (hit-to-
# saddle, consume exactly one, double-saddle guard, non-saddle no-op), the
# saddled flag riding CRPC_ENTITY_MOVE, EntityPig.interact + Entity.mountEntity
# (refuse-unsaddled, mount, toggle-dismount-on-self, refuse-while-occupied),
# the archive round trip (saddled survives, rider does not), and the negative
# control proving a ridden pig receives no steering.
add_executable(test_pig_saddle_env tests/test_pig_saddle_env.c)
target_link_libraries(test_pig_saddle_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_pig_saddle_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_pig_saddle_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_pig_saddle_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_pig_saddle_env COMMAND test_pig_saddle_env)

add_executable(test_spider_jockey_spawn tests/test_spider_jockey_spawn.c)
target_link_libraries(test_spider_jockey_spawn cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_spider_jockey_spawn PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_spider_jockey_spawn PROPERTIES C_STANDARD 99)
target_link_libraries(test_spider_jockey_spawn ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_spider_jockey_spawn COMMAND test_spider_jockey_spawn)

# T1 equivalence proof for server_tick_monster's trailing CRPC_ENTITY_MOVE
# change-gate (entity_monster.c) -- see the test file header for the full
# citation/coverage.
add_executable(test_entity_move_change_gate_equiv tests/test_entity_move_change_gate_equiv.c)
target_link_libraries(test_entity_move_change_gate_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_entity_move_change_gate_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_entity_move_change_gate_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_entity_move_change_gate_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_entity_move_change_gate_equiv COMMAND test_entity_move_change_gate_equiv)

# W01 opt-in, authoritative melee wind-up presentation/lifecycle.
add_executable(test_mob_windup_cue tests/test_mob_windup_cue.c)
target_link_libraries(test_mob_windup_cue cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mob_windup_cue PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_mob_windup_cue PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_windup_cue ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mob_windup_cue COMMAND test_mob_windup_cue)

# entities_client_tick erase-safety stress test (entity.c) -- see the test
# file header for the full m-dict.h citation/coverage.
add_executable(test_entities_client_tick_erase_safe tests/test_entities_client_tick_erase_safe.c)
target_link_libraries(test_entities_client_tick_erase_safe cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_entities_client_tick_erase_safe PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_entities_client_tick_erase_safe PROPERTIES C_STANDARD 99)
target_link_libraries(test_entities_client_tick_erase_safe ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_entities_client_tick_erase_safe COMMAND test_entities_client_tick_erase_safe)

# Beta 1.7.3 P3(a): the world list used to be enumerated EAGERLY inside screen_set(), which
# screen.c runs synchronously -- ~118 level.dat opens + gzip inflates on the main thread at
# the exact moment save-and-quit had the server fsync-ing the same card, producing a >3s
# no-progress stall and a HANG report reading main=SCRN_UPD. Now K rows resolve per frame.
# Asserts on WORK PER CALL (archive-open counts), never elapsed time: a PC SSD hides the SD
# cost this exists to bound. WORKING_DIRECTORY = source root for the scratch saves fixture.
add_executable(test_world_list_incremental tests/test_world_list_incremental.c)
target_link_libraries(test_world_list_incremental cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_world_list_incremental PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_world_list_incremental PROPERTIES C_STANDARD 99)
target_link_libraries(test_world_list_incremental ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_world_list_incremental COMMAND test_world_list_incremental
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# OSK widget + GuiCreateWorld pure logic: osk_feed_char edit semantics
# [740c583 GuiTextField.textboxKeyTyped], folder derivation
# [GuiCreateWorld.calcSaveDirName], seed resolution (Java 6 Long.parseLong,
# NO '+'; String.hashCode int32-wrap goldens hand-computed)
# [GuiCreateWorld.actionPerformed], the "-" mkdir-reservation dedupe
# [generateUnusedFolderName], and the LevelName TAG_STRING round-trip the
# dialog introduced. Scratch dirs are made in the build cwd and removed.
# 2026-09-11 chat composer lane: the pure model (Beta Enter/trim, the D-pad
# Up-alone opener gate, the byte-exact addChatMessage wrap) and the 50-line
# history ring (docs/mp_chat_auth_research_2026-09-11.md). Focused test: no pool.
add_executable(test_chat_composer
	tests/test_chat_composer.c
	source/game/gui/chat_composer_model.c)
target_include_directories(test_chat_composer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_chat_composer PROPERTIES C_STANDARD 99)
add_test(NAME test_chat_composer COMMAND test_chat_composer)

# b1-chat-opener-0919: the chat OPENER gesture as a pure state machine
# (source/platform/input_tap_gesture.h) -- the Wii Remote 2 TAP window, the
# Classic Plus release-to-open with its Minus cancel, and the poll-gap /
# owner-change / disconnect clearing. Header-only, so no TU beyond the test.
add_executable(test_chat_gesture tests/test_chat_gesture.c)
target_include_directories(test_chat_gesture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_chat_gesture PROPERTIES C_STANDARD 99)
add_test(NAME test_chat_gesture COMMAND test_chat_gesture)

# b1-chat-opener-0919/07 B: the Wii Remote SNEAK TOGGLE as a pure state machine
# (source/platform/input_sneak_toggle.h) -- one toggle per fresh press, the
# death/respawn and vehicle/bed-entry clears, and the "a screen preserves the
# state but suppresses its input, and the press that closed the menu cannot
# toggle" rule. Header-only, so no TU beyond the test.
add_executable(test_sneak_toggle tests/test_sneak_toggle.c)
target_include_directories(test_sneak_toggle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_sneak_toggle PROPERTIES C_STANDARD 99)
add_test(NAME test_sneak_toggle COMMAND test_sneak_toggle)

# b1-chat-opener-0919/15: the Beta sneak CAMERA DIP (Entity.ySize).
# Goldens for the pure header (source/entity/player_ysize.h) plus the real
# camera_attach_owned: zero-state byte-identity with the old lerp, the eye-only
# drop, the wire fold-out proving SRPC_PLAYER_POS/feet/collision are bit-
# identical standing vs sneaking for both local owners, and the block-pick ray
# carrying the dip. Links the camera TU, so it needs core/world like
# test_bed_camera.
add_executable(test_player_ysize tests/test_player_ysize.c)
target_link_libraries(test_player_ysize cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input)
target_include_directories(test_player_ysize PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_player_ysize PROPERTIES C_STANDARD 99)
target_link_libraries(test_player_ysize ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB
	glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_player_ysize COMMAND test_player_ysize)

# next-gaps-0924/G7: Beta step-up ySize goldens and rendered eye at partial ticks.
add_executable(test_player_step_ysize tests/test_player_step_ysize.c)
target_link_libraries(test_player_step_ysize cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input)
target_include_directories(test_player_step_ysize PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_player_step_ysize PROPERTIES C_STANDARD 99)
target_link_libraries(test_player_step_ysize ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB
	glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_player_step_ysize COMMAND test_player_step_ysize)

# next-gaps-0924/STEPCAM: LOCAL player step-up ySize through the real walk onto a
# real slab, incl. the binade-split heights (feet 63.5 / eye 65.12), rendered eye
# vs Beta orientCamera, collision placement pinned unchanged.
add_executable(test_local_step_camera_golden tests/test_local_step_camera_golden.c)
target_link_libraries(test_local_step_camera_golden cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input)
target_include_directories(test_local_step_camera_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_local_step_camera_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_local_step_camera_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB
	glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_local_step_camera_golden COMMAND test_local_step_camera_golden)

# next-gaps-0924/STEPCAM follow-up: the local step route's Beta gate
# (Entity.moveEntity:341 var36 && (var18 || ySize < 0.05F) && horizontal clip) and
# horizontal-only acceptance (:398), through the real walk.
add_executable(test_local_step_gate_golden tests/test_local_step_gate_golden.c)
target_link_libraries(test_local_step_gate_golden cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input)
target_include_directories(test_local_step_gate_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_local_step_gate_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_local_step_gate_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB
	glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_local_step_gate_golden COMMAND test_local_step_gate_golden)

# next-gaps-0924/STEPCAM: the local step height is Beta's EntityLiving.stepHeight
# 0.5F (slab steps, bed 9/16 stops), reached exactly in the float eye model.
add_executable(test_local_step_height_golden tests/test_local_step_height_golden.c)
target_link_libraries(test_local_step_height_golden cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input)
target_include_directories(test_local_step_height_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_local_step_height_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_local_step_height_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB
	glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_local_step_height_golden COMMAND test_local_step_height_golden)

# B1a chat keyboard geometry/navigation against docs/ui_design/chat-keyboard/SPEC.md.
add_executable(test_chat_keyboard
	tests/test_chat_keyboard.c
	source/game/gui/chat_composer_model.c
	source/game/gui/widget_osk.c)
target_compile_definitions(test_chat_keyboard PRIVATE
	PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_chat_keyboard PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_chat_keyboard m)
add_test(NAME test_chat_keyboard COMMAND test_chat_keyboard)

add_executable(test_osk_env tests/test_osk_env.c)
target_link_libraries(test_osk_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_osk_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_osk_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_osk_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_osk_env COMMAND test_osk_env)

# Create-world CONSOLE form state machine
# (docs/ui_design/create_world_console_2026-09-01.md sections 3-4): focus-ring order
# and wrap, disabled-Create skipping, the net-new Game Mode selector, the
# modal OSK's open/commit/revert, the numeric-first seed page's alphabet, and
# seed vectors for a seed typed on the OSK's letter pages (the hashCode
# branch). Drives cworld_form_model.c directly -- no gfx, no input, no
# filesystem; the same pure-model pattern every other screen test uses.
add_executable(test_cworld_form tests/test_cworld_form.c)
target_link_libraries(test_cworld_form cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_cworld_form PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_cworld_form PROPERTIES C_STANDARD 99)
target_link_libraries(test_cworld_form ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_cworld_form COMMAND test_cworld_form)

# Shared Beta GuiSlot logic and GuiButton AABB: hand-computed scroll clamps,
# row mapping/viewport edges, <250ms double-click activation, empty/one-row
# centering [740c583 GuiSlot], and synthetic pointer bounds [GuiButton].
add_executable(test_slotlist_env tests/test_slotlist_env.c)
target_link_libraries(test_slotlist_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_slotlist_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_slotlist_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_slotlist_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_slotlist_env COMMAND test_slotlist_env)

# Beta GuiOptions / GuiIngameMenu parity: hand-computed relevantOptions
# order/ids, GameSettings.getKeyBinding labels, GuiSlider pointer/thumb math,
# audio.volume persistence, and the existing simultaneous pause-edge seam.
add_executable(test_options_env tests/test_options_env.c)
target_link_libraries(test_options_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_options_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_options_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_options_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_options_env COMMAND test_options_env)

# Native controller container navigation and action separation.
add_executable(test_beta_window_click_golden tests/test_beta_window_click_golden.c ${CAVEX_MP_SOURCES})
target_link_libraries(test_beta_window_click_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
set_target_properties(test_beta_window_click_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_beta_window_click_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_beta_window_click_golden ws2_32)
endif()
target_include_directories(test_beta_window_click_golden PRIVATE source third_party)
add_test(NAME test_beta_window_click_golden COMMAND test_beta_window_click_golden)

add_executable(test_sort_session tests/test_sort_session.c ${CAVEX_MP_SOURCES})
target_link_libraries(test_sort_session cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_sort_session PRIVATE source third_party)
set_target_properties(test_sort_session PROPERTIES C_STANDARD 99)
target_link_libraries(test_sort_session ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_sort_session ws2_32)
endif()
add_test(NAME test_sort_session COMMAND test_sort_session)

# next-gaps-0924/m1: the multiplayer arm swing (docs/intent/m1-mp-arm-swing.md).
# Pure cadence/place-predicate goldens (game/mp_arm_swing.h) plus the REAL
# screen_ingame.c action path driven through the agent bridge (as
# test_split_actions_client) into the REAL svin mailbox and remote provider,
# reading the socket-bound frames in order.
add_executable(test_mp_arm_swing tests/test_mp_arm_swing.c
	source/game/agent_input.c ${CAVEX_MP_SOURCES})
target_link_libraries(test_mp_arm_swing cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input_agent
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_mp_arm_swing ws2_32)
endif()
target_compile_definitions(test_mp_arm_swing PRIVATE CAVEWII_AGENT_BRIDGE)
target_include_directories(test_mp_arm_swing PRIVATE source third_party)
set_target_properties(test_mp_arm_swing PROPERTIES C_STANDARD 99)
add_test(NAME test_mp_arm_swing COMMAND test_mp_arm_swing)
set_tests_properties(test_mp_arm_swing PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 120)

# next-gaps-0924/m2: another angler's fishing hook is visible online
# (docs/intent/m2-mp-remote-bobber.md). Packet23 type 90 bytes through the
# real decoder + process_event into the real client entity store.
add_executable(test_mp_remote_bobber tests/test_mp_remote_bobber.c ${CAVEX_MP_SOURCES})
target_link_libraries(test_mp_remote_bobber cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mp_remote_bobber PRIVATE source third_party)
set_target_properties(test_mp_remote_bobber PROPERTIES C_STANDARD 99)
target_link_libraries(test_mp_remote_bobber ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_mp_remote_bobber ws2_32)
endif()
add_test(NAME test_mp_remote_bobber COMMAND test_mp_remote_bobber)

# next-gaps-0924/m3: falling sand/gravel visible online
# (docs/intent/m3-mp-remote-falling-sand.md). Packet23 types 70/71 through the
# real decoder + process_event into the real client entity store.
add_executable(test_mp_remote_falling_sand tests/test_mp_remote_falling_sand.c ${CAVEX_MP_SOURCES})
target_link_libraries(test_mp_remote_falling_sand cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mp_remote_falling_sand PRIVATE source third_party)
set_target_properties(test_mp_remote_falling_sand PROPERTIES C_STANDARD 99)
target_link_libraries(test_mp_remote_falling_sand ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_mp_remote_falling_sand ws2_32)
endif()
add_test(NAME test_mp_remote_falling_sand COMMAND test_mp_remote_falling_sand)

# lane/craft-rpc: the server-authoritative one-batch selected-recipe
# transaction. Same shape as test_sort_session above (pooled engine objects +
# ${CAVEX_MP_SOURCES}); tests/ is on the include path so the "support/..."
# fixture helpers resolve, matching test_map_item's own include list.
foreach(_craft_test
		test_recipe_craft_transaction
		test_recipe_craft_capacity
		test_recipe_craft_ticket
		test_recipe_craft_pick_order
		test_recipe_craft_map_server
		test_recipe_craft_one_batch)
	add_executable(${_craft_test} tests/${_craft_test}.c ${CAVEX_MP_SOURCES})
	target_link_libraries(${_craft_test} cavex_test_core cavex_test_world
		cavex_test_tickq cavex_test_input)
	target_include_directories(${_craft_test} PRIVATE source third_party tests)
	set_target_properties(${_craft_test} PROPERTIES C_STANDARD 99)
	target_link_libraries(${_craft_test} ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB
		glfw GLEW::GLEW OpenGL::GL m)
	if(WIN32)
		target_link_libraries(${_craft_test} ws2_32)
	endif()
	add_test(NAME ${_craft_test} COMMAND ${_craft_test})
endforeach()

add_executable(test_sort_oracle_equiv tests/oracle/sort/test_sort_oracle_equiv.c ${CAVEX_MP_SOURCES})
target_link_libraries(test_sort_oracle_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_sort_oracle_equiv PRIVATE source third_party)
set_target_properties(test_sort_oracle_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_sort_oracle_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_sort_oracle_equiv ws2_32)
endif()
add_test(NAME test_sort_oracle_equiv COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/sort/run.py
	--bin $<TARGET_FILE:test_sort_oracle_equiv> --mp $<TARGET_FILE:test_mp_s0_env>)

add_executable(test_inventory_sort_golden tests/test_inventory_sort_golden.c)
target_link_libraries(test_inventory_sort_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_inventory_sort_golden PRIVATE source third_party)
set_target_properties(test_inventory_sort_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_inventory_sort_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_inventory_sort_golden COMMAND test_inventory_sort_golden)
set_tests_properties(test_inventory_sort_golden PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_menu_sound_events tests/test_menu_sound_events.c source/platform/time.c
	tools/wii_stage_server/source/sha256.c)
target_include_directories(test_menu_sound_events PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source)
target_compile_definitions(test_menu_sound_events PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_menu_sound_events m)
add_test(NAME test_menu_sound_events COMMAND test_menu_sound_events)
set_tests_properties(test_menu_sound_events PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_menu_sound_priority tests/test_menu_sound_priority.c source/platform/time.c)
target_include_directories(test_menu_sound_priority PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(test_menu_sound_priority PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_menu_sound_priority m)
add_test(NAME test_menu_sound_priority COMMAND test_menu_sound_priority)

add_executable(test_sort_presentation tests/test_sort_presentation.c)
target_link_libraries(test_sort_presentation cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_sort_presentation PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_sort_presentation PROPERTIES C_STANDARD 99)
target_link_libraries(test_sort_presentation ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_sort_presentation COMMAND test_sort_presentation)
set_tests_properties(test_sort_presentation PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Approved Wii UI geometry as assertions (Astra ui1/ui2/ui3 tables, 2026-09-11):
# compact inventory Next/crafting positions, split pause composition, footer
# pitch + chord width, gameplay HUD rows pinned at the call site.
add_executable(test_ui_geometry tests/test_ui_geometry.c)
target_link_libraries(test_ui_geometry cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_ui_geometry PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_ui_geometry PROPERTIES C_STANDARD 99)
target_link_libraries(test_ui_geometry ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_ui_geometry COMMAND test_ui_geometry)
set_tests_properties(test_ui_geometry PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(test_ui_geometry PRIVATE "-Wl,--wrap=gfx_bind_texture" "-Wl,--wrap=gfx_texture" "-Wl,--wrap=gfx_draw_quads" "-Wl,--wrap=gutil_draw_item" "-Wl,--wrap=gfx_width" "-Wl,--wrap=gfx_height" "-Wl,--wrap=input_pointer_enable" "-Wl,--wrap=tex_gfx_lookup" "-Wl,--wrap=audio_menu_event" "-Wl,--wrap=input_held" "-Wl,--wrap=input_pressed" "-Wl,--wrap=input_pointer" "-Wl,--wrap=gfx_blending" "-Wl,--wrap=gfx_alpha_test" "-Wl,--wrap=input_prompt_frame" "-Wl,--wrap=gutil_text_rgb")

target_link_options(test_sort_presentation PRIVATE "-Wl,--wrap=gfx_bind_texture" "-Wl,--wrap=gfx_texture" "-Wl,--wrap=gfx_draw_quads" "-Wl,--wrap=gfx_matrix_modelview" "-Wl,--wrap=gfx_write_buffers" "-Wl,--wrap=gfx_lighting" "-Wl,--wrap=gfx_cull_func" "-Wl,--wrap=gutil_draw_item" "-Wl,--wrap=gfx_width" "-Wl,--wrap=gfx_height" "-Wl,--wrap=input_pointer_enable" "-Wl,--wrap=tex_gfx_lookup" "-Wl,--wrap=audio_menu_event" "-Wl,--wrap=input_held" "-Wl,--wrap=input_pressed" "-Wl,--wrap=input_pointer")

add_executable(test_container_menu tests/test_container_menu.c)
target_link_libraries(test_container_menu cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_container_menu PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_container_menu PROPERTIES C_STANDARD 99)
target_link_libraries(test_container_menu ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_container_menu COMMAND test_container_menu)

add_executable(test_inventory_page_hold tests/test_inventory_page_hold.c)
target_link_libraries(test_inventory_page_hold cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_inventory_page_hold PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_inventory_page_hold PROPERTIES C_STANDARD 99)
target_link_options(test_inventory_page_hold PRIVATE "-Wl,--wrap=time_get" "-Wl,--wrap=input_player_frame")
add_test(NAME test_inventory_page_hold COMMAND test_inventory_page_hold)

# Lane 4: the approved crafting/inventory guide (Console Workbench, Pocket
# Ledger, personal 2x2). Same link shape as test_container_menu -- it drives
# the real screens through the real input latch.
add_executable(test_recipe_ui tests/test_recipe_ui.c)
target_link_libraries(test_recipe_ui cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_recipe_ui PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_recipe_ui PROPERTIES C_STANDARD 99)
target_link_libraries(test_recipe_ui ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_recipe_ui COMMAND test_recipe_ui)
target_link_options(test_recipe_ui PRIVATE "-Wl,--wrap=audio_menu_event")

# Chest sound corrections (docs/ui_design/approved/specs/sound-events.md +
# crafting-chests.md). Each wraps ONLY audio_menu_event, so the prediction,
# submit and authoritative-reply paths under test are the shipping ones.
add_executable(test_sort_completion_sound tests/test_sort_completion_sound.c ${CAVEX_MP_SOURCES})
target_link_libraries(test_sort_completion_sound cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_sort_completion_sound PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_sort_completion_sound PROPERTIES C_STANDARD 99)
target_link_libraries(test_sort_completion_sound ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_sort_completion_sound ws2_32)
endif()
target_link_options(test_sort_completion_sound PRIVATE "-Wl,--wrap=audio_menu_event")
add_test(NAME test_sort_completion_sound COMMAND test_sort_completion_sound)

add_executable(test_chest_back_sound tests/test_chest_back_sound.c)
target_link_libraries(test_chest_back_sound cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_chest_back_sound PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_chest_back_sound PROPERTIES C_STANDARD 99)
target_link_libraries(test_chest_back_sound ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_link_options(test_chest_back_sound PRIVATE "-Wl,--wrap=audio_menu_event")
add_test(NAME test_chest_back_sound COMMAND test_chest_back_sound)

add_executable(test_chest_item_sound_commit tests/test_chest_item_sound_commit.c)
target_link_libraries(test_chest_item_sound_commit cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_chest_item_sound_commit PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_chest_item_sound_commit PROPERTIES C_STANDARD 99)
target_link_libraries(test_chest_item_sound_commit ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_link_options(test_chest_item_sound_commit PRIVATE "-Wl,--wrap=audio_menu_event")
add_test(NAME test_chest_item_sound_commit COMMAND test_chest_item_sound_commit)

add_executable(test_split_layout tests/test_split_layout.c)
target_include_directories(test_split_layout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_split_layout m)
add_test(NAME test_split_layout COMMAND test_split_layout)

add_executable(test_local_pause tests/test_local_pause.c)
target_link_libraries(test_local_pause cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_local_pause PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_local_pause PROPERTIES C_STANDARD 99)
target_link_options(test_local_pause PRIVATE "-Wl,--wrap=audio_menu_event")
add_test(NAME test_local_pause COMMAND test_local_pause)
# stale-bugs-verify-0924/V1: verify 09-16 Classic unplug and pending-Leave host quit.
add_executable(test_split_stale_reports tests/test_split_stale_reports.c)
target_link_libraries(test_split_stale_reports cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_stale_reports PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_stale_reports PROPERTIES C_STANDARD 99)
target_link_options(test_split_stale_reports PRIVATE "-Wl,--wrap=audio_menu_event")
add_test(NAME test_split_stale_reports COMMAND test_split_stale_reports)
add_executable(test_local_menu_recovery tests/test_local_menu_recovery.c)
target_link_libraries(test_local_menu_recovery cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_local_menu_recovery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_local_menu_recovery PROPERTIES C_STANDARD 99)
target_link_options(test_local_menu_recovery PRIVATE "-Wl,--wrap=audio_menu_event")
add_test(NAME test_local_menu_recovery COMMAND test_local_menu_recovery)
target_link_options(test_local_menu_recovery PRIVATE "-Wl,--wrap=input_player_device" "-Wl,--wrap=input_devices")

add_executable(test_local_guest tests/test_local_guest.c)
target_link_libraries(test_local_guest cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_local_guest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_local_guest PROPERTIES C_STANDARD 99)
add_test(NAME test_local_guest COMMAND test_local_guest)

# G8-VEHICLE-SEAMS-01 step 1: vehicle owner/client/lifecycle seam suites.
# Every mode is its own ctest (one process per case, own working directory
# and TMP so fixture directories never collide under ctest -j).
add_executable(test_vehicle_world_routing tests/test_vehicle_world_routing.c)
target_link_libraries(test_vehicle_world_routing cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_vehicle_world_routing PRIVATE source third_party)
set_target_properties(test_vehicle_world_routing PROPERTIES C_STANDARD 99)
target_link_libraries(test_vehicle_world_routing ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
foreach(_kind RANGE 0 1)
	foreach(_actor RANGE 0 1)
		foreach(_fail RANGE 0 1)
			set(_name "test_vehicle_world_routing__${_kind}_${_actor}_${_fail}")
			file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_worlds/${_name}")
			add_test(NAME ${_name} COMMAND test_vehicle_world_routing ${_kind} 0 ${_actor} ${_fail}
				WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_worlds/${_name}")
		endforeach()
	endforeach()
endforeach()
foreach(_type RANGE 1 2)
	set(_name "test_vehicle_world_routing__cart_${_type}")
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_worlds/${_name}")
	add_test(NAME ${_name} COMMAND test_vehicle_world_routing 0 ${_type} 0 0
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_worlds/${_name}")
endforeach()
foreach(_kind RANGE 0 1)
	foreach(_actor RANGE 0 1)
		set(_name "test_vehicle_world_routing__primary_${_kind}_${_actor}")
		file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_worlds/${_name}")
		add_test(NAME ${_name} COMMAND test_vehicle_world_routing ${_kind} 0 ${_actor} 0 0
			WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_worlds/${_name}")
	endforeach()
endforeach()
add_executable(test_vehicle_owner_seams tests/test_vehicle_owner_seams.c)
target_link_libraries(test_vehicle_owner_seams cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_vehicle_owner_seams PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_vehicle_owner_seams PROPERTIES C_STANDARD 99)
target_link_libraries(test_vehicle_owner_seams ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_executable(test_vehicle_client_seams tests/test_vehicle_client_seams.c)
target_link_libraries(test_vehicle_client_seams cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_vehicle_client_seams PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_vehicle_client_seams PROPERTIES C_STANDARD 99)
target_link_libraries(test_vehicle_client_seams ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
# integ-1011/1e: real offered-body piston driver; one fresh fixture per case.
add_executable(test_split_offer_piston_script tests/test_split_offer_piston_script.c)
target_link_libraries(test_split_offer_piston_script cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_split_offer_piston_script PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_offer_piston_script PROPERTIES C_STANDARD 99)
target_link_libraries(test_split_offer_piston_script ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
foreach(_sop_case IN ITEMS profile run retry fifo dormant)
    set(_sop_name test_split_offer_piston_script__${_sop_case})
    set(_sop_dir "${CMAKE_CURRENT_BINARY_DIR}/split_offer_piston/${_sop_name}")
    file(MAKE_DIRECTORY "${_sop_dir}")
    add_test(NAME ${_sop_name} COMMAND test_split_offer_piston_script ${_sop_case} WORKING_DIRECTORY "${_sop_dir}")
    set_tests_properties(${_sop_name} PROPERTIES ENVIRONMENT "TMP=${_sop_dir};TEMP=${_sop_dir}")
endforeach()
if(Python3_Interpreter_FOUND)
    add_test(NAME test_split_offer_piston_collector COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_split_offer_piston_collector.py)
endif()

# SPEC R12 save_reload_3m: the phase driver's decision core (marker order,
# one-frame presses, two loads, every wait's named failure) and the collector
# contract (selftest + PROFILES/marker wiring).
add_executable(test_save_reload_script tests/test_save_reload_script.c)
target_link_libraries(test_save_reload_script cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_save_reload_script PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_save_reload_script PROPERTIES C_STANDARD 99)
target_link_libraries(test_save_reload_script ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_save_reload_script COMMAND test_save_reload_script)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_save_reload_collector COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_save_reload_collector.py)
endif()

# vehicle-smoke-1004 (CONTRACT-v1): host_vehicle_smoke_8m producer/driver.
# Real-handler cases on the owner-swap harness; one case per process.
add_executable(test_vehicle_smoke_script tests/test_vehicle_smoke_script.c)
target_link_libraries(test_vehicle_smoke_script cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_vehicle_smoke_script PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_vehicle_smoke_script PROPERTIES C_STANDARD 99)
target_link_libraries(test_vehicle_smoke_script ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__serialize")
add_test(NAME test_vehicle_smoke_script__serialize COMMAND test_vehicle_smoke_script serialize WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__serialize")
set_tests_properties(test_vehicle_smoke_script__serialize PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__serialize;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__serialize")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__fifo")
add_test(NAME test_vehicle_smoke_script__fifo COMMAND test_vehicle_smoke_script fifo WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__fifo")
set_tests_properties(test_vehicle_smoke_script__fifo PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__fifo;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__fifo")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__fixture")
add_test(NAME test_vehicle_smoke_script__fixture COMMAND test_vehicle_smoke_script fixture WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__fixture")
set_tests_properties(test_vehicle_smoke_script__fixture PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__fixture;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__fixture")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_0")
add_test(NAME test_vehicle_smoke_script__ride_0 COMMAND test_vehicle_smoke_script ride 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_0")
set_tests_properties(test_vehicle_smoke_script__ride_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_1")
add_test(NAME test_vehicle_smoke_script__ride_1 COMMAND test_vehicle_smoke_script ride 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_1")
set_tests_properties(test_vehicle_smoke_script__ride_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_2")
add_test(NAME test_vehicle_smoke_script__ride_2 COMMAND test_vehicle_smoke_script ride 2 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_2")
set_tests_properties(test_vehicle_smoke_script__ride_2 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_2;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__ride_2")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_0")
add_test(NAME test_vehicle_smoke_script__switch_0 COMMAND test_vehicle_smoke_script switch 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_0")
set_tests_properties(test_vehicle_smoke_script__switch_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_1")
add_test(NAME test_vehicle_smoke_script__switch_1 COMMAND test_vehicle_smoke_script switch 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_1")
set_tests_properties(test_vehicle_smoke_script__switch_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_2")
add_test(NAME test_vehicle_smoke_script__switch_2 COMMAND test_vehicle_smoke_script switch 2 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_2")
set_tests_properties(test_vehicle_smoke_script__switch_2 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_2;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__switch_2")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__guest_probes")
add_test(NAME test_vehicle_smoke_script__guest_probes COMMAND test_vehicle_smoke_script guest-probes WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__guest_probes")
set_tests_properties(test_vehicle_smoke_script__guest_probes PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__guest_probes;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__guest_probes")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__misroute")
add_test(NAME test_vehicle_smoke_script__misroute COMMAND test_vehicle_smoke_script misroute WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__misroute")
set_tests_properties(test_vehicle_smoke_script__misroute PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__misroute;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__misroute")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__death")
add_test(NAME test_vehicle_smoke_script__death COMMAND test_vehicle_smoke_script death WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__death")
set_tests_properties(test_vehicle_smoke_script__death PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__death;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__death")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__pending_transfer")
add_test(NAME test_vehicle_smoke_script__pending_transfer COMMAND test_vehicle_smoke_script pending-transfer WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__pending_transfer")
set_tests_properties(test_vehicle_smoke_script__pending_transfer PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__pending_transfer;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__pending_transfer")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__save")
add_test(NAME test_vehicle_smoke_script__save COMMAND test_vehicle_smoke_script save WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__save")
set_tests_properties(test_vehicle_smoke_script__save PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__save;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__save")
# vehicle-smoke-1004 O11 (S03): dispatch-seam
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dispatch_seam")
add_test(NAME test_vehicle_smoke_script__dispatch_seam COMMAND test_vehicle_smoke_script dispatch-seam WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dispatch_seam")
set_tests_properties(test_vehicle_smoke_script__dispatch_seam PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dispatch_seam;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dispatch_seam")
# vehicle-smoke-1004 O11 (S03): dispatch-guest
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dispatch_guest")
add_test(NAME test_vehicle_smoke_script__dispatch_guest COMMAND test_vehicle_smoke_script dispatch-guest WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dispatch_guest")
set_tests_properties(test_vehicle_smoke_script__dispatch_guest PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dispatch_guest;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dispatch_guest")
# vehicle-smoke-1004 O14: two-home geom
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_geom")
add_test(NAME test_vehicle_smoke_script__two_home_geom COMMAND test_vehicle_smoke_script two-home geom WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_geom")
set_tests_properties(test_vehicle_smoke_script__two_home_geom PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_geom;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_geom")
# vehicle-smoke-1004 O14: two-home envelope
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_envelope")
add_test(NAME test_vehicle_smoke_script__two_home_envelope COMMAND test_vehicle_smoke_script two-home envelope WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_envelope")
set_tests_properties(test_vehicle_smoke_script__two_home_envelope PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_envelope;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_envelope")
# vehicle-smoke-1004 O14: two-home far
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_far")
add_test(NAME test_vehicle_smoke_script__two_home_far COMMAND test_vehicle_smoke_script two-home far WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_far")
set_tests_properties(test_vehicle_smoke_script__two_home_far PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_far;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_far")
# vehicle-smoke-1004 O14: two-home lethal
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_lethal")
add_test(NAME test_vehicle_smoke_script__two_home_lethal COMMAND test_vehicle_smoke_script two-home lethal WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_lethal")
set_tests_properties(test_vehicle_smoke_script__two_home_lethal PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_lethal;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__two_home_lethal")
# vehicle-smoke-1004 O15: hull-aim
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__hull_aim")
add_test(NAME test_vehicle_smoke_script__hull_aim COMMAND test_vehicle_smoke_script hull-aim WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__hull_aim")
set_tests_properties(test_vehicle_smoke_script__hull_aim PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__hull_aim;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__hull_aim")
# vehicle-smoke-1004 O25: boat-yaw
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__boat_yaw")
add_test(NAME test_vehicle_smoke_script__boat_yaw COMMAND test_vehicle_smoke_script boat-yaw WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__boat_yaw")
set_tests_properties(test_vehicle_smoke_script__boat_yaw PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__boat_yaw;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__boat_yaw")
# vehicle-smoke-1004 O16: corner-guard
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__corner_guard")
add_test(NAME test_vehicle_smoke_script__corner_guard COMMAND test_vehicle_smoke_script corner-guard WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__corner_guard")
set_tests_properties(test_vehicle_smoke_script__corner_guard PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__corner_guard;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__corner_guard")
# vehicle-smoke-1004 O13 (G6): death-scatter-host
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__death_scatter_host")
add_test(NAME test_vehicle_smoke_script__death_scatter_host COMMAND test_vehicle_smoke_script death-scatter-host WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__death_scatter_host")
set_tests_properties(test_vehicle_smoke_script__death_scatter_host PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__death_scatter_host;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__death_scatter_host")
# vehicle-smoke-1004 O11 (S03): pickup-negative
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__pickup_negative")
add_test(NAME test_vehicle_smoke_script__pickup_negative COMMAND test_vehicle_smoke_script pickup-negative WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__pickup_negative")
set_tests_properties(test_vehicle_smoke_script__pickup_negative PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__pickup_negative;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__pickup_negative")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dormant")
add_test(NAME test_vehicle_smoke_script__dormant COMMAND test_vehicle_smoke_script dormant WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dormant")
set_tests_properties(test_vehicle_smoke_script__dormant PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dormant;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__dormant")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__driver")
add_test(NAME test_vehicle_smoke_script__driver COMMAND test_vehicle_smoke_script driver WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__driver")
set_tests_properties(test_vehicle_smoke_script__driver PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__driver;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__driver")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__hw_run")
add_test(NAME test_vehicle_smoke_script__hw_run COMMAND test_vehicle_smoke_script hw-run WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__hw_run")
set_tests_properties(test_vehicle_smoke_script__hw_run PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__hw_run;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__hw_run")
# CONTRACT-v1.1 (ASTRA-O01-RULING Q3/Q5): real client dispatch census; per-actor input epochs.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__client_dispatch")
add_test(NAME test_vehicle_smoke_script__client_dispatch COMMAND test_vehicle_smoke_script client_dispatch WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__client_dispatch")
set_tests_properties(test_vehicle_smoke_script__client_dispatch PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__client_dispatch;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__client_dispatch")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__epochs")
add_test(NAME test_vehicle_smoke_script__epochs COMMAND test_vehicle_smoke_script epochs WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__epochs")
set_tests_properties(test_vehicle_smoke_script__epochs PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__epochs;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__epochs")
# O03: host rides a cart with P2 present -> zero slot-1 cart controls (production fix in entity_local_player.c).
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__cart_route")
add_test(NAME test_vehicle_smoke_script__cart_route COMMAND test_vehicle_smoke_script cart_route WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__cart_route")
set_tests_properties(test_vehicle_smoke_script__cart_route PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__cart_route;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_smoke/test_vehicle_smoke_script__cart_route")
# vehicle-smoke-1004 (G01, Sol): the host_vehicle_smoke_8m collector contract
# -- closed schemas, seq/retry, phase/operation/serial predicates and the
# three-kind matrix, graded through the live parser on synthetic tapes.
add_test(NAME test_vehicle_smoke_evidence COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vehicle_smoke_evidence.py)
add_executable(test_vehicle_lifecycle_seams tests/test_vehicle_lifecycle_seams.c)
target_link_libraries(test_vehicle_lifecycle_seams cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_vehicle_lifecycle_seams PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_vehicle_lifecycle_seams PROPERTIES C_STANDARD 99)
target_link_libraries(test_vehicle_lifecycle_seams ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__wire_ids")
add_test(NAME test_vehicle_owner_seams__wire_ids COMMAND test_vehicle_owner_seams wire-ids WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__wire_ids")
set_tests_properties(test_vehicle_owner_seams__wire_ids PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__wire_ids;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__wire_ids")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_0")
add_test(NAME test_vehicle_owner_seams__other_rider_0 COMMAND test_vehicle_owner_seams other-rider 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_0")
set_tests_properties(test_vehicle_owner_seams__other_rider_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_1")
add_test(NAME test_vehicle_owner_seams__other_rider_1 COMMAND test_vehicle_owner_seams other-rider 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_1")
set_tests_properties(test_vehicle_owner_seams__other_rider_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_2")
add_test(NAME test_vehicle_owner_seams__other_rider_2 COMMAND test_vehicle_owner_seams other-rider 2 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_2")
set_tests_properties(test_vehicle_owner_seams__other_rider_2 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_2;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__other_rider_2")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_1_0")
add_test(NAME test_vehicle_owner_seams__switch_1_0 COMMAND test_vehicle_owner_seams switch 1 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_1_0")
set_tests_properties(test_vehicle_owner_seams__switch_1_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_1_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_1_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_1_1")
add_test(NAME test_vehicle_owner_seams__switch_1_1 COMMAND test_vehicle_owner_seams switch 1 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_1_1")
set_tests_properties(test_vehicle_owner_seams__switch_1_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_1_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_1_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_2_0")
add_test(NAME test_vehicle_owner_seams__switch_2_0 COMMAND test_vehicle_owner_seams switch 2 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_2_0")
set_tests_properties(test_vehicle_owner_seams__switch_2_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_2_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_2_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_2_1")
add_test(NAME test_vehicle_owner_seams__switch_2_1 COMMAND test_vehicle_owner_seams switch 2 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_2_1")
set_tests_properties(test_vehicle_owner_seams__switch_2_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_2_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_2_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_3_0")
add_test(NAME test_vehicle_owner_seams__switch_3_0 COMMAND test_vehicle_owner_seams switch 3 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_3_0")
set_tests_properties(test_vehicle_owner_seams__switch_3_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_3_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_3_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_3_1")
add_test(NAME test_vehicle_owner_seams__switch_3_1 COMMAND test_vehicle_owner_seams switch 3 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_3_1")
set_tests_properties(test_vehicle_owner_seams__switch_3_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_3_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_3_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_5_0")
add_test(NAME test_vehicle_owner_seams__switch_5_0 COMMAND test_vehicle_owner_seams switch 5 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_5_0")
set_tests_properties(test_vehicle_owner_seams__switch_5_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_5_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_5_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_5_1")
add_test(NAME test_vehicle_owner_seams__switch_5_1 COMMAND test_vehicle_owner_seams switch 5 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_5_1")
set_tests_properties(test_vehicle_owner_seams__switch_5_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_5_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_5_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_6_0")
add_test(NAME test_vehicle_owner_seams__switch_6_0 COMMAND test_vehicle_owner_seams switch 6 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_6_0")
set_tests_properties(test_vehicle_owner_seams__switch_6_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_6_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_6_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_6_1")
add_test(NAME test_vehicle_owner_seams__switch_6_1 COMMAND test_vehicle_owner_seams switch 6 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_6_1")
set_tests_properties(test_vehicle_owner_seams__switch_6_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_6_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_6_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_7_0")
add_test(NAME test_vehicle_owner_seams__switch_7_0 COMMAND test_vehicle_owner_seams switch 7 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_7_0")
set_tests_properties(test_vehicle_owner_seams__switch_7_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_7_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_7_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_7_1")
add_test(NAME test_vehicle_owner_seams__switch_7_1 COMMAND test_vehicle_owner_seams switch 7 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_7_1")
set_tests_properties(test_vehicle_owner_seams__switch_7_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_7_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__switch_7_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__wrong_world")
add_test(NAME test_vehicle_owner_seams__wrong_world COMMAND test_vehicle_owner_seams wrong-world WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__wrong_world")
set_tests_properties(test_vehicle_owner_seams__wrong_world PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__wrong_world;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__wrong_world")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_lifetime")
add_test(NAME test_vehicle_owner_seams__stale_lifetime COMMAND test_vehicle_owner_seams stale-lifetime WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_lifetime")
set_tests_properties(test_vehicle_owner_seams__stale_lifetime PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_lifetime;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_lifetime")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__destroy")
add_test(NAME test_vehicle_owner_seams__destroy COMMAND test_vehicle_owner_seams destroy WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__destroy")
set_tests_properties(test_vehicle_owner_seams__destroy PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__destroy;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__destroy")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_correction")
add_test(NAME test_vehicle_owner_seams__pig_correction COMMAND test_vehicle_owner_seams pig-correction WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_correction")
set_tests_properties(test_vehicle_owner_seams__pig_correction PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_correction;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_correction")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_retry")
add_test(NAME test_vehicle_owner_seams__pig_retry COMMAND test_vehicle_owner_seams pig-retry WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_retry")
set_tests_properties(test_vehicle_owner_seams__pig_retry PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_retry;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_retry")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_wrong_world")
add_test(NAME test_vehicle_owner_seams__pig_wrong_world COMMAND test_vehicle_owner_seams pig-wrong-world WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_wrong_world")
set_tests_properties(test_vehicle_owner_seams__pig_wrong_world PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_wrong_world;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_wrong_world")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_stale_lifetime")
add_test(NAME test_vehicle_owner_seams__pig_stale_lifetime COMMAND test_vehicle_owner_seams pig-stale-lifetime WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_stale_lifetime")
set_tests_properties(test_vehicle_owner_seams__pig_stale_lifetime PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_stale_lifetime;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_stale_lifetime")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_0_0")
add_test(NAME test_vehicle_owner_seams__invalid_release_0_0 COMMAND test_vehicle_owner_seams invalid-release 0 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_0_0")
set_tests_properties(test_vehicle_owner_seams__invalid_release_0_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_0_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_0_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_0_1")
add_test(NAME test_vehicle_owner_seams__invalid_release_0_1 COMMAND test_vehicle_owner_seams invalid-release 0 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_0_1")
set_tests_properties(test_vehicle_owner_seams__invalid_release_0_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_0_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_0_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_2_0")
add_test(NAME test_vehicle_owner_seams__invalid_release_2_0 COMMAND test_vehicle_owner_seams invalid-release 2 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_2_0")
set_tests_properties(test_vehicle_owner_seams__invalid_release_2_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_2_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__invalid_release_2_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_vehicle_0")
add_test(NAME test_vehicle_owner_seams__stale_vehicle_0 COMMAND test_vehicle_owner_seams stale-vehicle 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_vehicle_0")
set_tests_properties(test_vehicle_owner_seams__stale_vehicle_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_vehicle_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_vehicle_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_vehicle_2")
add_test(NAME test_vehicle_owner_seams__stale_vehicle_2 COMMAND test_vehicle_owner_seams stale-vehicle 2 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_vehicle_2")
set_tests_properties(test_vehicle_owner_seams__stale_vehicle_2 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_vehicle_2;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__stale_vehicle_2")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__frozen_pig_anchor")
add_test(NAME test_vehicle_owner_seams__frozen_pig_anchor COMMAND test_vehicle_owner_seams frozen-pig-anchor WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__frozen_pig_anchor")
set_tests_properties(test_vehicle_owner_seams__frozen_pig_anchor PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__frozen_pig_anchor;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__frozen_pig_anchor")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_walking")
add_test(NAME test_vehicle_owner_seams__pig_walking COMMAND test_vehicle_owner_seams pig-walking WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_walking")
set_tests_properties(test_vehicle_owner_seams__pig_walking PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_walking;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__pig_walking")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_0")
add_test(NAME test_vehicle_client_seams__follow_0 COMMAND test_vehicle_client_seams follow 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_0")
set_tests_properties(test_vehicle_client_seams__follow_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_1")
add_test(NAME test_vehicle_client_seams__follow_1 COMMAND test_vehicle_client_seams follow 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_1")
set_tests_properties(test_vehicle_client_seams__follow_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_2")
add_test(NAME test_vehicle_client_seams__follow_2 COMMAND test_vehicle_client_seams follow 2 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_2")
set_tests_properties(test_vehicle_client_seams__follow_2 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_2;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__follow_2")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_0")
add_test(NAME test_vehicle_client_seams__host_equiv_0 COMMAND test_vehicle_client_seams host-equiv 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_0")
set_tests_properties(test_vehicle_client_seams__host_equiv_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_1")
add_test(NAME test_vehicle_client_seams__host_equiv_1 COMMAND test_vehicle_client_seams host-equiv 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_1")
set_tests_properties(test_vehicle_client_seams__host_equiv_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_2")
add_test(NAME test_vehicle_client_seams__host_equiv_2 COMMAND test_vehicle_client_seams host-equiv 2 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_2")
set_tests_properties(test_vehicle_client_seams__host_equiv_2 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_2;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__host_equiv_2")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_0")
add_test(NAME test_vehicle_client_seams__once_0 COMMAND test_vehicle_client_seams once 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_0")
set_tests_properties(test_vehicle_client_seams__once_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_1")
add_test(NAME test_vehicle_client_seams__once_1 COMMAND test_vehicle_client_seams once 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_1")
set_tests_properties(test_vehicle_client_seams__once_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_2")
add_test(NAME test_vehicle_client_seams__once_2 COMMAND test_vehicle_client_seams once 2 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_2")
set_tests_properties(test_vehicle_client_seams__once_2 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_2;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__once_2")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__unmounted_equiv")
add_test(NAME test_vehicle_client_seams__unmounted_equiv COMMAND test_vehicle_client_seams unmounted-equiv WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__unmounted_equiv")
set_tests_properties(test_vehicle_client_seams__unmounted_equiv PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__unmounted_equiv;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__unmounted_equiv")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__same_world")
add_test(NAME test_vehicle_client_seams__same_world COMMAND test_vehicle_client_seams same-world WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__same_world")
set_tests_properties(test_vehicle_client_seams__same_world PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__same_world;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__same_world")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__separate_world")
add_test(NAME test_vehicle_client_seams__separate_world COMMAND test_vehicle_client_seams separate-world WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__separate_world")
set_tests_properties(test_vehicle_client_seams__separate_world PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__separate_world;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__separate_world")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__late_bind")
add_test(NAME test_vehicle_client_seams__late_bind COMMAND test_vehicle_client_seams late-bind WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__late_bind")
set_tests_properties(test_vehicle_client_seams__late_bind PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__late_bind;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__late_bind")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__stale_stamp")
add_test(NAME test_vehicle_client_seams__stale_stamp COMMAND test_vehicle_client_seams stale-stamp WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__stale_stamp")
set_tests_properties(test_vehicle_client_seams__stale_stamp PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__stale_stamp;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__stale_stamp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__pressure")
add_test(NAME test_vehicle_client_seams__pressure COMMAND test_vehicle_client_seams pressure WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__pressure")
set_tests_properties(test_vehicle_client_seams__pressure PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__pressure;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__pressure")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__steering_clear_1")
add_test(NAME test_vehicle_client_seams__steering_clear_1 COMMAND test_vehicle_client_seams steering-clear 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__steering_clear_1")
set_tests_properties(test_vehicle_client_seams__steering_clear_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__steering_clear_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__steering_clear_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__steering_clear_2")
add_test(NAME test_vehicle_client_seams__steering_clear_2 COMMAND test_vehicle_client_seams steering-clear 2 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__steering_clear_2")
set_tests_properties(test_vehicle_client_seams__steering_clear_2 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__steering_clear_2;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__steering_clear_2")
# R1's producer-backed client coverage, registered with ordinary passing exits.
foreach(_case input-owner pig-seat-correction world-follow_0 world-follow_1)
	set(_args "${_case}")
	if(_case MATCHES "^world-follow_([01])$")
		set(_args world-follow "${CMAKE_MATCH_1}")
	endif()
	string(REPLACE "-" "_" _name "${_case}")
	set(_dir "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_client_seams__${_name}")
	file(MAKE_DIRECTORY "${_dir}")
	add_test(NAME test_vehicle_client_seams__${_name} COMMAND test_vehicle_client_seams ${_args} WORKING_DIRECTORY "${_dir}")
	set_tests_properties(test_vehicle_client_seams__${_name} PROPERTIES ENVIRONMENT "TMP=${_dir};TEMP=${_dir}" TIMEOUT 300)
endforeach()
foreach(_case admission disconnect host-trace)
	foreach(_kind 0 1)
		string(REPLACE "-" "_" _name "${_case}_${_kind}")
		set(_dir "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__${_name}")
		file(MAKE_DIRECTORY "${_dir}")
		add_test(NAME test_vehicle_owner_seams__${_name} COMMAND test_vehicle_owner_seams ${_case} ${_kind} WORKING_DIRECTORY "${_dir}")
		set_tests_properties(test_vehicle_owner_seams__${_name} PROPERTIES ENVIRONMENT "TMP=${_dir};TEMP=${_dir}" TIMEOUT 300)
	endforeach()
endforeach()
foreach(_pair 1 2 3 5 6 7)
	set(_dir "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_owner_seams__guest_switch_${_pair}")
	file(MAKE_DIRECTORY "${_dir}")
	add_test(NAME test_vehicle_owner_seams__guest_switch_${_pair} COMMAND test_vehicle_owner_seams guest-switch ${_pair} WORKING_DIRECTORY "${_dir}")
	set_tests_properties(test_vehicle_owner_seams__guest_switch_${_pair} PROPERTIES ENVIRONMENT "TMP=${_dir};TEMP=${_dir}" TIMEOUT 300)
endforeach()
foreach(_kind 0 1)
	foreach(_world 0 1)
		set(_dir "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__destroy_${_kind}_${_world}")
		file(MAKE_DIRECTORY "${_dir}")
		add_test(NAME test_vehicle_lifecycle_seams__destroy_${_kind}_${_world} COMMAND test_vehicle_lifecycle_seams destroy ${_kind} ${_world} WORKING_DIRECTORY "${_dir}")
		set_tests_properties(test_vehicle_lifecycle_seams__destroy_${_kind}_${_world} PROPERTIES ENVIRONMENT "TMP=${_dir};TEMP=${_dir}" TIMEOUT 300)
	endforeach()
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_0_0")
add_test(NAME test_vehicle_lifecycle_seams__save_0_0 COMMAND test_vehicle_lifecycle_seams save 0 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_0_0")
set_tests_properties(test_vehicle_lifecycle_seams__save_0_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_0_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_0_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_0_1")
add_test(NAME test_vehicle_lifecycle_seams__save_0_1 COMMAND test_vehicle_lifecycle_seams save 0 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_0_1")
set_tests_properties(test_vehicle_lifecycle_seams__save_0_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_0_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_0_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_1_0")
add_test(NAME test_vehicle_lifecycle_seams__save_1_0 COMMAND test_vehicle_lifecycle_seams save 1 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_1_0")
set_tests_properties(test_vehicle_lifecycle_seams__save_1_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_1_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_1_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_1_1")
add_test(NAME test_vehicle_lifecycle_seams__save_1_1 COMMAND test_vehicle_lifecycle_seams save 1 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_1_1")
set_tests_properties(test_vehicle_lifecycle_seams__save_1_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_1_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_1_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_2_0")
add_test(NAME test_vehicle_lifecycle_seams__save_2_0 COMMAND test_vehicle_lifecycle_seams save 2 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_2_0")
set_tests_properties(test_vehicle_lifecycle_seams__save_2_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_2_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_2_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_2_1")
add_test(NAME test_vehicle_lifecycle_seams__save_2_1 COMMAND test_vehicle_lifecycle_seams save 2 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_2_1")
set_tests_properties(test_vehicle_lifecycle_seams__save_2_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_2_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__save_2_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_0_0")
add_test(NAME test_vehicle_lifecycle_seams__leave_0_0 COMMAND test_vehicle_lifecycle_seams leave 0 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_0_0")
set_tests_properties(test_vehicle_lifecycle_seams__leave_0_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_0_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_0_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_0_1")
add_test(NAME test_vehicle_lifecycle_seams__leave_0_1 COMMAND test_vehicle_lifecycle_seams leave 0 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_0_1")
set_tests_properties(test_vehicle_lifecycle_seams__leave_0_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_0_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_0_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_1_0")
add_test(NAME test_vehicle_lifecycle_seams__leave_1_0 COMMAND test_vehicle_lifecycle_seams leave 1 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_1_0")
set_tests_properties(test_vehicle_lifecycle_seams__leave_1_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_1_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_1_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_1_1")
add_test(NAME test_vehicle_lifecycle_seams__leave_1_1 COMMAND test_vehicle_lifecycle_seams leave 1 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_1_1")
set_tests_properties(test_vehicle_lifecycle_seams__leave_1_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_1_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_1_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_2_0")
add_test(NAME test_vehicle_lifecycle_seams__leave_2_0 COMMAND test_vehicle_lifecycle_seams leave 2 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_2_0")
set_tests_properties(test_vehicle_lifecycle_seams__leave_2_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_2_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_2_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_2_1")
add_test(NAME test_vehicle_lifecycle_seams__leave_2_1 COMMAND test_vehicle_lifecycle_seams leave 2 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_2_1")
set_tests_properties(test_vehicle_lifecycle_seams__leave_2_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_2_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__leave_2_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_0_0")
add_test(NAME test_vehicle_lifecycle_seams__checkpoint_failure_0_0 COMMAND test_vehicle_lifecycle_seams checkpoint-failure 0 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_0_0")
set_tests_properties(test_vehicle_lifecycle_seams__checkpoint_failure_0_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_0_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_0_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_0_1")
add_test(NAME test_vehicle_lifecycle_seams__checkpoint_failure_0_1 COMMAND test_vehicle_lifecycle_seams checkpoint-failure 0 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_0_1")
set_tests_properties(test_vehicle_lifecycle_seams__checkpoint_failure_0_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_0_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_0_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_1_0")
add_test(NAME test_vehicle_lifecycle_seams__checkpoint_failure_1_0 COMMAND test_vehicle_lifecycle_seams checkpoint-failure 1 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_1_0")
set_tests_properties(test_vehicle_lifecycle_seams__checkpoint_failure_1_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_1_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_1_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_1_1")
add_test(NAME test_vehicle_lifecycle_seams__checkpoint_failure_1_1 COMMAND test_vehicle_lifecycle_seams checkpoint-failure 1 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_1_1")
set_tests_properties(test_vehicle_lifecycle_seams__checkpoint_failure_1_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_1_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_1_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_2_0")
add_test(NAME test_vehicle_lifecycle_seams__checkpoint_failure_2_0 COMMAND test_vehicle_lifecycle_seams checkpoint-failure 2 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_2_0")
set_tests_properties(test_vehicle_lifecycle_seams__checkpoint_failure_2_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_2_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_2_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_2_1")
add_test(NAME test_vehicle_lifecycle_seams__checkpoint_failure_2_1 COMMAND test_vehicle_lifecycle_seams checkpoint-failure 2 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_2_1")
set_tests_properties(test_vehicle_lifecycle_seams__checkpoint_failure_2_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_2_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__checkpoint_failure_2_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_0_0")
add_test(NAME test_vehicle_lifecycle_seams__allocation_failure_0_0 COMMAND test_vehicle_lifecycle_seams allocation-failure 0 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_0_0")
set_tests_properties(test_vehicle_lifecycle_seams__allocation_failure_0_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_0_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_0_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_0_1")
add_test(NAME test_vehicle_lifecycle_seams__allocation_failure_0_1 COMMAND test_vehicle_lifecycle_seams allocation-failure 0 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_0_1")
set_tests_properties(test_vehicle_lifecycle_seams__allocation_failure_0_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_0_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_0_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_1_0")
add_test(NAME test_vehicle_lifecycle_seams__allocation_failure_1_0 COMMAND test_vehicle_lifecycle_seams allocation-failure 1 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_1_0")
set_tests_properties(test_vehicle_lifecycle_seams__allocation_failure_1_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_1_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_1_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_1_1")
add_test(NAME test_vehicle_lifecycle_seams__allocation_failure_1_1 COMMAND test_vehicle_lifecycle_seams allocation-failure 1 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_1_1")
set_tests_properties(test_vehicle_lifecycle_seams__allocation_failure_1_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_1_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_1_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_2_0")
add_test(NAME test_vehicle_lifecycle_seams__allocation_failure_2_0 COMMAND test_vehicle_lifecycle_seams allocation-failure 2 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_2_0")
set_tests_properties(test_vehicle_lifecycle_seams__allocation_failure_2_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_2_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_2_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_2_1")
add_test(NAME test_vehicle_lifecycle_seams__allocation_failure_2_1 COMMAND test_vehicle_lifecycle_seams allocation-failure 2 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_2_1")
set_tests_properties(test_vehicle_lifecycle_seams__allocation_failure_2_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_2_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__allocation_failure_2_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_0_0")
add_test(NAME test_vehicle_lifecycle_seams__death_0_0 COMMAND test_vehicle_lifecycle_seams death 0 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_0_0")
set_tests_properties(test_vehicle_lifecycle_seams__death_0_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_0_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_0_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_0_1")
add_test(NAME test_vehicle_lifecycle_seams__death_0_1 COMMAND test_vehicle_lifecycle_seams death 0 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_0_1")
set_tests_properties(test_vehicle_lifecycle_seams__death_0_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_0_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_0_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_1_0")
add_test(NAME test_vehicle_lifecycle_seams__death_1_0 COMMAND test_vehicle_lifecycle_seams death 1 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_1_0")
set_tests_properties(test_vehicle_lifecycle_seams__death_1_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_1_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_1_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_1_1")
add_test(NAME test_vehicle_lifecycle_seams__death_1_1 COMMAND test_vehicle_lifecycle_seams death 1 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_1_1")
set_tests_properties(test_vehicle_lifecycle_seams__death_1_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_1_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_1_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_2_0")
add_test(NAME test_vehicle_lifecycle_seams__death_2_0 COMMAND test_vehicle_lifecycle_seams death 2 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_2_0")
set_tests_properties(test_vehicle_lifecycle_seams__death_2_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_2_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_2_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_2_1")
add_test(NAME test_vehicle_lifecycle_seams__death_2_1 COMMAND test_vehicle_lifecycle_seams death 2 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_2_1")
set_tests_properties(test_vehicle_lifecycle_seams__death_2_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_2_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__death_2_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_0_0")
add_test(NAME test_vehicle_lifecycle_seams__host_respawn_0_0 COMMAND test_vehicle_lifecycle_seams host-respawn 0 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_0_0")
set_tests_properties(test_vehicle_lifecycle_seams__host_respawn_0_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_0_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_0_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_0_1")
add_test(NAME test_vehicle_lifecycle_seams__host_respawn_0_1 COMMAND test_vehicle_lifecycle_seams host-respawn 0 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_0_1")
set_tests_properties(test_vehicle_lifecycle_seams__host_respawn_0_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_0_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_0_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_1_0")
add_test(NAME test_vehicle_lifecycle_seams__host_respawn_1_0 COMMAND test_vehicle_lifecycle_seams host-respawn 1 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_1_0")
set_tests_properties(test_vehicle_lifecycle_seams__host_respawn_1_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_1_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_1_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_1_1")
add_test(NAME test_vehicle_lifecycle_seams__host_respawn_1_1 COMMAND test_vehicle_lifecycle_seams host-respawn 1 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_1_1")
set_tests_properties(test_vehicle_lifecycle_seams__host_respawn_1_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_1_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_1_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_2_0")
add_test(NAME test_vehicle_lifecycle_seams__host_respawn_2_0 COMMAND test_vehicle_lifecycle_seams host-respawn 2 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_2_0")
set_tests_properties(test_vehicle_lifecycle_seams__host_respawn_2_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_2_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_2_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_2_1")
add_test(NAME test_vehicle_lifecycle_seams__host_respawn_2_1 COMMAND test_vehicle_lifecycle_seams host-respawn 2 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_2_1")
set_tests_properties(test_vehicle_lifecycle_seams__host_respawn_2_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_2_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__host_respawn_2_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_0_0")
add_test(NAME test_vehicle_lifecycle_seams__transfer_0_0 COMMAND test_vehicle_lifecycle_seams transfer 0 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_0_0")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_0_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_0_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_0_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_0_1")
add_test(NAME test_vehicle_lifecycle_seams__transfer_0_1 COMMAND test_vehicle_lifecycle_seams transfer 0 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_0_1")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_0_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_0_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_0_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_1_0")
add_test(NAME test_vehicle_lifecycle_seams__transfer_1_0 COMMAND test_vehicle_lifecycle_seams transfer 1 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_1_0")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_1_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_1_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_1_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_1_1")
add_test(NAME test_vehicle_lifecycle_seams__transfer_1_1 COMMAND test_vehicle_lifecycle_seams transfer 1 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_1_1")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_1_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_1_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_1_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_2_0")
add_test(NAME test_vehicle_lifecycle_seams__transfer_2_0 COMMAND test_vehicle_lifecycle_seams transfer 2 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_2_0")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_2_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_2_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_2_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_2_1")
add_test(NAME test_vehicle_lifecycle_seams__transfer_2_1 COMMAND test_vehicle_lifecycle_seams transfer 2 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_2_1")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_2_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_2_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_2_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_0_0")
add_test(NAME test_vehicle_lifecycle_seams__transfer_refusal_0_0 COMMAND test_vehicle_lifecycle_seams transfer-refusal 0 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_0_0")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_refusal_0_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_0_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_0_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_0_1")
add_test(NAME test_vehicle_lifecycle_seams__transfer_refusal_0_1 COMMAND test_vehicle_lifecycle_seams transfer-refusal 0 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_0_1")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_refusal_0_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_0_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_0_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_1_0")
add_test(NAME test_vehicle_lifecycle_seams__transfer_refusal_1_0 COMMAND test_vehicle_lifecycle_seams transfer-refusal 1 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_1_0")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_refusal_1_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_1_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_1_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_1_1")
add_test(NAME test_vehicle_lifecycle_seams__transfer_refusal_1_1 COMMAND test_vehicle_lifecycle_seams transfer-refusal 1 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_1_1")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_refusal_1_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_1_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_1_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_2_0")
add_test(NAME test_vehicle_lifecycle_seams__transfer_refusal_2_0 COMMAND test_vehicle_lifecycle_seams transfer-refusal 2 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_2_0")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_refusal_2_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_2_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_2_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_2_1")
add_test(NAME test_vehicle_lifecycle_seams__transfer_refusal_2_1 COMMAND test_vehicle_lifecycle_seams transfer-refusal 2 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_2_1")
set_tests_properties(test_vehicle_lifecycle_seams__transfer_refusal_2_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_2_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__transfer_refusal_2_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__accept_host_transfer")
add_test(NAME test_vehicle_lifecycle_seams__accept_host_transfer COMMAND test_vehicle_lifecycle_seams accept-host-transfer WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__accept_host_transfer")
set_tests_properties(test_vehicle_lifecycle_seams__accept_host_transfer PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__accept_host_transfer;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__accept_host_transfer")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_hazards")
add_test(NAME test_vehicle_lifecycle_seams__respawn_hazards COMMAND test_vehicle_lifecycle_seams respawn-hazards WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_hazards")
set_tests_properties(test_vehicle_lifecycle_seams__respawn_hazards PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_hazards;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_hazards")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_melee")
add_test(NAME test_vehicle_lifecycle_seams__respawn_melee COMMAND test_vehicle_lifecycle_seams respawn-melee WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_melee")
set_tests_properties(test_vehicle_lifecycle_seams__respawn_melee PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_melee;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_melee")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_0")
add_test(NAME test_vehicle_lifecycle_seams__respawn_damage_core_0 COMMAND test_vehicle_lifecycle_seams respawn-damage-core 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_0")
set_tests_properties(test_vehicle_lifecycle_seams__respawn_damage_core_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_1")
add_test(NAME test_vehicle_lifecycle_seams__respawn_damage_core_1 COMMAND test_vehicle_lifecycle_seams respawn-damage-core 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_1")
set_tests_properties(test_vehicle_lifecycle_seams__respawn_damage_core_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_2")
add_test(NAME test_vehicle_lifecycle_seams__respawn_damage_core_2 COMMAND test_vehicle_lifecycle_seams respawn-damage-core 2 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_2")
set_tests_properties(test_vehicle_lifecycle_seams__respawn_damage_core_2 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_2;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_vehicle_lifecycle_seams__respawn_damage_core_2")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_0_0")
add_test(NAME test_local_guest__vehicle_record_0_0 COMMAND test_local_guest vehicle-record 0 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_0_0")
set_tests_properties(test_local_guest__vehicle_record_0_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_0_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_0_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_0_1")
add_test(NAME test_local_guest__vehicle_record_0_1 COMMAND test_local_guest vehicle-record 0 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_0_1")
set_tests_properties(test_local_guest__vehicle_record_0_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_0_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_0_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_1_0")
add_test(NAME test_local_guest__vehicle_record_1_0 COMMAND test_local_guest vehicle-record 1 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_1_0")
set_tests_properties(test_local_guest__vehicle_record_1_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_1_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_1_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_1_1")
add_test(NAME test_local_guest__vehicle_record_1_1 COMMAND test_local_guest vehicle-record 1 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_1_1")
set_tests_properties(test_local_guest__vehicle_record_1_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_1_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_1_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_2_0")
add_test(NAME test_local_guest__vehicle_record_2_0 COMMAND test_local_guest vehicle-record 2 0 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_2_0")
set_tests_properties(test_local_guest__vehicle_record_2_0 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_2_0;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_2_0")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_2_1")
add_test(NAME test_local_guest__vehicle_record_2_1 COMMAND test_local_guest vehicle-record 2 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_2_1")
set_tests_properties(test_local_guest__vehicle_record_2_1 PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_2_1;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__vehicle_record_2_1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__publication_retry")
add_test(NAME test_local_guest__publication_retry COMMAND test_local_guest publication-retry WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__publication_retry")
set_tests_properties(test_local_guest__publication_retry PROPERTIES ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__publication_retry;TEMP=${CMAKE_CURRENT_BINARY_DIR}/vehicle_seams/test_local_guest__publication_retry")

# Beta Entity.isWet rain quench across host, guest, and mob production hazards.
add_executable(test_rain_quench_golden tests/test_rain_quench_golden.c)
target_link_libraries(test_rain_quench_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_rain_quench_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_rain_quench_golden PROPERTIES C_STANDARD 99)
add_test(NAME test_rain_quench_golden COMMAND test_rain_quench_golden)

# Experimental local split-screen (lane/guest-body, 2026-09-07): the CLIENT
# half of gameplay admission. test_local_guest already drives the server gate
# end to end; these two pin what may -- and may NOT -- set
# client_session.local_gameplay_ready, the single gate main.c's two-view loop
# reads. Client-session + input-inventory only: no world, no server thread.
# S1-d (mp-split-0912/68): the SERVER half of the split_actions_6m proof --
# per-actor accepted-action counters and their main-thread mirror. Deliberately
# its own binary rather than more of test_local_guest: that file already owns
# the admission/lifetime contract, and a parallel fix round on either lane
# should not have to merge through the other.
# split_recovery_6m (2026-09-17, robot-recovery-0917): the scripted recovery
# phase driver. Links the standard full engine because the happy path is driven
# against the PRODUCTION handlers (local_menu_host_recovery_frame -> the real
# controller_recovery machine, local_pause_update -> the real guest-leave and
# host-exit transitions), not against reimplementations. audio_menu_event is
# wrapped exactly as test_local_pause wraps it (no audio device in a test).
add_executable(test_split_recovery_script tests/test_split_recovery_script.c)
target_link_libraries(test_split_recovery_script cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_recovery_script PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_recovery_script PROPERTIES C_STANDARD 99)
target_link_options(test_split_recovery_script PRIVATE "-Wl,--wrap=audio_menu_event")
add_test(NAME test_split_recovery_script COMMAND test_split_recovery_script)

# mp_chat_login_3m (2026-09-17, b1-chat-keyboard-0917): the scripted chat/OSK
# command-delivery phase driver. Same shape as test_split_recovery_script above
# and for the same reason -- the happy path is driven against the PRODUCTION
# handlers (screen_chat_update -> the real osk_chat_update_owned, through
# input.c's SEAM S1 and the unchanged input_owner_latch), not against
# reimplementations -- so it links the standard full engine and wraps
# audio_menu_event the same way.
add_executable(test_chat_login_script tests/test_chat_login_script.c)
target_link_libraries(test_chat_login_script cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_chat_login_script PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_chat_login_script PROPERTIES C_STANDARD 99)
target_link_options(test_chat_login_script PRIVATE "-Wl,--wrap=audio_menu_event,--wrap=gfx_width,--wrap=gfx_height")
add_test(NAME test_chat_login_script COMMAND test_chat_login_script)

# mp_play_10m (2026-09-21, push-0921/B3 + B4b). Two FOCUSED tests that link
# NOTHING: source/game/mp_play_core.h is a pure phase machine and
# source/game/mp_play_map.h is pure translation, exactly like
# mp_reconnect_soak_core.h / autopilot_unstick.h before them. They compile only
# their own tests/ file, so the build-graph check's engine-TU ceiling and the
# "no pool link plus a direct engine source" rule are both trivially satisfied.
# The impure half (mp_play_script.c) is proved by the suite's own input /
# container / pause / autopilot tests, not here -- each test's header says so.
add_executable(test_mp_play_core tests/test_mp_play_core.c)
target_include_directories(test_mp_play_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
# mp_play_core.h:1061 (atan2f) and the test's own sin/cos/sincos calls need libm
# on Linux gcc; MinGW links it implicitly, which is why only tools/ci.sh saw the
# undefined references (integration-0924 local_ci, 2026-09-24).
target_link_libraries(test_mp_play_core m)
set_target_properties(test_mp_play_core PROPERTIES C_STANDARD 99)
add_test(NAME test_mp_play_core COMMAND test_mp_play_core)

# hw1023c: source lint. Every gfx_draw_quads_flt (direct GX CLR0) in
# source/entity/*.c and source/graphics/render_entity*.c must sit inside
# gfx_lighting(false) ... gfx_lighting(true) in its own function; an unbracketed
# painting draw desynced the GX FIFO and wedged the Wii GPU every frame.
# Links nothing; reads the source tree at run time (root overridable by argv).
add_executable(test_entity_direct_colour_bracket tests/test_entity_direct_colour_bracket.c)
target_compile_definitions(test_entity_direct_colour_bracket PRIVATE
	"CW_SOURCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/source\"")
set_target_properties(test_entity_direct_colour_bracket PROPERTIES C_STANDARD 99)
add_test(NAME test_entity_direct_colour_bracket COMMAND test_entity_direct_colour_bracket)

add_executable(test_mp_pause_menu_core tests/test_mp_pause_menu_core.c)
target_include_directories(test_mp_pause_menu_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_mp_pause_menu_core PROPERTIES C_STANDARD 99)
add_test(NAME test_mp_pause_menu_core COMMAND test_mp_pause_menu_core)

# REL-11a: survival_journey_core.h is pure/header-only like mp_pause_menu_core.h;
# the test links no engine source (live adapter = REL-11-adapter-a, unproven).
add_executable(test_survival_journey_core tests/test_survival_journey_core.c)
target_include_directories(test_survival_journey_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_survival_journey_core PROPERTIES C_STANDARD 99)
add_test(NAME test_survival_journey_core COMMAND test_survival_journey_core)

# REL-16a: soak_idle_play pure timer/phase core (links nothing, fake time).
add_executable(test_soak_idle_play_core tests/test_soak_idle_play_core.c)
target_include_directories(test_soak_idle_play_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_soak_idle_play_core PROPERTIES C_STANDARD 99)
add_test(NAME test_soak_idle_play_core COMMAND test_soak_idle_play_core)

add_executable(test_mp_play_map tests/test_mp_play_map.c)
target_include_directories(test_mp_play_map PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
# Same libm gap as test_mp_play_core: atan2f via mp_play_core.h:1061 plus the
# test's own sincos/sincosf (tests/test_mp_play_map.c:726,756) fail to link on
# Linux gcc; MinGW links libm implicitly (integration-0924 local_ci run 2).
target_link_libraries(test_mp_play_map m)
set_target_properties(test_mp_play_map PROPERTIES C_STANDARD 99)
add_test(NAME test_mp_play_map COMMAND test_mp_play_map)

add_executable(test_split_actions_counters tests/test_split_actions_counters.c)
target_link_libraries(test_split_actions_counters cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_actions_counters PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_actions_counters PROPERTIES C_STANDARD 99)
add_test(NAME test_split_actions_counters COMMAND test_split_actions_counters)

add_executable(test_local_join_ready tests/test_local_join_ready.c)
target_link_libraries(test_local_join_ready cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_local_join_ready PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_local_join_ready PROPERTIES C_STANDARD 99)
add_test(NAME test_local_join_ready COMMAND test_local_join_ready)

add_executable(test_local_join_failure tests/test_local_join_failure.c)
target_link_libraries(test_local_join_failure cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_local_join_failure PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_local_join_failure PROPERTIES C_STANDARD 99)
add_test(NAME test_local_join_failure COMMAND test_local_join_failure)


# task_c75a3ed4 (2026-08-31): autopilot death recovery -- a scripted walk that
# dies (fall/drown/mob damage) must self-Respawn instead of freezing forever
# on screen_death (root cause of the mp_lan_walk_10m spawn-pocket "walker
# immobility"). Pure gstate/screen state-machine test, no gfx/world/network
# bootstrap needed.
add_executable(test_autopilot_death_recovery tests/test_autopilot_death_recovery.c)
target_link_libraries(test_autopilot_death_recovery cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_autopilot_death_recovery PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_autopilot_death_recovery PROPERTIES C_STANDARD 99)
target_link_libraries(test_autopilot_death_recovery ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_autopilot_death_recovery COMMAND test_autopilot_death_recovery)

# push-0921/B18 (review B17 H4): ap_script_route_rank is the one thing the
# script seam changes inside a SHARED production function (autopilot_probe_route,
# walked by far_walk_6m / split_anchor_6m / split_play_2m|8m / split_actions_6m
# as well as by mp_play). B17 ran two mutations past the whole suite and
# neither was caught: `return i;` unconditionally, and dropping the
# !s_script_walk guard. Both are pinned here, on a featureless flat plateau
# where every candidate is legal, so only the cost function can decide the pick.
add_executable(test_autopilot_route_rank tests/test_autopilot_route_rank.c)
target_link_libraries(test_autopilot_route_rank cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_autopilot_route_rank PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_autopilot_route_rank PROPERTIES C_STANDARD 99)
target_link_libraries(test_autopilot_route_rank ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_autopilot_route_rank COMMAND test_autopilot_route_rank)

# World management (menu queue item 4): the recursive delete
# [740c583 ISaveFormat.deleteWorldDirectory] on a nested fixture tree, the
# rename apply op (LevelName rewritten IN PLACE, TRIMMED, folder untouched
# [740c583 ISaveFormat.renameWorld]), and the exported Java trim. Scratch
# dirs are made in the build cwd and removed.
add_executable(test_world_manage_env tests/test_world_manage_env.c)
target_link_libraries(test_world_manage_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_world_manage_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_world_manage_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_world_manage_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_world_manage_env COMMAND test_world_manage_env)

# The autoload "+new" sentinel leak fix (2026-08-17): sworld_autoload_new_tick
# in isolation, no GL context and no input -- same carve-out reasoning as
# test_world_manage_env just above. Shapes (a) empty saves + no sentinel is a
# no-op, (b) a config-sourced sentinel still creates a world (capture
# convenience preserved) and latches, (b2) the announce fprintf's env-vs-config
# source detection is driven by a real getenv.
add_executable(test_autoload_sentinel_env tests/test_autoload_sentinel_env.c)
target_link_libraries(test_autoload_sentinel_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_autoload_sentinel_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_autoload_sentinel_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_autoload_sentinel_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_autoload_sentinel_env COMMAND test_autoload_sentinel_env)

# S0 audio foundation env test: playback-math goldens vs the Beta formulas
# (range/falloff/loudness/UI-quarter/pitch-unclamped), the PCM16 WAV parser,
# the StepSound break-vs-place tables (glass/sand traps), the 8-mob sound
# table, HEADLESS registry load (audio_init(false,..): registry + arena, NO
# device -- every S0 id must resolve a placeholder via the per-KEY
# fallback), and CRPC_SOUND emission through the production server paths
# (break/place/explosion/TNT-prime/egg-lay/mob-hurt-death/player-hurt).
# S1 additions: the transcode tool CORE compiled in-process (NO_MAIN +
# EXTERN_VORBIS: header-only stb_vorbis, linking the implementation already
# in audio_registry.c's TU) for identity/normalize/reject/manifest goldens,
# and the audio_registry_reload drain (scratch pack override -> reload ->
# reload back, arena reset proven byte-exact).
# WORKING_DIRECTORY = the source root: the registry loader resolves
# "assets/sound/..." exactly like the shipped game run from its install dir.
add_executable(test_audio_env tools/sound_transcode.c
	tests/test_audio_env.c)
target_link_libraries(test_audio_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
# Audio-tool flags apply ONLY to the two TUs this target compiles itself
# (sound_transcode.c + the test); the engine objects come from the pool.
target_compile_definitions(test_audio_env PRIVATE
	SOUND_TRANSCODE_NO_MAIN SOUND_TRANSCODE_EXTERN_VORBIS)
target_include_directories(test_audio_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tools
)
set_target_properties(test_audio_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_audio_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_audio_env COMMAND test_audio_env
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# ingame-import-1030 (B): the in-game importer's sound phase
# (beta_import_sound.c -> bi_sounds_import) writes the SAME bytes as the PC
# Pack Helper (owned_importer.process_sounds, strict, through the built
# sound_transcode tool). The C driver stubs the orchestrator's three progress
# hooks and carries the stb_vorbis implementation TU; the Python side builds
# the synthetic resources/ tree (ffmpeg fixtures + generated WAVs, never
# Mojang), runs both, and diffs the trees.
add_executable(test_beta_import_sound tests/test_beta_import_sound.c
	source/platform/beta_import_sound.c source/platform/pack_cache.c
	tests/beta_import_vorbis_pc.c)
target_include_directories(test_beta_import_sound PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_beta_import_sound PROPERTIES C_STANDARD 99)
target_link_libraries(test_beta_import_sound m)
if(Python3_Interpreter_FOUND)
	add_test(NAME test_beta_import_sound COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_beta_import_sound.py
		$<TARGET_FILE:test_beta_import_sound> $<TARGET_FILE:sound_transcode>)
endif()
# test_beta_import_e2e (coder A's block, above) hands this binary's --tool
# form to the Pack Helper as the transcoder, so build it with the probe.
add_dependencies(beta_import_probe test_beta_import_sound)

add_executable(test_hoe_till_sound tests/test_hoe_till_sound.c)
target_link_libraries(test_hoe_till_sound cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_hoe_till_sound PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
add_test(NAME test_hoe_till_sound COMMAND test_hoe_till_sound)

add_executable(test_itemreed_place_sound tests/test_itemreed_place_sound.c)
target_link_libraries(test_itemreed_place_sound cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_itemreed_place_sound PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
add_test(NAME test_itemreed_place_sound COMMAND test_itemreed_place_sound)

# SNOW-001: reed/cake/repeater used on a snow layer place INSIDE the snow cell
# (Beta ItemReed.onItemUse snow rule, 740c583 ItemReed.java:12-13,43-48),
# host and guest each in their own committed world through the real dispatch.
add_executable(test_itemreed_snow_place tests/test_itemreed_snow_place.c)
target_link_libraries(test_itemreed_snow_place cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_itemreed_snow_place PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_itemreed_snow_place PROPERTIES C_STANDARD 99)
add_test(NAME test_itemreed_snow_place COMMAND test_itemreed_snow_place)
set_tests_properties(test_itemreed_snow_place PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)

add_executable(test_fire_punch_fizz tests/test_fire_punch_fizz.c)
target_link_libraries(test_fire_punch_fizz cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_fire_punch_fizz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
add_test(NAME test_fire_punch_fizz COMMAND test_fire_punch_fizz)

add_executable(test_redstone_torch_burnout_fizz tests/test_redstone_torch_burnout_fizz.c)
target_link_libraries(test_redstone_torch_burnout_fizz cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_redstone_torch_burnout_fizz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
add_test(NAME test_redstone_torch_burnout_fizz COMMAND test_redstone_torch_burnout_fizz)

# MEM2 audio-region split. The audio PCM arena moved OUT of the display-list lwp
# carve into its own reserved region (mem2.h MEM2_AUDIO_REGION_BYTES). The Wii
# branch is the only place the region math runs, so what this pins is the
# ARITHMETIC (mem2_plan_regions goldens for the shipping wants, the real-arena2
# clip, the 32-byte DSP-DMA alignment of the audio base, and the property the
# whole change exists for: the carve size does not depend on the audio want) and
# the SEAM (a registry load must request ZERO bytes from mem2_alloc, proven by
# the mem2 accounting counters, with the decoded PCM landing inside the region
# the seam handed out). Also pins FAIL-SOFT on a deliberately short grant:
# fewer resident keys, the shortfall reported through audio_arena_total(), a
# reload that re-fills the SAME region in place, and no sample handed out for a
# key that could not load. WORKING_DIRECTORY = the source root: the registry
# loader resolves "assets/sound/..." exactly like test_audio_env above.
add_executable(test_mem2_audio_region tests/test_mem2_audio_region.c)
target_link_libraries(test_mem2_audio_region cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mem2_audio_region PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mem2_audio_region PROPERTIES C_STANDARD 99)
target_link_libraries(test_mem2_audio_region ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mem2_audio_region COMMAND test_mem2_audio_region
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# C2 P1 (Astra D1): the approved UI sound package and note.* load BEFORE any
# other key, so an arena that HOLDS the protected set can no longer lose it to
# a pack that overflows (an arena smaller than the protected set still can).
# Pins the golden (a roomy arena resolves the same file for every key as the
# old plain table-order walk), the protection (an arena sized for the protected
# set plus three ordinary keys keeps all thirteen whole while the tail goes
# silent, with the reject counter moving and protected_silent/short at 0),
# the pass order as reported by the CAVEWII_TESTING accessor
# audio_registry_test_scan_order (the PC-only arena-report lines are only a
# secondary check), and reversibility. WORKING_DIRECTORY = the source root.
add_executable(test_audio_registry_priority tests/test_audio_registry_priority.c)
target_link_libraries(test_audio_registry_priority cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_audio_registry_priority PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_audio_registry_priority PROPERTIES C_STANDARD 99)
target_link_libraries(test_audio_registry_priority ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_audio_registry_priority COMMAND test_audio_registry_priority
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# boot-sound-measure: the boot scan's per-phase counters (open miss/hit,
# read, convert, per-key/per-file max) on the audio_load observer record,
# with exact counts derived from an independent model of the probe order and
# a stored registry hash captured from the pre-counter loader (byte identity).
add_executable(test_audio_load_counters tests/test_audio_load_counters.c)
target_link_libraries(test_audio_load_counters cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_audio_load_counters PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_audio_load_counters PROPERTIES C_STANDARD 99)
target_link_libraries(test_audio_load_counters ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_audio_load_counters COMMAND test_audio_load_counters
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# pack-cache-0927: the opt-in audio pack cache -- cached == fresh decode,
# hit/miss/invalidate/corrupt/torn/unwritable, both producers, OFF inert.
add_executable(test_audio_pack_cache tests/test_audio_pack_cache.c)
target_link_libraries(test_audio_pack_cache cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_audio_pack_cache PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_audio_pack_cache PROPERTIES C_STANDARD 99)
target_link_libraries(test_audio_pack_cache ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_audio_pack_cache COMMAND test_audio_pack_cache
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# pack-cache-0927: the opt-in TEXTURE pack cache -- restored == fresh load
# (every slot byte, sign colour, colormaps, atlas tables), managed-manifest
# and unmanaged-source-list invalidation, corrupt/torn, the host tool's Wii
# target, bundled root. Needs a hidden GL context (PC uploads).
add_executable(test_tex_pack_cache tests/test_tex_pack_cache.c)
target_link_libraries(test_tex_pack_cache cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_tex_pack_cache PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_tex_pack_cache PROPERTIES C_STANDARD 99)
target_link_libraries(test_tex_pack_cache ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_tex_pack_cache COMMAND test_tex_pack_cache
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# pack-cache-0927: the shared GX encoders (tex_gx_conv.c) are byte-identical
# to a verbatim reference copy of the original Wii conversion code.
add_executable(test_tex_gx_conv_equiv
	tests/test_tex_gx_conv_equiv.c
	source/platform/tex_gx_conv.c
	source/platform/texture_mip.c
)
target_link_libraries(test_tex_gx_conv_equiv m)
target_include_directories(test_tex_gx_conv_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_tex_gx_conv_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_tex_gx_conv_equiv COMMAND test_tex_gx_conv_equiv)

# sound-dir-index: the boot/reload sound scan lists each directory once and
# opens only names present. Proves the listing path loads a byte-identical
# registry (and opens the same files) as the per-file probe across normal,
# missing-dir, partial-fallback, case-variant, odd and deferred-cave trees.
add_executable(test_audio_dir_index_equiv tests/test_audio_dir_index_equiv.c)
target_link_libraries(test_audio_dir_index_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_audio_dir_index_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_audio_dir_index_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_audio_dir_index_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_audio_dir_index_equiv COMMAND test_audio_dir_index_equiv
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# asyncsnd-0926 (Astra ruling 3): the opt-in async pass C of the sound
# registry + the world-entry gate. Sync oracle vs hand-stepped and real-thread
# async runs (registry hash: counts, frames, rates, arena offsets, PCM bytes;
# per-root open/read counters), publication order and pending-key skips,
# thread-creation fallback, cancel/join + stale steps, repeated pack switches,
# the progress timeout -> FAILED -> retry path, deferred-cave coexistence, the
# gate's decision table and request parking, and the async fields of the
# reload record. Fixtures generated per process.
add_executable(test_audio_async_load tests/test_audio_async_load.c)
target_link_libraries(test_audio_async_load cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_audio_async_load PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_audio_async_load PROPERTIES C_STANDARD 99)
target_link_libraries(test_audio_async_load ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_audio_async_load COMMAND test_audio_async_load
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# C2 P2 (Astra ruling 3): the opt-in deferred one-shot loader, audio_deferred.c.
# The test plays the role of audio.c and drives the loader BY HAND (no thread
# under CAVEWII_TESTING), so every cancel/steal/reload interleaving is
# deterministic: state after each transition, exactly one terminal record per
# request, scratch guard words intact, PCM byte-identical to the resident
# wavh_parse path. Fixtures are generated at run time into a per-process dir.
add_executable(test_audio_deferred tests/test_audio_deferred.c)
target_link_libraries(test_audio_deferred cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_audio_deferred PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/source/platform
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_audio_deferred PROPERTIES C_STANDARD 99)
target_link_libraries(test_audio_deferred ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_audio_deferred COMMAND test_audio_deferred
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# C2 P2 (Astra ruling 3): the same opt-in, one level up -- through the REAL
# three-pass registry scan against a scratch pack on disk. Pins what
# test_audio_deferred cannot reach: the opt-in OFF path byte-identical to the
# resident scan, the ON scan's probed (pcm == NULL) variant pool and its
# audio_registry_deferred_source paths, deferred PCM byte-identical to the
# resident decode of the same file, pack-then-default precedence, the
# AUDIO_DEFERRED_MAX_BYTES ceiling on both sides, an unreservable scratch
# falling back to a RESIDENT scan with the arena rolled back, a reload landing
# mid-load, and the unchanged one-draw variant RNG. Fixtures are generated at
# run time into a per-process dir. WORKING_DIRECTORY = the source root.
add_executable(test_audio_deferred_registry tests/test_audio_deferred_registry.c)
target_link_libraries(test_audio_deferred_registry cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
# NOTE: source/platform is deliberately NOT on the include path here (unlike
# test_audio_deferred, which does not reach pthread.h). This test includes
# game/game_state.h -> entity/entity.h -> platform/thread.h -> <pthread.h>, and
# with source/platform on the path MinGW's pthread.h picks up the PROJECT's
# source/platform/time.h for its own <time.h>, which breaks its _timespec64
# declarations. Everything here resolves fine as "platform/<header>".
target_include_directories(test_audio_deferred_registry PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_audio_deferred_registry PROPERTIES C_STANDARD 99)
target_link_libraries(test_audio_deferred_registry ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_audio_deferred_registry COMMAND test_audio_deferred_registry
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Lane C "Loading sounds..." (Astra ruling 1, c3-loading-sounds-0919): the
# AUTHORED geometry + wording of the static loading notice, pinned without a
# GPU, and the proof that the shared strip layout is pixel-for-pixel the
# pack-switch strip screen_texturepacks.c drew before the move.
add_executable(test_loading_notice tests/test_loading_notice.c)
target_link_libraries(test_loading_notice cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_loading_notice PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_loading_notice PROPERTIES C_STANDARD 99)
target_link_libraries(test_loading_notice ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_loading_notice COMMAND test_loading_notice
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# S3 background-music env test (docs/finish_line_plan.md S3): the PURE
# playRandomMusicIfReady timer state machine (audio_music_timer.h) and the
# PURE double-buffer fill/advance/underrun decisions (audio_music_ring.h),
# golden vs hand-derived expected values -- never against a live decode or
# the decode thread, the poisoned-golden rule. Also: the headless registry
# scan (audio_music_track_count() against a scratch music/ pack -- catalogs
# FILE PATHS only, proving no decode happens at scan time, the whole point
# of streaming vs the S0/S1/S2 decode-at-load model), the music.volume
# config round-trip, and the Music row now being a live, enabled options
# control (screen_options.c). WORKING_DIRECTORY = the source root, the
# test_audio_env precedent (assets/ resolves relative to it).
add_executable(test_audio_music_env tests/test_audio_music_env.c)
target_link_libraries(test_audio_music_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_audio_music_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_audio_music_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_audio_music_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_audio_music_env COMMAND test_audio_music_env
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# music-readahead-0926 (T1): the 32-half decode ring + refill low-water mark
# hands the voice byte-identical PCM to the legacy 4-half ring -- proven
# through the REAL audio_music.c worker/drain loop (CAVEWII_TESTING hooks)
# against an independent whole-file stb_vorbis decode, passthrough and 48 kHz
# resampled, plus the measured wake/burst ratio and an untouched music RNG.
# Set CW_MUSIC_EQUIV_TRACK to an owned Beta .ogg to repeat it on a real track.
add_executable(test_music_readahead_equiv tests/test_music_readahead_equiv.c)
target_link_libraries(test_music_readahead_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_music_readahead_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_music_readahead_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_music_readahead_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_music_readahead_equiv COMMAND test_music_readahead_equiv
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# G9 decode deadline promotion (T2 scheduling, opt-in with the tick boost on a
# local server): the real audio_music.c worker hands the voice byte-identical
# PCM with promotion forced off vs on (jitter + decoder-outrunning drain
# consumers, passthrough + 48 kHz, 32/8 + 4/1 rings), stays race-free under
# CPU hogs + random stop/track-change/gate flips/EOF (every segment a prefix
# of an independent decode, demoted at every quiescent point), and is gated
# on a registered local tick-boost owner (remote MP shape = no promotion).
add_executable(test_music_decode_promotion tests/test_music_decode_promotion.c)
target_link_libraries(test_music_decode_promotion cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_music_decode_promotion PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_music_decode_promotion PROPERTIES C_STANDARD 99)
target_link_libraries(test_music_decode_promotion ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_music_decode_promotion COMMAND test_music_decode_promotion
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Beta minecart placement/settle golden (the hover fix): ItemMinecart spawns at
# railY+0.85 (railY+.5 + yOffset .35) and EntityMinecart.onUpdate's rail branch
# pins posY to getPos()'s flat-tile track height railY+0.5 on the FIRST tick,
# even at zero speed -- a placed cart must not hover at spawn height. Driven
# through the real item_get->onItemPlace -> server_local_spawn_minecart -> the
# production entity pass. Also pins the isRailBlock gate (stone refused,
# powered rail accepted). [decompile 740c583]
add_executable(test_minecart_place tests/test_minecart_place.c)
target_link_libraries(test_minecart_place cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_place PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_place PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_place ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_place COMMAND test_minecart_place)

# Beta 1.7.3 powered-rail SIGN/DIRECTION goldens. test_minecart_place starts a
# RESTING cart and asserts only `speed > 0`, so it passes on the surviving
# unconditional `speed = 0.05f` launch. This pins the semantics Beta actually
# defines: magnitude gating, sign-preserving boost, wall-directed signed rest
# kicks, no fabricated launch, symmetric braking.
add_executable(test_minecart_rail_beta tests/test_minecart_rail_beta.c)
target_link_libraries(test_minecart_rail_beta cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_rail_beta PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_rail_beta PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_rail_beta ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_rail_beta COMMAND test_minecart_rail_beta)

# O06 (vehicle-smoke-1004, KANBAN Beta-parity card): minecart rail-travel
# collision goldens -- EntityMinecart.onUpdate:316 moveEntity(var35, 0, var37)
# against block boxes (end stop, ridden approach, walls/headroom, powered
# stop, slope wall). Authored before the fix; values from the decompile.
add_executable(test_minecart_rail_collision_golden tests/test_minecart_rail_collision_golden.c)
target_link_libraries(test_minecart_rail_collision_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_rail_collision_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_rail_collision_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_rail_collision_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_rail_collision_golden COMMAND test_minecart_rail_collision_golden)
# O08 (vehicle-smoke-1004, G4): player/cart separation after a production
# dismount -- RIDE_INTENT dismount, the cart nudge's exact +d/4 delivery, the
# knockback consumer, separation, remount/switch-back, stale/world/P2 gates.
add_executable(test_minecart_dismount_separation tests/test_minecart_dismount_separation.c)
target_link_libraries(test_minecart_dismount_separation cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_dismount_separation PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_dismount_separation PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_dismount_separation ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_dismount_separation COMMAND test_minecart_dismount_separation)
# G6 (vehicle-smoke-1004 O13): player death-drop scatter, Beta golden on the
# journaled Nether and Overworld death paths.
# vehicle-smoke-1004 O17: camera_ray_pick termination + equivalence
add_executable(test_camera_ray_pick tests/test_camera_ray_pick.c)
target_link_libraries(test_camera_ray_pick cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_camera_ray_pick PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_camera_ray_pick PROPERTIES C_STANDARD 99)
target_link_libraries(test_camera_ray_pick ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_camera_ray_pick COMMAND test_camera_ray_pick)

# aim-snap-1005/A01: soft-snap melee assist (playability carve-out)
add_executable(test_attack_assist tests/test_attack_assist.c)
target_link_libraries(test_attack_assist cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_attack_assist PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_attack_assist PROPERTIES C_STANDARD 99)
target_link_libraries(test_attack_assist ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_attack_assist COMMAND test_attack_assist)

# vehicle-smoke-1004 G1: minecart off-rail physics golden
add_executable(test_minecart_offrail_golden tests/test_minecart_offrail_golden.c)
target_link_libraries(test_minecart_offrail_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_offrail_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_offrail_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_offrail_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_offrail_golden COMMAND test_minecart_offrail_golden)
# vehicle-smoke-1004 O19: the vehicle rig slot (consumed rig archived, never deleted)
add_executable(test_vehicle_rig_slot tests/test_vehicle_rig_slot.c)
target_link_libraries(test_vehicle_rig_slot cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_vehicle_rig_slot PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_vehicle_rig_slot PROPERTIES C_STANDARD 99)
target_link_libraries(test_vehicle_rig_slot ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_vehicle_rig_slot COMMAND test_vehicle_rig_slot)
# vehicle-smoke-1004 G7: player-owned drops draw from the actor stream (EntityPlayer.java:305-315)
add_executable(test_player_drop_rng_golden tests/test_player_drop_rng_golden.c)
target_link_libraries(test_player_drop_rng_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_player_drop_rng_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_player_drop_rng_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_player_drop_rng_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_player_drop_rng_golden COMMAND test_player_drop_rng_golden)
# vehicle-smoke-1004 G3: minecarts are not solid to mobs (EntityMinecart.getBoundingBox null)
add_executable(test_mob_vehicle_solidity tests/test_mob_vehicle_solidity.c)
target_link_libraries(test_mob_vehicle_solidity cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mob_vehicle_solidity PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_mob_vehicle_solidity PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_vehicle_solidity ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mob_vehicle_solidity COMMAND test_mob_vehicle_solidity)
# vehicle-smoke-1004 G2: cart<->entity and cart<->cart collision (EntityMinecart :441-447, :554-612)
add_executable(test_minecart_entity_collision_golden tests/test_minecart_entity_collision_golden.c)
target_link_libraries(test_minecart_entity_collision_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_entity_collision_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_minecart_entity_collision_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_entity_collision_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_entity_collision_golden COMMAND test_minecart_entity_collision_golden)
# vehicle-smoke-1004 G5: player push decays by Beta friction, small pushes applied (EntityLiving.java:440-470)
add_executable(test_player_push_decay_golden tests/test_player_push_decay_golden.c)
target_link_libraries(test_player_push_decay_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_player_push_decay_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(test_player_push_decay_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_player_push_decay_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_player_push_decay_golden COMMAND test_player_push_decay_golden)

add_executable(test_death_scatter_golden tests/test_death_scatter_golden.c)
target_link_libraries(test_death_scatter_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_death_scatter_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_death_scatter_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_death_scatter_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_death_scatter_golden COMMAND test_death_scatter_golden)


# CR-05 (docs/audit_2026-08-20_code_review.md): detector-rail golden --
# authored FIRST from the decompile per beta-tick-golden (occupancy bit 8,
# 20-tick recheck chain, per-face power truth table, below-notify
# propagation). Full source link like test_minecart_rail_beta above.
add_executable(test_detector_rail_golden tests/test_detector_rail_golden.c)
target_link_libraries(test_detector_rail_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_detector_rail_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_detector_rail_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_detector_rail_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_detector_rail_golden COMMAND test_detector_rail_golden)

# Audit #24 (ratified): pressure-plate golden -- authored per beta-tick-golden
# from the decompile (player press-on-tick-1 + tickRate()==20 recheck cadence
# + phantom-power release, a mob pressing a STONE plate, an item pressing a
# WOOD plate but not a STONE one, an arrow correctly NOT pressing either,
# support-drop). Full source link like test_detector_rail_golden above.
add_executable(test_pressure_plate_golden tests/test_pressure_plate_golden.c)
target_link_libraries(test_pressure_plate_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_pressure_plate_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_pressure_plate_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_pressure_plate_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_pressure_plate_golden COMMAND test_pressure_plate_golden)

# ItemSign.onItemUse port (a4ff7f8's recorded follow-up): wall placement
# metadata, refusal gates, yaw->16-direction goldens, dispatch-level
# consume/GUI-open assertions.
add_executable(test_sign_placement tests/test_sign_placement.c)
target_link_libraries(test_sign_placement cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_sign_placement PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_sign_placement PROPERTIES C_STANDARD 99)
target_link_libraries(test_sign_placement ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_sign_placement COMMAND test_sign_placement)

# wave 3: the client sign-text store + its four feed paths (chunk load, edit
# echo, break, MP UpdateSign), driven through the REAL CRPC path.
add_executable(test_sign_text_store tests/test_sign_text_store.c)
target_link_libraries(test_sign_text_store cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_sign_text_store PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_sign_text_store PROPERTIES C_STANDARD 99)
target_link_libraries(test_sign_text_store ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_sign_text_store COMMAND test_sign_text_store)

# G8: sign edit and break publication follows the owning world and generation.
add_executable(test_sign_world_publication tests/test_sign_world_publication.c)
target_link_libraries(test_sign_world_publication cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_sign_world_publication PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_sign_world_publication PROPERTIES C_STANDARD 99)
target_link_libraries(test_sign_world_publication ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_sign_world_publication COMMAND test_sign_world_publication)

# G8: secondary chunk, sign and entity publication with scoped resend.
add_executable(test_a2w_publication tests/test_a2w_publication.c)
target_link_libraries(test_a2w_publication cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_a2w_publication PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_a2w_publication PROPERTIES C_STANDARD 99)
target_link_libraries(test_a2w_publication ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_a2w_publication COMMAND test_a2w_publication)
add_test(NAME test_fireball_publication COMMAND test_a2w_publication fireball)
set_tests_properties(test_fireball_publication PROPERTIES TIMEOUT 120)
add_test(NAME test_projectile_publication COMMAND test_a2w_publication projectiles)
set_tests_properties(test_projectile_publication PROPERTIES TIMEOUT 120)
add_test(NAME test_guest_respawn_recovery COMMAND test_a2w_publication respawn-recovery)
add_test(NAME test_guest_respawn_recovery_invalid COMMAND test_a2w_publication respawn-recovery-invalid)
add_test(NAME test_guest_respawn_recovery_retry COMMAND test_a2w_publication respawn-recovery-host-ow)
set_tests_properties(test_guest_respawn_recovery test_guest_respawn_recovery_invalid
	test_guest_respawn_recovery_retry PROPERTIES TIMEOUT 180)

add_test(NAME test_host_secondary_recovery_vacant COMMAND test_a2w_publication secondary-recovery-vacant)
add_test(NAME test_host_secondary_recovery_peer COMMAND test_a2w_publication secondary-recovery-peer)
add_test(NAME test_host_secondary_recovery_late COMMAND test_a2w_publication secondary-recovery-late)
add_test(NAME test_host_secondary_recovery_center COMMAND test_a2w_publication secondary-recovery-center)
set_tests_properties(test_host_secondary_recovery_vacant test_host_secondary_recovery_peer
	test_host_secondary_recovery_late test_host_secondary_recovery_center PROPERTIES TIMEOUT 180)

# wave 3: Beta's sign-text GEOMETRY golden. Links the pure builder ALONE --
# no GL/GX, no store -- which is the whole reason that file is its own TU.
add_executable(test_sign_text_quads
	source/graphics/render_sign_text.c
	tests/test_sign_text_quads.c
)
target_include_directories(test_sign_text_quads PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_sign_text_quads PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_sign_text_quads m)
add_test(NAME test_sign_text_quads COMMAND test_sign_text_quads)

# MP name-tag geometry golden. Links the pure builder ALONE -- no GL/GX, no
# entity dict -- mirroring test_sign_text_quads's own rig for exactly the
# same reason (render_name_tag.c is its own TU precisely so this can happen).
add_executable(test_name_tag_quads
	source/graphics/render_name_tag.c
	tests/test_name_tag_quads.c
)
target_include_directories(test_name_tag_quads PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_name_tag_quads PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_name_tag_quads m)
add_test(NAME test_name_tag_quads COMMAND test_name_tag_quads)

# Field defect regression (2026-08-30 maintainer live Wii-vs-PC MP session,
# PROJECT_MEMORY.md "NO NAME TAGS over remote players on the Wii"): unlike
# test_name_tag_quads above (pure geometry only, no entity dict, no draw
# calls), this links render_name_tag.c + render_name_tag_draw.c -- the ACTUAL
# entry point main.c calls -- against a recorder gfx layer (test_precip_gx_
# state.c's rig) and a synthetic dict_entity_t (test_audio_env.c's dict_
# entity_safe_get + malloc construction pattern), proving the previously-
# untested select_nearest_players/build_batch dict-iteration and dispatch
# logic actually reaches gfx_draw_quads_flt with texture_font bound, for both
# the standing and sneaking draw shapes, across single- and multi-player
# dicts. See tests/test_name_tag_draw_pass.c's header for the full
# investigation writeup.
add_executable(test_name_tag_draw_pass
	source/graphics/render_name_tag.c
	source/graphics/render_name_tag_draw.c
	tests/test_name_tag_draw_pass.c
)
target_include_directories(test_name_tag_draw_pass PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_name_tag_draw_pass PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_name_tag_draw_pass ${CMAKE_THREAD_LIBS_INIT} GLEW::GLEW OpenGL::GL m)
add_test(NAME test_name_tag_draw_pass COMMAND test_name_tag_draw_pass)

# MP visual-parity pass (2026-08-31) items 2/3/4 pure-math golden: the
# ModelBiped swing-progress/sneak pose deltas (render_biped_swing_pose.h) and
# the pickup-flight ease-in-quadratic lerp (entity_pickup_flight.h). Both are
# header-only pure math with zero GX/game_state/entity.h coupling -- mirrors
# test_head_anchor's "bind the shared production composition directly" rig.
add_executable(test_mp_visual_parity_pose
	tests/test_mp_visual_parity_pose.c
)
target_include_directories(test_mp_visual_parity_pose PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(test_mp_visual_parity_pose PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_link_libraries(test_mp_visual_parity_pose m)
add_test(NAME test_mp_visual_parity_pose COMMAND test_mp_visual_parity_pose)

# Animation-parity pass (2026-09-03) golden, part 1 (hurt-flash/death-roll):
# a re-assertion of the shared hurt-flash/death-roll formulas (combat.h) at
# the sample points the new remote-player hurt tint / death roll render now
# also depends on. Header-only pure math -- same "zero GX/game_state/
# network-layer coupling" rig as test_mp_visual_parity_pose above. Extended
# in place by later commits on this lane as items 3/4/5 land their own
# wire-decode goldens (fire/sneak/riding bit decode, armor slot mapping).
add_executable(test_mp_animation_parity_wire
	tests/test_mp_animation_parity_wire.c
)
target_include_directories(test_mp_animation_parity_wire PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
target_link_libraries(test_mp_animation_parity_wire m)
add_test(NAME test_mp_animation_parity_wire COMMAND test_mp_animation_parity_wire)

# Server-AUTHORITATIVE mount/throttle/persistence (milestone item 6). Fail-before
# runs on the mount case: the old handler's monster-only filter drops the packet.
add_executable(test_minecart_authority tests/test_minecart_authority.c)
target_link_libraries(test_minecart_authority cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_authority PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_authority PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_authority ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_authority COMMAND test_minecart_authority)

# Boat-slice: buoyancy, drag, land-crash destruction, hit-damage accumulation
# + the STRICT >40 threshold, mount/dismount via interact, and the exact
# destruction drop multiset on BOTH destruction paths.
add_executable(test_boat_env tests/test_boat_env.c)
target_link_libraries(test_boat_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_boat_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_boat_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_boat_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_boat_env COMMAND test_boat_env)

# F3/F4: focused client-side ride-intent + camera/reconciliation tests.
# Compiled with CAVEWII_AGENT_BRIDGE so the agent input hooks drive real
# entity_tick through synthetic JUMP input.
add_executable(test_minecart_client source/game/agent_input.c tests/test_minecart_client.c)
target_link_libraries(test_minecart_client cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input_agent)
target_compile_definitions(test_minecart_client PRIVATE CAVEWII_AGENT_BRIDGE)
target_include_directories(test_minecart_client PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_client PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_client ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_client COMMAND test_minecart_client)

# Rider intent -> signed axis step. Pure helper, so no world/server harness.
add_executable(test_minecart_throttle_sign tests/test_minecart_throttle_sign.c)
target_link_libraries(test_minecart_throttle_sign cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_throttle_sign PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_throttle_sign PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_throttle_sign ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_throttle_sign COMMAND test_minecart_throttle_sign)

# Beta's legYaw=1.5F hurt flail (render-only; the sole visible reaction a passive
# mob has, since Beta gives animals no flee AI at all).
add_executable(test_mob_hurt_flail tests/test_mob_hurt_flail.c)
target_link_libraries(test_mob_hurt_flail cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mob_hurt_flail PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mob_hurt_flail PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_hurt_flail ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mob_hurt_flail COMMAND test_mob_hurt_flail)

# Rail CURVE traversal: the 16-case sign table. No test in the tree ever drove
# minecart_server_tick over a meta 2..9 tile, so curve exits and the curve ->
# straight hand-off were entirely unguarded. Pins that the exit heading stored
# at entity_minecart.c:413-414 keeps sgn(speed) -- dropping that `* sgn` makes
# every NEGATIVE-speed cart exit a curve backward (the same class as the
# double-applied sgn 5fabbb5 fixed). The 8 positive rows pass without it; only
# the 8 negative rows discriminate. Also pins curve_turn's no-turn default and
# the anti-stall invariant that a curve hands the next tile a heading on that
# tile's own axis.
add_executable(test_minecart_curve_traversal tests/test_minecart_curve_traversal.c)
target_link_libraries(test_minecart_curve_traversal cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_curve_traversal PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_curve_traversal PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_curve_traversal ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_curve_traversal COMMAND test_minecart_curve_traversal)

# END-TO-END guard for the 5fabbb5 "frozen snow mobs" fix. The existing coverage
# (test_block_behavior_parity.c:467-486) calls blocks[BLOCK_SNOW]->getBoundingBox
# directly with a synthetic block_info -- it pins the BOX SHAPE but never ticks a
# mob, so it cannot see the symptom. The freeze is produced two layers away, in
# entity_intersection_threshold (entity.c:374-376), which on an already-
# intersecting box returns threshold 0 and leaves pos BIT-IDENTICAL. So the
# observable is exact: with the bug, displacement is not small, it is ZERO.
# Asserts at two altitudes -- one pinned-velocity step through the real
# entity_try_move_axis with the mob's production bbox, and 200 full production
# ticks -- plus that DEEP snow (metadata >= 3) still collides, so a future
# "always return 0" simplification fails too.
add_executable(test_mob_snow_walk_env tests/test_mob_snow_walk_env.c)
target_link_libraries(test_mob_snow_walk_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mob_snow_walk_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mob_snow_walk_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_snow_walk_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mob_snow_walk_env COMMAND test_mob_snow_walk_env)

# Field bug 2026-08-31: mob step-ascent snap. This engine's mob gravity (2.0/tick)
# cannot integrate Beta's ~7-tick jump arc, so the old auto-jump impulse 4.0 rose
# +1.8 blocks in ONE tick then bounced ~0.8 back down onto the step -- the pigman
# "stutter/snap" seen in the field. Guards the two-part fix: impulse trimmed to
# Beta's jump apex (~1.17 [740c583 EntityLiving.jump]) and the single-tick rise
# banked into e->y_size + released 60%/tick (Beta's ySize ease [740c583
# Entity.moveEntity]) so the OBSERVABLE pos eases up while the collision box
# lands immediately. Asserts max single-tick observable rise < 1.0, no overshoot
# past the step top, and that the climb still functionally succeeds.
add_executable(test_mob_step_ascent_env tests/test_mob_step_ascent_env.c)
target_link_libraries(test_mob_step_ascent_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mob_step_ascent_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mob_step_ascent_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_step_ascent_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mob_step_ascent_env COMMAND test_mob_step_ascent_env)

# next-gaps-0924/W1B: Beta 1.7.3 mob movement parity goldens -- ice friction +
# accel chain (EntityLiving.moveEntityWithHeading:479-497), ladder climb/descent
# (:499-533, isOnLadder:550-555), suffocation through the hurt window
# (onEntityUpdate:113-115, Entity.isEntityInsideOpaqueBlock:851-863). Values
# derived from the Java, run through the production tick_server.
add_executable(test_mob_move_parity_golden tests/test_mob_move_parity_golden.c)
target_link_libraries(test_mob_move_parity_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mob_move_parity_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mob_move_parity_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_move_parity_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mob_move_parity_golden COMMAND test_mob_move_parity_golden)

# next-gaps-0924/W1B-b: T1 equivalence sweep -- the corner-cell de-duplication in
# entity_monster_inside_opaque_block_at vs a naive 8-read transcription of
# Entity.isEntityInsideOpaqueBlock (Entity.java:851-863) over a checkerboard
# world, random positions, widths 0.3..4.0 and eye heights; also read count <=.
add_executable(test_mob_suffocation_equiv tests/test_mob_suffocation_equiv.c)
target_link_libraries(test_mob_suffocation_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mob_suffocation_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mob_suffocation_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_suffocation_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mob_suffocation_equiv COMMAND test_mob_suffocation_equiv)

# FH-13: end-to-end proof that server_tick_monster's chase/wander branches route
# through real A* pathfinding (not the retired mob_chase_dir beeline) -- a creeper
# navigates AROUND a wall to reach the player, and the Beta recalc-gate RNG draws
# (nextInt(20) keep-path / nextInt(100) follow) land on s->rand_src in the exact
# short-circuit order the decompile specifies (harness shape from
# test_mob_snow_walk_env.c).
add_executable(test_pathfinding_entity_env tests/test_pathfinding_entity_env.c)
target_link_libraries(test_pathfinding_entity_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_pathfinding_entity_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_pathfinding_entity_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_pathfinding_entity_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_pathfinding_entity_env COMMAND test_pathfinding_entity_env)

# mob-AI lane (task_958b34f1): the pathfinding node-arrival dead zone -- a mob
# whose width*2 arrival radius (path_follow_steer) exceeds its own melee reach
# (the wolf's 1.5F bite gate, the shared 2.0F MOB_ATTACK_REACH_SQ for a wide
# spider) got its chase heading hard-zeroed on same-tick path exhaustion instead
# of decayed (Beta's own moveForward *= 0.98F persistence), which froze it
# PERMANENTLY just outside reach. Zombie is the narrow-mob control (never
# enters the dead zone); spider and wolf are the two dead-zone mobs on the
# roster. Fixed in entity_monster.c's mob_path_follow_tail.
add_executable(test_mob_chase_deadzone_env tests/test_mob_chase_deadzone_env.c)
target_link_libraries(test_mob_chase_deadzone_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mob_chase_deadzone_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mob_chase_deadzone_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_mob_chase_deadzone_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_mob_chase_deadzone_env COMMAND test_mob_chase_deadzone_env)

# Beta 1.7.3 SLOPE parity. Two gaps, one file. (1) GEOMETRY: block_rail.c's
# calcRailMeta gives a tile shape 2 because the track continues one block UP to
# the EAST, and Beta's gravity for shape 2 pushes WEST (downhill) -- two
# independent sources agreeing that shape 2's high side is EAST. entity_minecart
# .c's slope_height labels 2/3 the other way round. The fixture builds a REAL
# two-rail ramp and lets production calcRailMeta assign the shape, so the test
# cannot silently agree with a wrong consumer. (2) GRAVITY: Beta applies
# +-0.0078125 per tick on every sloped shape [740c583 EntityMinecart.onUpdate:
# 209-246]; CaveWii applies NO slope impulse at all, so carts cannot roll
# downhill. Registered before the -UNDEBUG sweep so its asserts survive Release.
add_executable(test_minecart_slope_beta tests/test_minecart_slope_beta.c)
target_link_libraries(test_minecart_slope_beta cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_slope_beta PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_slope_beta PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_slope_beta ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_slope_beta COMMAND test_minecart_slope_beta)

# Beta 1.7.3 powered-rail POWER STATE (handoff P1(a)): bit 8 was never set by anything, two
# shape-rewrite paths destroyed it (metadata is a 4-bit field, so `& ~0xF` was `0`), and
# calcRailMeta handed powerable rails curve metas 8/9 -- which CARRY bit 0x8, faking
# POWERED_ON. Drives the REAL server notify cascade (torch placement -> rail refresh ->
# chain walk), pinning the direct set/clear, the preservation, the curve clamp, the
# detector's single sprite, and Beta's depth-8 chain (9 lit per source, 10th dark).
add_executable(test_powered_rail_power tests/test_powered_rail_power.c)
target_link_libraries(test_powered_rail_power cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_powered_rail_power PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_powered_rail_power PROPERTIES C_STANDARD 99)
target_link_libraries(test_powered_rail_power ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_powered_rail_power COMMAND test_powered_rail_power)

# Watchdog PHASE instrument guard. The 2026-07-27 real-Wii hang was read off the
# on-screen cell bars while two rows lied: watchdog_phase_worker had ZERO call sites
# (permanently 0, decoded as "mesher idle") and WDM_SCREEN_UPDATE was never stamped
# (phase 6 covered world_build_chunks AND the whole screen update). Neither was
# catchable on PC because that branch discarded phases. Asserts the PC branch stores +
# reports every phase, that every wd_phase_main / wd_phase_worker enumerator has a call
# site under source/ (a tree scan -- the zero-call-site defect class itself), that
# CLIENT_TICK / BUILD_CHUNKS / SCREEN_UPDATE decode to distinct cell counts, and that
# the REAL mesher worker reports WDW_RECV parked and WDW_BUILD during a production
# chunk build. WORKING_DIRECTORY = the source root: the call-site scan walks "source/".
add_executable(test_watchdog_phase tests/test_watchdog_phase.c)
target_link_libraries(test_watchdog_phase cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_watchdog_phase PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_watchdog_phase PROPERTIES C_STANDARD 99)
target_link_libraries(test_watchdog_phase ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_watchdog_phase COMMAND test_watchdog_phase
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_stripe_wait_breadcrumb
	tests/test_stripe_wait_breadcrumb.c source/platform/thread.c source/platform/time.c)
target_compile_definitions(test_stripe_wait_breadcrumb PRIVATE PLATFORM_PC)
target_include_directories(test_stripe_wait_breadcrumb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_stripe_wait_breadcrumb ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_stripe_wait_breadcrumb COMMAND test_stripe_wait_breadcrumb)

add_executable(test_g9_probe tests/test_g9_probe.c source/platform/thread.c source/platform/time.c)
target_compile_definitions(test_g9_probe PRIVATE PLATFORM_PC)
target_include_directories(test_g9_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_g9_probe ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME test_g9_probe COMMAND test_g9_probe)
add_executable(test_g9_priority_lease tests/test_g9_priority_lease.c)
target_link_libraries(test_g9_priority_lease cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_g9_priority_lease PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_g9_priority_lease PROPERTIES C_STANDARD 99)
target_link_options(test_g9_priority_lease PRIVATE -Wl,--wrap=mem2_alloc)
add_test(NAME test_g9_priority_lease COMMAND test_g9_priority_lease)
# G9 R20 Q3 worst-tick attribution probe (debug.g9_worst_tick_int)
add_executable(test_g9_worst_tick tests/test_g9_worst_tick.c)
target_link_libraries(test_g9_worst_tick cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_g9_worst_tick PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_g9_worst_tick PROPERTIES C_STANDARD 99)
add_test(NAME test_g9_worst_tick COMMAND test_g9_worst_tick)
add_executable(test_g9_main_handoff tests/test_g9_main_handoff.c)
target_link_libraries(test_g9_main_handoff cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_g9_main_handoff PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_g9_main_handoff PROPERTIES C_STANDARD 99)
add_test(NAME test_g9_main_handoff COMMAND test_g9_main_handoff
	"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/music_readahead/stream_stereo_12s.ogg")
set_tests_properties(test_g9_main_handoff PROPERTIES TIMEOUT 30)
# R18 S1: finite native audio backend, named SDK boundary doubles only.
# The SDK headers supply declarations; no PPC implementation is host-linked.
find_path(_cw_audio_sdk_headers aesndlib.h HINTS
	"$ENV{DEVKITPRO}/libogc/include"
	"C:/msys64/opt/devkitpro/libogc/include")
if(_cw_audio_sdk_headers)
	add_executable(test_g9_audio_bound tests/test_g9_audio_bound.c)
	target_compile_definitions(test_g9_audio_bound PRIVATE PLATFORM_PC)
	target_include_directories(test_g9_audio_bound PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/source)
	target_include_directories(test_g9_audio_bound SYSTEM PRIVATE ${_cw_audio_sdk_headers})
	set_target_properties(test_g9_audio_bound PROPERTIES C_STANDARD 99)
	add_test(NAME test_g9_audio_bound COMMAND test_g9_audio_bound)
	set_tests_properties(test_g9_audio_bound PROPERTIES TIMEOUT 10)
endif()
# R15 Step 1: HOST-only replay + unchanged native lease behind SDK dispatch doubles.
if(WIN32) # QueryThreadCycleTime/GetThreadTimes and process affinity adapter.
add_executable(test_g9_starvation_diag tests/test_g9_starvation_diag.c
	tools/wii_stage_server/source/sha256.c)
target_link_libraries(test_g9_starvation_diag cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_g9_starvation_diag PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_stage_server/source)
set_target_properties(test_g9_starvation_diag PROPERTIES C_STANDARD 99)
foreach(_cw_diag_wrap lease_begin lease_end lease_checkpoint lease_pops
	lease_active_owner lease_active lease_defer handoff_enabled handoff_dispatch)
	target_link_options(test_g9_starvation_diag PRIVATE -Wl,--wrap=thread_g9_${_cw_diag_wrap})
endforeach()
foreach(_cw_diag_wrap g9_capture_pop g9_capture_drain_final g9_capture_restore_return
	lighting_flood_resume_light)
	target_link_options(test_g9_starvation_diag PRIVATE -Wl,--wrap=${_cw_diag_wrap})
endforeach()
add_test(NAME test_g9_starvation_diag COMMAND test_g9_starvation_diag)
set_tests_properties(test_g9_starvation_diag PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
# G9 tick-wide boost (T2 opt-in): same adapter, knob OFF RED / ON GREEN.
add_test(NAME test_g9_tick_boost COMMAND test_g9_starvation_diag --tick-boost)
set_tests_properties(test_g9_tick_boost PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endif()
if(Python3_Interpreter_FOUND)
	add_test(NAME test_g9_cpu_evidence COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_g9_cpu_evidence.py $<TARGET_FILE:test_g9_probe>)
endif()

# MEM1 red-zone guard on struct chunk. The 2026-07-27 real-Wii hang was a spin over a
# corrupted intrusive-list chain whose nodes are the LAST members of struct chunk -- a plain
# malloc MEM1 block that mem2.c's red zone structurally cannot see, which is why the canary
# read 0 and named nothing. chunk.c now lays the same red zone after the struct; this pins the
# three properties that make the diagnostic worth anything: no false positive on a healthy
# alloc/destroy cycle, a FIRE on a one-byte overrun immediately past the struct, and a reported
# offset that is the real distance into the guard (the writer's fingerprint). NARROW target on
# purpose -- the test supplies its own `gstate` plus stubs for chunk.c's render/mesh/neighbour
# callees, so it must NOT link ${_cavex_srcs} (duplicate symbols). mem2.c comes along because
# chunk_alloc puts the ~10KB block buffer in MEM2.
add_executable(test_chunk_guard
	source/chunk.c
	source/platform/mem2.c
	source/platform/displaylist_scene_hash.c
	tests/test_chunk_guard.c
)
target_compile_definitions(test_chunk_guard PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_chunk_guard PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_chunk_guard PROPERTIES C_STANDARD 99)
target_link_libraries(test_chunk_guard m)
add_test(NAME test_chunk_guard COMMAND test_chunk_guard)

add_executable(test_server_tick_pacing
	source/platform/time.c
	tests/test_server_tick_pacing.c
)
target_link_libraries(test_server_tick_pacing m)
target_compile_definitions(test_server_tick_pacing PRIVATE PLATFORM_PC)
target_include_directories(test_server_tick_pacing PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
)
add_test(NAME test_server_tick_pacing COMMAND test_server_tick_pacing)
# REL-10-tests (R10): MAIN pacing eligibility over the whole local server tick.
# Production thread.c checkpoint + the existing server tick boundary calls;
# time_get is wrapped to a fake clock and MAIN's usleep is recorded, not slept.
# Controlled failure: "REL-10-tests RED:" + exit 1; harness faults exit 2.
add_executable(test_main_tick_pacing tests/test_main_tick_pacing.c
	source/platform/thread.c source/platform/time.c)
target_compile_definitions(test_main_tick_pacing PRIVATE PLATFORM_PC)
target_include_directories(test_main_tick_pacing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_main_tick_pacing ${CMAKE_THREAD_LIBS_INIT})
target_link_options(test_main_tick_pacing PRIVATE -Wl,--wrap=time_get -Wl,--wrap=usleep)
add_test(NAME test_main_tick_pacing COMMAND test_main_tick_pacing)
set_tests_properties(test_main_tick_pacing PROPERTIES TIMEOUT 60)
# REL-10-tests: pacing off/on (preset 1 and 2) byte-identical production
# simulation (test_g9_save_io_equiv stress session + the real split admission
# with two LIVE worlds, one child per arm); each paced arm must record
# deterministic light-drain-window donations; planted session/world/entity/
# body RNG-draw and edit-order mutants that the comparator must detect.
add_executable(test_main_tick_pacing_equiv tests/test_main_tick_pacing_equiv.c)
target_link_libraries(test_main_tick_pacing_equiv cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_main_tick_pacing_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_main_tick_pacing_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_main_tick_pacing_equiv COMMAND test_main_tick_pacing_equiv)
set_tests_properties(test_main_tick_pacing_equiv PROPERTIES TIMEOUT 600)

# Beta 1.7.3 wolf slice goldens. Full production source linkage keeps each
# focused assertion on the exact runtime implementation rather than a test copy.
foreach(_wolf_test IN ITEMS
	test_wolf_tame
	test_wolf_anger_pack
	test_wolf_attack
	test_wolf_follow_sit
	test_wolf_persistence
	test_wolf_spawn_biome
	test_wolf_tail_sound
	test_wolf_interest_hold)
	add_executable(${_wolf_test} tests/${_wolf_test}.c)
	target_link_libraries(${_wolf_test} cavex_test_core cavex_test_world
		cavex_test_tickq cavex_test_input)
	target_include_directories(${_wolf_test} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/source
		${CMAKE_CURRENT_SOURCE_DIR}/third_party
		${CMAKE_CURRENT_SOURCE_DIR}/tests)
	set_target_properties(${_wolf_test} PROPERTIES C_STANDARD 99)
	target_link_libraries(${_wolf_test}
		${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
	add_test(NAME ${_wolf_test} COMMAND ${_wolf_test})
endforeach()

# Beta 1.7.3 precipitation render (render-slice, RENDER_DISPATCH.md):
# EntityRenderer.renderRainSnow / addRainParticles [740c583].
add_executable(test_precip_render_env tests/test_precip_render_env.c)
target_link_libraries(test_precip_render_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_precip_render_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_precip_render_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_precip_render_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_precip_render_env COMMAND test_precip_render_env)

# Online lightning visuals (Packet71 type 1): RenderLightningBolt geometry
# count/shape, EntityLightningBolt render lifecycle and the World.func_4079_a
# sky-flash curve, all against hand-derived b1.7.3 values.
add_executable(test_lightning_render tests/test_lightning_render.c)
target_link_libraries(test_lightning_render cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_lightning_render PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_lightning_render PROPERTIES C_STANDARD 99)
target_link_libraries(test_lightning_render ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_lightning_render COMMAND test_lightning_render)

# render_rain_snow's GX state contract (task_b126e4e2): the immediate-mode
# quads MUST be bracketed by gfx_lighting(false)/(true) -- on Wii that pair is
# the CLR0 INDEX8<->DIRECT vertex-descriptor switch, and skipping it desyncs
# the GX FIFO (GP wedge, one 500ms drawdone abort per frame, the storm-load
# world-clock freeze). Links render_weather.c against a recorder gfx layer
# (NOT ${_cavex_srcs} -- the real pc/gfx.c would need a live GL context).
add_executable(test_precip_gx_state
	tests/test_precip_gx_state.c
	source/graphics/render_weather.c
	source/light_brightness.c
	source/worldgen/java_random.c)
target_compile_definitions(test_precip_gx_state PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_precip_gx_state PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_precip_gx_state PROPERTIES C_STANDARD 99)
target_link_libraries(test_precip_gx_state ${CMAKE_THREAD_LIBS_INIT} GLEW::GLEW OpenGL::GL m)
add_test(NAME test_precip_gx_state COMMAND test_precip_gx_state)

# C40: record the actual matrix used by a rain quad and its restoration.
add_executable(test_weather_force_render
	tests/test_weather_force_render.c
	source/graphics/render_weather.c
	source/light_brightness.c
	source/worldgen/java_random.c)
target_compile_definitions(test_weather_force_render PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_weather_force_render PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_weather_force_render PROPERTIES C_STANDARD 99)
target_link_libraries(test_weather_force_render ${CMAKE_THREAD_LIBS_INIT} GLEW::GLEW OpenGL::GL m)
add_test(NAME test_weather_force_render COMMAND test_weather_force_render)

add_executable(test_weather_force_end_to_end tests/test_weather_force_end_to_end.c)
target_link_libraries(test_weather_force_end_to_end cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_weather_force_end_to_end PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_weather_force_end_to_end PROPERTIES C_STANDARD 99)
target_link_libraries(test_weather_force_end_to_end ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_weather_force_end_to_end COMMAND test_weather_force_end_to_end)

# gutil_clouds' GX state contract (followups lane, 2026-09-02): the immediate-
# mode cloud quads MUST be bracketed by gfx_lighting(false)/(true), same class
# as test_precip_gx_state above and the gutil_sky_box fix (95159f6) in this
# SAME source file -- gutil_clouds was the sibling function that fix's commit
# message explicitly flagged as carrying the identical unrestored-lighting
# pattern but scoped OUT pending its own trace. Links gfx_util.c against a
# recorder gfx layer (NOT ${_cavex_srcs} -- no live GL context here).
add_executable(test_clouds_gx_state
	tests/test_clouds_gx_state.c
	source/graphics/gfx_util.c
	source/platform/time.c)
target_compile_definitions(test_clouds_gx_state PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_clouds_gx_state PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_clouds_gx_state PROPERTIES C_STANDARD 99)
target_link_libraries(test_clouds_gx_state ${CMAKE_THREAD_LIBS_INIT} GLEW::GLEW OpenGL::GL m)
add_test(NAME test_clouds_gx_state COMMAND test_clouds_gx_state)

# dict_server_chunks key-construction / iterate-then-get investigation
# (followups lane, 2026-09-02): server-local lane reported get() NULL for a
# key it() had just enumerated. Header-only -- DICT_DEF2 (m-lib) generates
# the whole dict_server_chunks_* API as static inline functions at the point
# network/server_world.h expands it, so this links NO server_world.c/
# server_local.c body at all, genuinely isolated from the 16k-line TUs the
# original report came from.
add_executable(test_dict_server_chunks_lookup
	tests/test_dict_server_chunks_lookup.c)
target_compile_definitions(test_dict_server_chunks_lookup PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_dict_server_chunks_lookup PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_dict_server_chunks_lookup PROPERTIES C_STANDARD 99)
target_link_libraries(test_dict_server_chunks_lookup ${CMAKE_THREAD_LIBS_INIT} m)
add_test(NAME test_dict_server_chunks_lookup COMMAND test_dict_server_chunks_lookup)

# Beta 1.7.3 storage + furnace minecart goldens (minecart-slice,
# MINECART_DISPATCH.md): EntityMinecart.attackEntityFrom/interact/onUpdate's
# type-1/2 branches, ItemMinecart's type mapping, and the entity-archive
# Type/PushX/PushZ/Fuel/Items persistence [740c583].
add_executable(test_minecart_types_env tests/test_minecart_types_env.c)
target_link_libraries(test_minecart_types_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_minecart_types_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_minecart_types_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_minecart_types_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_minecart_types_env COMMAND test_minecart_types_env)

# Beta 1.7.3 fishing rod/hook goldens (fishing-slice, FISHING_DISPATCH.md):
# EntityFish.catchFish()'s durability-cost table (incl. the inGround
# override), the rain-gated bite chance/duration, the toward-angler pull
# physics, the two-draw splash-pitch order, and the real cast/reel dispatch
# driven over a live superflat server_world [740c583].
add_executable(test_fishing_env tests/test_fishing_env.c)
target_link_libraries(test_fishing_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_fishing_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_fishing_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_fishing_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_fishing_env COMMAND test_fishing_env)

# critique-1028 eat: right-click with NO block target uses the held item (Beta Minecraft.clickMouse
# :834-838 -> sendUseItem): food eats instantly, snowball/bow fire, block target unchanged, and the
# online Packet15 (-1,-1,-1,255) bytes.
add_executable(test_use_item_no_target tests/test_use_item_no_target.c source/network/mp_codec.c)
target_link_libraries(test_use_item_no_target cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_use_item_no_target PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_use_item_no_target PROPERTIES C_STANDARD 99)
target_link_libraries(test_use_item_no_target ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_use_item_no_target COMMAND test_use_item_no_target)

# BUCKET-WATER-OFFLINE: a bucket used with NO ordinary block target runs Beta
# ItemBucket.onItemRightClick's own liquid ray in the local path (pick up sources only,
# place at the hit face, nether water evaporates, creative keeps the bucket).
add_executable(test_bucket_liquid_local tests/test_bucket_liquid_local.c)
target_link_libraries(test_bucket_liquid_local cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_bucket_liquid_local PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_bucket_liquid_local PROPERTIES C_STANDARD 99)
target_link_libraries(test_bucket_liquid_local ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_bucket_liquid_local COMMAND test_bucket_liquid_local)

# Beta 1.7.3 painting slice (painting-slice, gap audit #9, PAINTING_DISPATCH.md):
# EnumArt's 25-entry table, EntityPainting.setDirection/onValidSurface, the
# placement-time art selection RNG, direction/facing mapping, and the
# invalid-surface drop [740c583].
add_executable(test_painting_env tests/test_painting_env.c)
target_link_libraries(test_painting_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_painting_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_painting_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_painting_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_painting_env COMMAND test_painting_env)

# Batch I (audit register worldgen-saves #1, the largest confirmed parity
# gap): Beta 1.7.3 falling sand/gravel. Pure archive-layer round trip for
# ARCHIVED_ENTITY_FALLING_SAND's single "Tile" tag (test_wolf_persistence.c's
# shape).
add_executable(test_falling_sand_archive tests/test_falling_sand_archive.c)
target_link_libraries(test_falling_sand_archive cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_falling_sand_archive PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_falling_sand_archive PROPERTIES C_STANDARD 99)
target_link_libraries(test_falling_sand_archive ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_falling_sand_archive COMMAND test_falling_sand_archive)

# The Giant (Beta EntityGiantZombie, EntityList "Giant"/53) -- def-table
# goldens (hp 200, 3.6x10.8 box, speed 0.5, no drops), melee 400, the
# exhaustive never-spawns-naturally invariant over every picker roll, the
# spawner "Giant" mapping (worldgen dungeon table untouched), and the
# "Giant" archive round-trip.
add_executable(test_giant tests/test_giant.c)
target_link_libraries(test_giant cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_giant PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_giant PROPERTIES C_STANDARD 99)
target_link_libraries(test_giant ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_giant COMMAND test_giant)

# Full end-to-end env golden: BlockSand.onBlockAdded/onNeighborBlockChange ->
# tickRate()==3 scheduled tick -> tryToFall -> EntityFallingSand (motion,
# origin-clear, landing placement, cannot-place item drop, 100-tick void
# give-up), gravel parity, unsupported-column cascade order, water/lava
# fall-through, solid-ground negative control, and the full NBT save/load
# round trip of a mid-air faller (tests/test_creeper_charged_explosion.c's
# and tests/test_tnt_primed_env.c's harness shape).
add_executable(test_falling_sand_golden tests/test_falling_sand_golden.c)
target_link_libraries(test_falling_sand_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_falling_sand_golden PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_falling_sand_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_falling_sand_golden ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_falling_sand_golden COMMAND test_falling_sand_golden)

# Beta 1.7.3 note block + jukebox goldens (noteblock/jukebox-slice): changePitch
# cycle, the instrument-by-material table + live variety, covered-block silence,
# the redstone rising-edge trigger (power held = one note), the pitch formula
# spot values, jukebox insert/eject (exact offsets + world event 1005), break
# ejection, and real save/evict/reload round trips INCLUDING the D12
# contents-only-edit shape for notes. See docs/briefs/NOTEBLOCK_JUKEBOX_BRIEF.md.
add_executable(test_noteblock_jukebox_env tests/test_noteblock_jukebox_env.c)
target_link_libraries(test_noteblock_jukebox_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_noteblock_jukebox_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_noteblock_jukebox_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_noteblock_jukebox_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_noteblock_jukebox_env COMMAND test_noteblock_jukebox_env)

# GB-06 (docs/audit_2026-08-14_game_breakage.md): the server-side entity-
# interaction authority gate -- Beta NetServerHandler.func_6006_a's
# canEntityBeSeen + distSq<36.0 conjunction, applied to SRPC_ENTITY_ATTACK,
# SRPC_ENTITY_USE, and the ride-intent mount leg. Proves reject == ZERO
# mutation (health/wear/anger/RNG/knockback/occupancy) through walls, at the
# strict 36.0 boundary, and for stale ids; plus the gate-before-death-flop
# ordering and per-branch positive controls.
add_executable(test_entity_interaction_authority tests/test_entity_interaction_authority.c)
target_link_libraries(test_entity_interaction_authority cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_entity_interaction_authority PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_entity_interaction_authority PROPERTIES C_STANDARD 99)
target_link_libraries(test_entity_interaction_authority ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_entity_interaction_authority COMMAND test_entity_interaction_authority)

# Server-thread clean-quit lifecycle (amahpour-shared shutdown race): the FIRST
# test to drive the real threaded entry (server_local_start/shutdown). Enforces
# join-on-quit by construction -- a while(1) regression hangs the join and reds
# by timeout. Pre-start no-op, idempotence, and restartability legs included.
add_executable(test_server_shutdown_env tests/test_server_shutdown_env.c)
target_link_libraries(test_server_shutdown_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_server_shutdown_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_server_shutdown_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_server_shutdown_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_server_shutdown_env COMMAND test_server_shutdown_env)

# MP-S1 P0 transport/codec/world-stream lifecycle. The full source snapshot is
# required for the real clin mailbox/chunk-pool consumer and worldgen climate.
add_executable(test_mp_s0_env source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_mp_s0_env.c
)
target_link_libraries(test_mp_s0_env cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mp_s0_env PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mp_s0_env PROPERTIES C_STANDARD 99)
target_link_libraries(test_mp_s0_env ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_mp_s0_env ws2_32)
endif()
add_test(NAME test_mp_s0_env COMMAND test_mp_s0_env)

# remote-mob-fire-0924/R1: Beta Packet40 watcher byte 0 drives remote mob fire.
add_executable(test_remote_mob_fire source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_remote_mob_fire.c)
target_link_libraries(test_remote_mob_fire cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_remote_mob_fire PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_remote_mob_fire PROPERTIES C_STANDARD 99)
if(WIN32)
	target_link_libraries(test_remote_mob_fire ws2_32)
endif()
add_test(NAME test_remote_mob_fire COMMAND test_remote_mob_fire)

# next-gaps-0924/G8b: Beta's red overlay lasts the whole deathTime flop
# (RenderLiving.java:67-75) -- MP Packet38 2/3 through the real codec and
# flush_entity_status, plus the SP dying CRPC_ENTITY_MOVE via clin.
add_executable(test_dying_red_tint source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_dying_red_tint.c)
target_link_libraries(test_dying_red_tint cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_dying_red_tint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_dying_red_tint PROPERTIES C_STANDARD 99)
if(WIN32)
	target_link_libraries(test_dying_red_tint ws2_32)
endif()
add_test(NAME test_dying_red_tint COMMAND test_dying_red_tint)
# mp-velocity-0926 (astra-knockback-0926 item 3): remote Packet28 for the local player.
add_executable(test_mp_local_velocity source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_mp_local_velocity.c)
target_link_libraries(test_mp_local_velocity cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_mp_local_velocity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_mp_local_velocity PROPERTIES C_STANDARD 99)
if(WIN32)
	target_link_libraries(test_mp_local_velocity ws2_32)
endif()
add_test(NAME test_mp_local_velocity COMMAND test_mp_local_velocity)
# b1stamp-remote-0926: remote-origin RPCs carry the remote session identity.
add_executable(test_b1_remote_stamp source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_b1_remote_stamp.c)
target_link_libraries(test_b1_remote_stamp cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_b1_remote_stamp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_options(test_b1_remote_stamp PRIVATE -Wl,--wrap=clin_process_direct)
set_target_properties(test_b1_remote_stamp PROPERTIES C_STANDARD 99)
if(WIN32)
	target_link_libraries(test_b1_remote_stamp ws2_32)
endif()
add_test(NAME test_b1_remote_stamp COMMAND test_b1_remote_stamp)

# mp-effects (critique-1028 risks #4-#6, #9): Packet54 block-event dispatch
# (note / piston), Packet61 aux effects, own-id 0x26/0x28 and the Packet16
# resend, fixture bytes -> process_event -> a clin_process_direct spy.
add_executable(test_mp_effects source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_mp_effects.c)
target_link_libraries(test_mp_effects cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_mp_effects PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_options(test_mp_effects PRIVATE -Wl,--wrap=clin_process_direct)
set_target_properties(test_mp_effects PROPERTIES C_STANDARD 99)
if(WIN32)
	target_link_libraries(test_mp_effects ws2_32)
endif()
add_test(NAME test_mp_effects COMMAND test_mp_effects)

# P3 MP-MISC (critique-1028): 0x47 lightning sounds, 0xC8 -> achievements
# seam, online sign editing (place echo -> editor -> one Packet130), plus
# malformed/oversized input per id. Same harness as test_mp_effects.
add_executable(test_mp_misc source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_mp_misc.c)
target_link_libraries(test_mp_misc cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_mp_misc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_options(test_mp_misc PRIVATE -Wl,--wrap=clin_process_direct -Wl,--wrap=game_event_emit)
set_target_properties(test_mp_misc PROPERTIES C_STANDARD 99)
if(WIN32)
	target_link_libraries(test_mp_misc ws2_32)
endif()
add_test(NAME test_mp_misc COMMAND test_mp_misc)

# M3 remote loading-screen readiness (mp-split-0912/18): the safe-core +
# announced-obligation gate and the census that feeds it. Same MP source
# snapshot as test_mp_s0_env above (server_remote.c owns the census and pulls
# the rest of the provider in); screen_load_world.c, which owns the gate,
# comes from cavex_test_core.
add_executable(test_load_world_ready source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_load_world_ready.c
)
target_link_libraries(test_load_world_ready cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_load_world_ready PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_load_world_ready PROPERTIES C_STANDARD 99)
target_link_libraries(test_load_world_ready ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_load_world_ready ws2_32)
endif()
add_test(NAME test_load_world_ready COMMAND test_load_world_ready)

# Packet51/52/53 ordering regressions in the online client's chunk retention
# (2026-09-10 multiplayer audit: partial-replaces-full, stale edit over a newer
# snapshot, deferred-log overflow). Includes the S0 suite's harness by source.
add_executable(test_mp_chunk_order source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_mp_chunk_order.c
)
target_link_libraries(test_mp_chunk_order cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mp_chunk_order PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mp_chunk_order PROPERTIES C_STANDARD 99)
target_link_libraries(test_mp_chunk_order ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_mp_chunk_order ws2_32)
endif()
add_test(NAME test_mp_chunk_order COMMAND test_mp_chunk_order)

# IN-008 (integ-1011): replay the recorded real-Beta walk stream
# (tests/fixtures/mp_walk/stream.bin) through the production remote transport
# at the Wii admission radius 4; the session must stay alive (cause:
# tmp/jobs/loop/IN-007-cause.md, Packet51 body chain exhausted). Same source
# set + harness-by-include pattern as test_mp_chunk_order above.
add_executable(test_mp_walk_session source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_mp_walk_session.c
)
target_link_libraries(test_mp_walk_session cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mp_walk_session PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mp_walk_session PROPERTIES C_STANDARD 99)
target_link_libraries(test_mp_walk_session ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_mp_walk_session ws2_32)
endif()
add_test(NAME test_mp_walk_session COMMAND test_mp_walk_session
	"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mp_walk/stream.bin")
# REL-MP-FIX-tests: the same binary on a fresh-terrain real-Beta stream whose
# Packet52/53 flood exhausts the deferred-edit log on chained backed columns
# (tmp/jobs/loop/REL-MP-DIAG/DIAG.md), and on its no-fresh-terrain control.
# Both must keep the session in PLAY; no WILL_FAIL.
add_test(NAME test_mp_walk_session_fresh COMMAND test_mp_walk_session
	"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mp_walk_fresh/stream.bin")
add_test(NAME test_mp_walk_session_fresh_control COMMAND test_mp_walk_session
	"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mp_walk_fresh/control.bin")
# REL-MP-JOINSPEED: the same binary on a recorded join against a FRESH vanilla
# Beta 1.7.3 world (tests/fixtures/mp_join_vanilla/README.md). Every replay
# above also checks the lossless-equivalence oracle and the compaction bounds;
# this stream is the one whose pre-body edit flood used to cost ~2,000
# compactions (one per incoming edit) and a 40 s Wii join.
add_test(NAME test_mp_walk_session_vanilla_join COMMAND test_mp_walk_session
	"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mp_join_vanilla/stream.bin")

# W1 (mp-split-0912): MP admission radius + demote-on-exit / promote-on-entry
# with a lossless re-encode (docs/mp_admission_budget_design_2026-09-13.md
# sections 2.1-2.3, 2.5). Same source set + harness-by-include pattern as
# test_mp_chunk_order above.
add_executable(test_mp_admission source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_mp_admission.c
)
target_link_libraries(test_mp_admission cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mp_admission PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mp_admission PROPERTIES C_STANDARD 99)
target_link_libraries(test_mp_admission ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_mp_admission ws2_32)
endif()
add_test(NAME test_mp_admission COMMAND test_mp_admission)

# IN-009b: byte-exact fold witnesses for retained remote columns -- every
# Packet51 region applies to a held column in arrival order (client CFR
# NetClientHandler.handleMapChunk -> World/Chunk.setChunkData), compared over
# all 81920 decoded bytes against an in-test sequential CFR-region reference.
# Same source set + harness-by-include pattern as test_mp_chunk_order above.
add_executable(test_mp_fold_equiv source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_mp_fold_equiv.c
)
target_link_libraries(test_mp_fold_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mp_fold_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mp_fold_equiv PROPERTIES C_STANDARD 99)
target_link_libraries(test_mp_fold_equiv ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_mp_fold_equiv ws2_32)
endif()
add_test(NAME test_mp_fold_equiv COMMAND test_mp_fold_equiv
	"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mp_walk/stream.bin")

# IN-009e: the remote terminal latch records the ORIGINAL failure where it
# happens -- a socket write failure in PLAY is latched by the write thread as
# mp_end 7 with its phase and exact rx_bytes, not as the receive EOF its own
# teardown causes. Same source set + harness-by-include pattern as
# test_mp_chunk_order above.
add_executable(test_mp_end_reason source/network/mp_codec.c
	source/network/mp_session.c
	source/network/server_remote.c
	source/network/server_remote_transport.c
	source/network/server_remote_protocol.c
	source/network/server_remote_entities.c
	source/network/server_remote_chunks.c
	source/platform/net_socket.c
	tests/test_mp_end_reason.c
)
target_link_libraries(test_mp_end_reason cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mp_end_reason PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mp_end_reason PROPERTIES C_STANDARD 99)
target_link_libraries(test_mp_end_reason ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_mp_end_reason ws2_32)
endif()
add_test(NAME test_mp_end_reason COMMAND test_mp_end_reason)

# BUG-SPAWN: a fresh seed-12345 world (perf_stress load setup) never buries the player at
# spawn, before or after the 3x3 spawn neighbourhood populates (Beta populates the spawn
# area before preparePlayerToSpawn). Seed 42 is the control.
add_executable(test_spawn_safety tests/test_spawn_safety.c)
target_link_libraries(test_spawn_safety cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_spawn_safety PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_spawn_safety PROPERTIES C_STANDARD 99)
add_test(NAME test_spawn_safety COMMAND test_spawn_safety)
set_tests_properties(test_spawn_safety PROPERTIES TIMEOUT 240)
# REL-03-tests (R3): new-world spawn vs the ORIGINAL Beta 1.7.3 spawn search + first join
# on identical inputs (terrain seed AND the pinned World.rand spawn RNG), seeds
# 12345/42/0/-1/4242. Fixtures: tests/oracle/spawn/fixtures.json from capture_spawn.py
# (original client jar). RED (exit 1) until REL-03 lands; exit 2 = setup failure.
add_executable(test_spawn_oracle tests/test_spawn_oracle.c)
target_link_libraries(test_spawn_oracle cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_spawn_oracle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_spawn_oracle PROPERTIES C_STANDARD 99)
add_test(NAME test_spawn_oracle COMMAND test_spawn_oracle
	${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/spawn/fixtures.json)
set_tests_properties(test_spawn_oracle PROPERTIES TIMEOUT 600)
# R3 sweep: test_spawn_oracle over 20 seeds (small, negative, 32-bit edges, Long.MIN/MAX)
# + the loaded body box never strictly intersects a block collision box. Fixtures:
# tests/oracle/spawn/sweep_fixtures.json from capture_spawn_sweep.py (original client jar).
add_executable(test_spawn_oracle_sweep tests/test_spawn_oracle_sweep.c)
target_link_libraries(test_spawn_oracle_sweep cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_spawn_oracle_sweep PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_spawn_oracle_sweep PROPERTIES C_STANDARD 99)
add_test(NAME test_spawn_oracle_sweep COMMAND test_spawn_oracle_sweep
	${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/spawn/sweep_fixtures.json)
set_tests_properties(test_spawn_oracle_sweep PROPERTIES TIMEOUT 1200)
# REL-03b (R3): a failed new-world spawn search (chunk generation refused through the
# --wrap=mem2_alloc fault seam) stops world creation: status query false, no level.dat,
# visible error. Prints "REL-03b RED" and exits 1 on failure.
add_executable(test_spawn_search_failure tests/test_spawn_search_failure.c)
target_link_libraries(test_spawn_search_failure cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_spawn_search_failure PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_options(test_spawn_search_failure PRIVATE -Wl,--wrap=mem2_alloc)
set_target_properties(test_spawn_search_failure PROPERTIES C_STANDARD 99)
add_test(NAME test_spawn_search_failure COMMAND test_spawn_search_failure)
set_tests_properties(test_spawn_search_failure PROPERTIES TIMEOUT 240)
# R3 create path: the REAL dialog create (screen_select_world.c) yields Beta's spawn
# for the seed that needs mid-search population (3141592653589793238), served by the
# server half of the svin_spawn_* mailbox (server_local_spawn_search_service).
add_executable(test_spawn_create_path tests/test_spawn_create_path.c)
target_link_libraries(test_spawn_create_path cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_spawn_create_path PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_spawn_create_path PROPERTIES C_STANDARD 99)
add_test(NAME test_spawn_create_path COMMAND test_spawn_create_path)
set_tests_properties(test_spawn_create_path PROPERTIES TIMEOUT 600)
# REL-05-tests (R5): a REAL forced light halt (production reserve refusal via
# server_world_test_light_fail_after inside a production nested callback) at each
# committed HALT-STOP P1 boundary (D1 lever fan, D2 setter fan, D3 wire drain, D4
# scheduled tick + rearm tail): halt latch, no later dispatch/mutation/save, last
# committed save byte-identical after reopen; healthy control + in-process
# stop-byte-blinded mutants (CW_REL05_MUTANT=<name>|all runs a mutant as primary).
add_executable(test_light_halt_forced tests/test_light_halt_forced.c)
target_link_libraries(test_light_halt_forced cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_light_halt_forced PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_light_halt_forced PROPERTIES C_STANDARD 99)
add_test(NAME test_light_halt_forced COMMAND test_light_halt_forced)
set_tests_properties(test_light_halt_forced PROPERTIES TIMEOUT 120)
# REL-FLK-tests: a full-column refresh of a visible column keeps drawing its
# retained geometry (held real worker, column-specific submissions per frame).
# GL leaves, the two draw seams and the install-result witness are test-local link wrappers.
add_executable(test_chunk_refresh_draw_continuity tests/test_chunk_refresh_draw_continuity.c)
target_link_libraries(test_chunk_refresh_draw_continuity cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_chunk_refresh_draw_continuity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_options(test_chunk_refresh_draw_continuity PRIVATE
	-Wl,--wrap=chunk_render -Wl,--wrap=displaylist_render -Wl,--wrap=displaylist_destroy
	-Wl,--wrap=world_build_full_section -Wl,--wrap=world_build_full_section_leased
	-Wl,--wrap=gfx_fog -Wl,--wrap=gfx_lighting -Wl,--wrap=gfx_tint -Wl,--wrap=gfx_alpha_test
	-Wl,--wrap=gfx_alpha_test_ref -Wl,--wrap=gfx_bind_texture -Wl,--wrap=gfx_blending
	-Wl,--wrap=gfx_cull_func -Wl,--wrap=gfx_write_buffers -Wl,--wrap=gfx_matrix_modelview
	-Wl,--wrap=gfx_matrix_texture -Wl,--wrap=gfx_fog_pos)
set_target_properties(test_chunk_refresh_draw_continuity PROPERTIES C_STANDARD 99)
add_test(NAME test_chunk_refresh_draw_continuity COMMAND test_chunk_refresh_draw_continuity)
set_tests_properties(test_chunk_refresh_draw_continuity PROPERTIES TIMEOUT 120)
# REL-TNT-tests (R5a): a real light-reservation refusal at powered TNT's AIR write
# (BlockTNT.onBlockAdded / onNeighborBlockChange, entity before AIR) must halt
# through P1 instead of terminating. abort is a target-local link wrapper that only
# reports the verified RED shape (exit 1, "REL-TNT RED:"); server_world_set_block
# is wrapped only to record the refused call's coordinate. --witness added|neighbour
# runs one witness; no argument runs both (GREEN).
add_executable(test_tnt_light_halt tests/test_tnt_light_halt.c)
target_link_libraries(test_tnt_light_halt cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_tnt_light_halt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_options(test_tnt_light_halt PRIVATE -Wl,--wrap=abort -Wl,--wrap=server_world_set_block)
set_target_properties(test_tnt_light_halt PROPERTIES C_STANDARD 99)
add_test(NAME test_tnt_light_halt COMMAND test_tnt_light_halt)
set_tests_properties(test_tnt_light_halt PROPERTIES TIMEOUT 120)
# IN-003: client Sky/Block == server stored light after the initial
# generation-light publication (Beta Chunk.java:474 getChunkData).
add_executable(test_client_sky_matches_server tests/test_client_sky_matches_server.c)
target_link_libraries(test_client_sky_matches_server cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_client_sky_matches_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_client_sky_matches_server PROPERTIES C_STANDARD 99)
add_test(NAME test_client_sky_matches_server COMMAND test_client_sky_matches_server)
set_tests_properties(test_client_sky_matches_server PROPERTIES TIMEOUT 300)

# Multiplayer setup screen (screen_multiplayer_setup.c, maintainer-requested
# 2026-08-31 v1/v2, v3 2026-08-31 later-Minecraft-style upgrade): the pure,
# gstate/disk-free half only -- config.c's config_read_server_list/
# config_write_server_list (migration + the config round-trip) against temp
# `struct config` objects, the screen's own CAVEWII_CLIENT_INTERFACE_TEST
# array-mutation hooks, and (v3) the pure 0xFF/string16 ping-response parser
# + latency->bars mapping. Needs mp_codec.c directly (mp_address_parse/
# mp_login_username_valid/mp_utf16_to_display) -- same CAVEX_MP_SOURCES-
# outside-the-engine-manifest reason as test_mp_s0_env above. v3's ping
# subsystem references net_socket_connect/_send/_receive/_close even though
# no test case opens a real socket (the pure parser/bars hooks are the only
# things actually exercised), so net_socket.c must link -- same shape as
# test_mp_s0_env's own net_socket.c + ws2_32 requirement.
add_executable(test_mp_setup_screen
	source/network/mp_codec.c
	source/platform/net_socket.c
	source/game/gui/screen_multiplayer_setup.c
	tests/test_mp_setup_screen.c
)
target_link_libraries(test_mp_setup_screen cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_mp_setup_screen PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_mp_setup_screen PROPERTIES C_STANDARD 99)
target_link_libraries(test_mp_setup_screen ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
if(WIN32)
	target_link_libraries(test_mp_setup_screen ws2_32)
endif()
add_test(NAME test_mp_setup_screen COMMAND test_mp_setup_screen)

# Beta 1.7.3 compass + clock (gap-audit parity tail): registration golden
# (ids 345/347, icons (6,3)/(6,4), plain-item properties) + the two
# CraftingManager:71-72 plus-shape recipes with metal-discriminator and
# superset-grid negatives. Links the full source set for items_init.
add_executable(test_compass_clock_recipe tests/test_compass_clock_recipe.c)
target_link_libraries(test_compass_clock_recipe cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_compass_clock_recipe PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_compass_clock_recipe PROPERTIES C_STANDARD 99)
target_link_libraries(test_compass_clock_recipe ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
add_test(NAME test_compass_clock_recipe COMMAND test_compass_clock_recipe)

# Input-layer edge semantics: the merged per-action mask (one down-state ORed from every
# binding source, edges computed once per frame) that replaced the per-binding
# `any_pressed && !any_held && !any_released` anti-ghost rule. Pins the swallowed sibling
# press, all-sources-clear release, within-frame idempotence, chord latch + component
# masking, and the PC mouse codes the old chord cutoff ate. Links the REAL PLATFORM_PC
# input.c; the test supplies its own glfwGetKey/glfwGetMouseButton (so glfw is NOT linked --
# only its headers are needed) plus link-only stubs for the engine symbols input.c
# references but this test never calls. Registered before the -UNDEBUG sweep below.
add_executable(test_input_edges
	source/platform/input_inventory.c
	source/platform/input_glyph.c
	source/platform/time.c
	source/platform/input.c
	source/config.c
	source/parson/parson.c
	tests/test_input_edges.c
)
target_compile_definitions(test_input_edges PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_input_edges PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	$<TARGET_PROPERTY:glfw,INTERFACE_INCLUDE_DIRECTORIES>
)
set_target_properties(test_input_edges PROPERTIES C_STANDARD 99)
target_link_libraries(test_input_edges m)
add_test(NAME test_input_edges COMMAND test_input_edges)

# ui-build-1026 T15c: the menus_dwell walker's held level + release edge in
# input.c (one walker press = one page-swap tap; hardware unchanged when the
# walker is inactive). Same minimal PLATFORM_PC object set as test_input_edges.
add_executable(test_menu_autopilot_input_level
	source/platform/input_inventory.c
	source/platform/input_glyph.c
	source/platform/time.c
	source/platform/input.c
	source/config.c
	source/parson/parson.c
	tests/test_menu_autopilot_input_level.c
)
target_compile_definitions(test_menu_autopilot_input_level PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_menu_autopilot_input_level PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
	$<TARGET_PROPERTY:glfw,INTERFACE_INCLUDE_DIRECTORIES>
)
set_target_properties(test_menu_autopilot_input_level PROPERTIES C_STANDARD 99)
target_link_libraries(test_menu_autopilot_input_level m)
add_test(NAME test_menu_autopilot_input_level COMMAND test_menu_autopilot_input_level)

# Controller identity/notice timing and production atlas lookup.
add_executable(test_input_glyphs
    tests/test_input_glyphs.c
    source/platform/input_inventory.c
    source/platform/time.c
    source/platform/input_glyph.c
    source/graphics/gui_util.c
    source/config.c
    source/parson/parson.c
    source/lodepng/lodepng.c)
target_compile_definitions(test_input_glyphs PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_input_glyphs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
# Capture actual GUI quads with strict link-only stubs for unrelated draws.
target_link_libraries(test_input_glyphs m)
add_test(NAME test_input_glyphs COMMAND test_input_glyphs)
set_tests_properties(test_input_glyphs PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_input_notices
    tests/test_input_notices.c
    source/platform/input_inventory.c
    source/platform/time.c)
target_compile_definitions(test_input_notices PRIVATE PLATFORM_PC)
target_include_directories(test_input_notices PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_input_notices m)
add_test(NAME test_input_notices COMMAND test_input_notices)

# ---- Lane 3: approved controller events + pause recovery -------------------
# Detection and the card MODEL are pure (no GX, no clock, no driver): these two
# drive the real input_inventory state machine directly.
add_executable(test_controller_event_states
    tests/test_controller_event_states.c
    source/platform/input_inventory.c
    source/platform/time.c)
target_compile_definitions(test_controller_event_states PRIVATE PLATFORM_PC)
target_include_directories(test_controller_event_states PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_controller_event_states m)
add_test(NAME test_controller_event_states COMMAND test_controller_event_states)

# Lane 5: furnace / dispenser approved composition. Uses the production
# container_compact + window layer through the shared test pools, exactly like
# test_container_menu.
foreach(_cw_l5 test_furnace_approved_layout test_dispenser_approved_layout)
	add_executable(${_cw_l5} tests/${_cw_l5}.c)
	target_link_libraries(${_cw_l5} cavex_test_core cavex_test_world
		cavex_test_tickq cavex_test_input)
	target_include_directories(${_cw_l5} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/source
		${CMAKE_CURRENT_SOURCE_DIR}/third_party)
	set_target_properties(${_cw_l5} PROPERTIES C_STANDARD 99)
	target_link_libraries(${_cw_l5} ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw
		GLEW::GLEW OpenGL::GL m)
	add_test(NAME ${_cw_l5} COMMAND ${_cw_l5})
endforeach()

# Lane 5: the approved sign editor. Drives the REAL widget_osk edit model and
# the real sign cell diff; only the OSK's draw/device edge is doubled.
add_executable(test_sign_approved_flow
    tests/test_sign_approved_flow.c
    source/game/gui/widget_osk.c
    source/game/gui/sign_osk_model.c
    source/game/gui/menu_header.c)
target_compile_definitions(test_sign_approved_flow PRIVATE
    PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_sign_approved_flow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_sign_approved_flow m)
add_test(NAME test_sign_approved_flow COMMAND test_sign_approved_flow)

# Lane 5: the approved death screen. The REAL screen_death.c runs against
# doubled device/graphics/transport symbols in the test TU.
add_executable(test_death_commit_timing
    tests/test_death_commit_timing.c
    source/game/gui/screen_death.c
    source/game/gui/menu_header.c
    source/platform/time.c)
target_compile_definitions(test_death_commit_timing PRIVATE
    PLATFORM_PC CAVEWII_CLIENT_INTERFACE_TEST CGLM_ALL_UNALIGNED)
target_include_directories(test_death_commit_timing PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_death_commit_timing m)
add_test(NAME test_death_commit_timing COMMAND test_death_commit_timing)

# Lane 5: the approved pause composition. The REAL screen_pause.c runs; only
# the device/graphics/transport boundary is doubled inside the test TU, the
# same shape test_controller_event_cards uses.
add_executable(test_pause_approved_actions
    tests/test_pause_approved_actions.c
    source/game/gui/screen_pause.c
    source/game/gui/menu_header.c
    source/platform/input_inventory.c
    source/platform/time.c)
target_compile_definitions(test_pause_approved_actions PRIVATE
    PLATFORM_PC CAVEWII_CLIENT_INTERFACE_TEST CGLM_ALL_UNALIGNED)
target_include_directories(test_pause_approved_actions PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_pause_approved_actions m)
add_test(NAME test_pause_approved_actions COMMAND test_pause_approved_actions)

add_executable(test_mp_pause_parity source/game/agent_input.c tests/test_mp_pause_parity.c)
target_link_libraries(test_mp_pause_parity cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input_agent ${CMAKE_THREAD_LIBS_INIT}
	ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_compile_definitions(test_mp_pause_parity PRIVATE CAVEWII_AGENT_BRIDGE)
target_include_directories(test_mp_pause_parity PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
add_test(NAME test_mp_pause_parity COMMAND test_mp_pause_parity)

# Lane 5: the approved header/controller-notice composition. Pure geometry --
# menu_header.c pulls in nothing but gfx_settings.h, so the golden runs with no
# GL context, no window and no font metrics.
add_executable(test_menu_header_notice_composition
    tests/test_menu_header_notice_composition.c
    source/game/gui/menu_header.c)
target_include_directories(test_menu_header_notice_composition PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_menu_header_notice_composition m)
add_test(NAME test_menu_header_notice_composition
    COMMAND test_menu_header_notice_composition)

add_executable(test_controller_event_cards
    tests/test_controller_event_cards.c
    source/platform/input_inventory.c
    source/platform/time.c)
target_compile_definitions(test_controller_event_cards PRIVATE PLATFORM_PC)
target_include_directories(test_controller_event_cards PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_controller_event_cards m)
add_test(NAME test_controller_event_cards COMMAND test_controller_event_cards)

# The cue tests join the real detection layer to the REAL shared dispatcher
# (audio.c is compiled into the test TU behind backend doubles, exactly like
# test_menu_sound_events) so the per-device collapse window and battery latch
# under test are the shipping ones.
add_executable(test_controller_event_audio
    tests/test_controller_event_audio.c
    source/platform/input_inventory.c
    source/platform/controller_events.c
    source/platform/time.c)
target_compile_definitions(test_controller_event_audio PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_controller_event_audio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_controller_event_audio m)
add_test(NAME test_controller_event_audio COMMAND test_controller_event_audio)

add_executable(test_low_battery_episode
    tests/test_low_battery_episode.c
    source/platform/input_inventory.c
    source/platform/controller_events.c
    source/platform/time.c)
target_compile_definitions(test_low_battery_episode PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_low_battery_episode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_low_battery_episode m)
add_test(NAME test_low_battery_episode COMMAND test_low_battery_episode)

# The recovery machine is pure state: the test supplies the two audio symbols
# controller_events.c references but this test never reaches.
add_executable(test_pause_reconnect_resume_gate
    tests/test_pause_reconnect_resume_gate.c
    source/platform/controller_events.c
    source/platform/input_inventory.c
    source/platform/time.c)
target_compile_definitions(test_pause_reconnect_resume_gate PRIVATE PLATFORM_PC)
target_include_directories(test_pause_reconnect_resume_gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_pause_reconnect_resume_gate m)
add_test(NAME test_pause_reconnect_resume_gate COMMAND test_pause_reconnect_resume_gate)

# Host driver doubles compile the real Wii adapter, without a production seam.
add_executable(test_input_wii_adapter
    source/game/gui/screen.c
    source/game/gui/loading_tips.c
    source/game/gui/screen_pause.c
    source/game/gui/screen_controller_settings.c
    source/game/gui/container_menu.c
    source/game/gui/menu_header.c
    tests/test_input_wii_adapter.c
    source/platform/input.c
    source/platform/input_inventory.c
    source/platform/controller_events.c
    source/platform/input_glyph.c
    source/graphics/gui_util.c
    source/config.c
    source/parson/parson.c)
target_compile_definitions(test_input_wii_adapter PRIVATE PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST)
target_include_directories(test_input_wii_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_input_wii_adapter PROPERTIES C_STANDARD 99)
target_link_libraries(test_input_wii_adapter m)
add_test(NAME test_input_wii_adapter COMMAND test_input_wii_adapter)
set_tests_properties(test_input_wii_adapter PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ir-jitter-1005/J01: Wii menu pointer sub-pixel deadzone golden. Same real Wii
# adapter + driver fixture as test_input_wii_adapter; the test #includes that
# fixture with its main renamed, so only the test source differs.
add_executable(test_menu_ir_jitter_golden
    source/game/gui/screen.c
    source/game/gui/loading_tips.c
    source/game/gui/screen_pause.c
    source/game/gui/screen_controller_settings.c
    source/game/gui/container_menu.c
    source/game/gui/menu_header.c
    tests/test_menu_ir_jitter_golden.c
    source/platform/input.c
    source/platform/input_inventory.c
    source/platform/controller_events.c
    source/platform/input_glyph.c
    source/graphics/gui_util.c
    source/config.c
    source/parson/parson.c)
target_compile_definitions(test_menu_ir_jitter_golden PRIVATE PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST)
target_include_directories(test_menu_ir_jitter_golden PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_menu_ir_jitter_golden PROPERTIES C_STANDARD 99)
target_link_libraries(test_menu_ir_jitter_golden m)
add_test(NAME test_menu_ir_jitter_golden COMMAND test_menu_ir_jitter_golden)
set_tests_properties(test_menu_ir_jitter_golden PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# push-0921/A1: the front-end menus with NO POINTER available. Same focused
# shape as test_input_wii_adapter right above -- the REAL PLATFORM_WII
# input.c on the libogc driver doubles in tests/support/wii_input -- but
# driving the REAL update() handlers of the seven pre-world screens through
# the three controller families' shipped binding tables. That is the only
# honest rig for this card: the Close/page/focus roles are decided per FAMILY
# by config_wii.json plus input_owner_accepts' per-family code filter, and a
# PLATFORM_PC build has neither. The MP screens compile in directly (they are
# CAVEX_MP_SOURCES, outside the engine manifest -- same reason
# test_mp_setup_screen compiles them itself); their transport
# (net_socket/net_bringup/mp_session/server_remote) is stubbed in the rig
# because no case here opens a socket.
# Both real-screen controller suites use the same Wii input profile and
# device doubles. Pool their shared frontend TUs once; screens unique to each
# suite stay explicit. This is separate from the PC full-engine test pools.
set(CAVEX_FRONTEND_INPUT_TEST_SOURCES
    source/game/gui/screen_select_world.c
    source/game/gui/loading_tips.c
    source/game/gui/screen_title.c
    source/game/gui/screen_confirm.c
    # ui-build-1026/T09: select world opens the convert backup screen, which
    # drives world_copy (+ its cNBT metadata helpers).
    source/game/gui/screen_convert_backup.c
    source/network/world_copy.c
    source/cNBT/buffer.c
    source/cNBT/nbt_loading.c
    source/cNBT/nbt_parsing.c
    source/cNBT/nbt_treeops.c
    source/cNBT/nbt_util.c
    source/game/gui/widget_slotlist.c
    source/game/gui/widget_osk.c
    source/game/gui/play_session.c
    source/game/gui/cworld_form_model.c
    source/stack.c
    source/util.c
    source/platform/input.c
    source/platform/input_inventory.c
    source/platform/controller_events.c
    source/platform/input_glyph.c
    source/graphics/gui_util.c
    source/config.c
    source/parson/parson.c
    source/game/gui/widget_osk_frontend.c
)
add_library(cavex_frontend_input_pool OBJECT ${CAVEX_FRONTEND_INPUT_TEST_SOURCES})
target_link_libraries(cavex_frontend_input_pool PUBLIC ZLIB::ZLIB) # T09: world_copy/cNBT
target_compile_definitions(cavex_frontend_input_pool PRIVATE
    PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST
    CAVEWII_RAND_SEED_TEST)
target_include_directories(cavex_frontend_input_pool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_options(cavex_frontend_input_pool PRIVATE -UNDEBUG)
set_target_properties(cavex_frontend_input_pool PROPERTIES C_STANDARD 99)

add_executable(test_frontend_no_pointer
    tests/test_frontend_no_pointer.c
    source/game/gui/screen_texturepacks.c
    source/game/gui/screen_creative_picker.c
    source/game/gui/screen_multiplayer_setup.c
    source/game/gui/screen_multiplayer_connect.c
    source/network/mp_codec.c
    source/texturepacks.c
    # ingame-import-1030: the Import button opens this screen; the worker
    # is the scriptable double (no SD card / thread in this rig).
    source/game/gui/screen_beta_import.c
    tests/support/fake_beta_import.c)
target_link_libraries(test_frontend_no_pointer cavex_frontend_input_pool)
target_compile_definitions(test_frontend_no_pointer PRIVATE
    PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST)
target_include_directories(test_frontend_no_pointer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_frontend_no_pointer PROPERTIES C_STANDARD 99)
target_link_libraries(test_frontend_no_pointer m)
add_test(NAME test_frontend_no_pointer COMMAND test_frontend_no_pointer)
set_tests_properties(test_frontend_no_pointer PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ui-build-1026/T01: ONE pointer click chooses a Load / Texture packs row.
# Same real-screen Wii input rig and TU set as test_frontend_no_pointer (its
# source is #included whole, main renamed); own sandbox dir for ctest -j.
add_executable(test_menu_one_click_rows
    tests/test_menu_one_click_rows.c
    source/game/gui/screen_texturepacks.c
    source/game/gui/screen_creative_picker.c
    source/game/gui/screen_multiplayer_setup.c
    source/game/gui/screen_multiplayer_connect.c
    source/network/mp_codec.c
    source/texturepacks.c
    # ingame-import-1030: the Import button opens this screen; the worker
    # is the scriptable double (no SD card / thread in this rig).
    source/game/gui/screen_beta_import.c
    tests/support/fake_beta_import.c)
target_link_libraries(test_menu_one_click_rows cavex_frontend_input_pool)
target_compile_definitions(test_menu_one_click_rows PRIVATE
    PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST)
target_include_directories(test_menu_one_click_rows PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_menu_one_click_rows PROPERTIES C_STANDARD 99)
target_link_libraries(test_menu_one_click_rows m)
add_test(NAME test_menu_one_click_rows COMMAND test_menu_one_click_rows)
set_tests_properties(test_menu_one_click_rows PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_texturepacks_screen
    tests/test_texturepacks_screen.c
    source/game/gui/screen_rename_world.c
    source/game/gui/screen_texturepacks.c
    source/game/gui/screen_creative_picker.c
    source/game/gui/screen_multiplayer_setup.c
    source/game/gui/screen_multiplayer_connect.c
    source/network/mp_codec.c
    source/texturepacks.c
    # ingame-import-1030: the Import button opens this screen; the worker
    # is the scriptable double (no SD card / thread in this rig).
    source/game/gui/screen_beta_import.c
    tests/support/fake_beta_import.c)
target_link_libraries(test_texturepacks_screen cavex_frontend_input_pool)
target_compile_definitions(test_texturepacks_screen PRIVATE
    PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST)
target_include_directories(test_texturepacks_screen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_texturepacks_screen PROPERTIES C_STANDARD 99)
target_link_libraries(test_texturepacks_screen m)
add_test(NAME test_texturepacks_screen COMMAND test_texturepacks_screen)
set_tests_properties(test_texturepacks_screen PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_options(test_texturepacks_screen PRIVATE -Wl,--wrap=gutil_prompt_glyph -Wl,--wrap=gutil_hint_row_draw -Wl,--wrap=path_from_sd_root)

# ingame-import-1030 (coder C): the "Import Beta 1.7.3" button and progress
# screen -- real screens, real Wii input for all three controller families
# plus the pointer, every backend state through the scriptable double, and
# the return to Texture Packs with the imported pack applied by the real
# transaction. Same rig + TU set as test_texturepacks_screen.
add_executable(test_beta_import_ui
    tests/test_beta_import_ui.c
    source/game/gui/screen_texturepacks.c
    source/game/gui/screen_creative_picker.c
    source/game/gui/screen_multiplayer_setup.c
    source/game/gui/screen_multiplayer_connect.c
    source/network/mp_codec.c
    source/texturepacks.c
    source/game/gui/screen_beta_import.c
    tests/support/fake_beta_import.c)
target_link_libraries(test_beta_import_ui cavex_frontend_input_pool)
target_compile_definitions(test_beta_import_ui PRIVATE
    PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST)
target_include_directories(test_beta_import_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_beta_import_ui PROPERTIES C_STANDARD 99)
target_link_libraries(test_beta_import_ui m)
target_link_options(test_beta_import_ui PRIVATE -Wl,--wrap=path_from_sd_root
    -Wl,--wrap=gutil_text_rgb -Wl,--wrap=tex_init -Wl,--wrap=audio_registry_reload
    -Wl,--wrap=gfx_bind_texture -Wl,--wrap=screen_notice_compose)
add_test(NAME test_beta_import_ui COMMAND test_beta_import_ui)
set_tests_properties(test_beta_import_ui PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
# PC captures of the same two screens (real render2D into the GL
# framebuffer); a tool, not a test -- it needs a window. Built on request:
#   cmake --build build-verify --target test_beta_import_ui_capture
add_executable(test_beta_import_ui_capture EXCLUDE_FROM_ALL tests/capture_beta_import_ui.c)
target_link_libraries(test_beta_import_ui_capture cavex_test_core cavex_test_world
    cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw
    GLEW::GLEW OpenGL::GL m)
target_include_directories(test_beta_import_ui_capture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_beta_import_ui_capture PROPERTIES C_STANDARD 99)
target_link_options(test_beta_import_ui_capture PRIVATE -Wl,--wrap=input_prompt_frame)

# ui-build-1026/T03: the title's Tutorial world row (load / honest dialog /
# walker seam + the walker's own walk decision on the real title). Same rig
# + TU set as test_menu_one_click_rows, plus the pure walker script.
add_executable(test_title_tutorial
    tests/test_title_tutorial.c
    source/game/menu_autopilot_script.c
    source/game/gui/screen_texturepacks.c
    source/game/gui/screen_creative_picker.c
    source/game/gui/screen_multiplayer_setup.c
    source/game/gui/screen_multiplayer_connect.c
    source/network/mp_codec.c
    source/texturepacks.c
    # ingame-import-1030: the Import button opens this screen; the worker
    # is the scriptable double (no SD card / thread in this rig).
    source/game/gui/screen_beta_import.c
    tests/support/fake_beta_import.c)
target_link_libraries(test_title_tutorial cavex_frontend_input_pool)
target_compile_definitions(test_title_tutorial PRIVATE
    PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST)
target_include_directories(test_title_tutorial PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_title_tutorial PROPERTIES C_STANDARD 99)
target_link_libraries(test_title_tutorial m)
add_test(NAME test_title_tutorial COMMAND test_title_tutorial)
set_tests_properties(test_title_tutorial PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# push-0921/A5: the ONE menu activation rule (Astra ruling 2) as a pure truth
# table. gutil_menu_activate_decide takes no statics, no clock and no input, so
# the only thing this target needs is gui_util.c itself -- the draw-side
# externals it references are link-only doubles in the test, exactly the shape
# test_input_glyphs established for the same TU. No screen, no rig, no config.
add_executable(test_menu_activate_rule
    tests/test_menu_activate_rule.c
    source/graphics/gui_util.c
    source/platform/input_glyph.c
    source/platform/input_inventory.c
    source/platform/time.c
    source/config.c
    source/parson/parson.c)
target_compile_definitions(test_menu_activate_rule PRIVATE
    PLATFORM_PC CGLM_ALL_UNALIGNED)
target_include_directories(test_menu_activate_rule PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_menu_activate_rule PROPERTIES C_STANDARD 99)
target_link_libraries(test_menu_activate_rule m)
add_test(NAME test_menu_activate_rule COMMAND test_menu_activate_rule)

add_executable(test_carve_observer tests/test_carve_observer.c)
target_link_libraries(test_carve_observer cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_carve_observer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_carve_observer PROPERTIES C_STANDARD 99)
add_test(NAME test_carve_observer COMMAND test_carve_observer)
# Boot/unload windows of the server world vs. the always-armed carve boundary
# hook (needs the real server_local + server_world, hence the pooled link).
add_executable(test_carve_boundary_world_lifecycle tests/test_carve_boundary_world_lifecycle.c)
target_link_libraries(test_carve_boundary_world_lifecycle cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_carve_boundary_world_lifecycle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_carve_boundary_world_lifecycle PROPERTIES C_STANDARD 99)
add_test(NAME test_carve_boundary_world_lifecycle COMMAND test_carve_boundary_world_lifecycle)
# The carve observer must not starve the server thread's far-skin extraction:
# per-column carve-event count, the modelled main-loop/server interleave cost
# gate (< 2x unarmed), and the byte-identity gate on the emitted records at the
# pre-2026-09-08 per-frame walk cadence. Needs the real server_world generator.
add_executable(test_carve_frame_walk_cost tests/test_carve_frame_walk_cost.c)
target_link_libraries(test_carve_frame_walk_cost cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_carve_frame_walk_cost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_carve_frame_walk_cost PROPERTIES C_STANDARD 99)
add_test(NAME test_carve_frame_walk_cost COMMAND test_carve_frame_walk_cost)
# lane/dl-ladder-pools fix round 2 (2026-09-08): the fixed-tenant slot pools
# must not corrupt the address-keyed carve ledger (slot 0 aliased the pool
# reservation, so the first take collided and the first release ERASED the
# reservation -- 3.03 MB of unaccounted carve on real Wii 703aef00), and one
# missed FREE note must cost one counted error rather than blinding the ledger.
add_executable(test_carve_pool_ledger tests/test_carve_pool_ledger.c)
target_link_libraries(test_carve_pool_ledger cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_carve_pool_ledger PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_carve_pool_ledger PROPERTIES C_STANDARD 99)
add_test(NAME test_carve_pool_ledger COMMAND test_carve_pool_ledger)
add_executable(test_startup_retirement tests/test_startup_retirement.c)
target_link_libraries(test_startup_retirement cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_startup_retirement PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_startup_retirement PROPERTIES C_STANDARD 99)
add_test(NAME test_startup_retirement COMMAND test_startup_retirement)
# ~185s (two 600-tick continuations through the production reload) -- legitimately
# > 60s, so it must not share a `ctest -jN` core slot with 3 other tests for that
# whole span; RUN_SERIAL runs it alone, once every other test has finished feeding
# the pool, so it gets the CPU time its own iteration-budget mesh-wait assumes.
set_tests_properties(test_startup_retirement PROPERTIES RUN_SERIAL TRUE)
add_executable(test_admission_light_equiv tests/test_admission_light_equiv.c)
target_link_libraries(test_admission_light_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_admission_light_equiv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_admission_light_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_admission_light_equiv COMMAND test_admission_light_equiv)

add_executable(test_settle_observer tests/test_settle_observer.c)
target_link_libraries(test_settle_observer cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_settle_observer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_settle_observer PROPERTIES C_STANDARD 99)
add_test(NAME test_settle_observer COMMAND test_settle_observer)
add_executable(test_tps_observer tests/test_tps_observer.c)
target_link_libraries(test_tps_observer cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_tps_observer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_tps_observer PROPERTIES C_STANDARD 99)
add_test(NAME test_tps_observer COMMAND test_tps_observer)
add_executable(test_server_replay_record tests/test_server_replay_record.c)
target_link_libraries(test_server_replay_record cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_server_replay_record PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_server_replay_record PROPERTIES C_STANDARD 99)
add_test(NAME test_server_replay_record COMMAND test_server_replay_record)
add_test(NAME test_analyze_settle COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_analyze_settle.py)
add_test(NAME test_analyze_tps_walk COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_analyze_tps_walk.py)
add_executable(test_split_residency_single_equiv tests/test_split_residency_single_equiv.c)
target_link_libraries(test_split_residency_single_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_residency_single_equiv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_residency_single_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_split_residency_single_equiv COMMAND test_split_residency_single_equiv)
add_executable(test_split_residency_fairness tests/test_split_residency_fairness.c)
target_link_libraries(test_split_residency_fairness cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_residency_fairness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_residency_fairness PROPERTIES C_STANDARD 99)
add_test(NAME test_split_residency_fairness COMMAND test_split_residency_fairness)

# Beta 1.7.3 swept entity collision RED (next-gaps-0924/W2-MOVE).
add_executable(test_entity_swept_collision tests/test_entity_swept_collision.c)
target_link_libraries(test_entity_swept_collision cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_entity_swept_collision PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_entity_swept_collision PROPERTIES C_STANDARD 99)
add_test(NAME test_entity_swept_collision COMMAND test_entity_swept_collision)

# Runtime split server radius (maintainer ruling 2026-09-11): the accessor is
# the constant in single player, the dial (capped) with two anchors; the split
# policy turns the far ring off on join and restores it on leave.
add_executable(test_split_view_radius_equiv tests/test_split_view_radius_equiv.c)
target_link_libraries(test_split_view_radius_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_view_radius_equiv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_view_radius_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_split_view_radius_equiv COMMAND test_split_view_radius_equiv)
add_executable(test_split_view_radius_transition tests/test_split_view_radius_transition.c)
target_link_libraries(test_split_view_radius_transition cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_view_radius_transition PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_view_radius_transition PROPERTIES C_STANDARD 99)
add_test(NAME test_split_view_radius_transition COMMAND test_split_view_radius_transition)

# Astra option A / S-A1: the server-owned LIVE split transition. Wraps the mem2
# allocator both ways -- alloc for the residency-block failure injection (the
# existing hook, same as the column-dedup tests) and free for the "exactly one
# block, no leak" accounting by pointer identity.
add_executable(test_split_live_toggle tests/test_split_live_toggle.c)
target_link_libraries(test_split_live_toggle cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_live_toggle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_live_toggle PROPERTIES C_STANDARD 99)
target_link_options(test_split_live_toggle PRIVATE -Wl,--wrap=mem2_alloc -Wl,--wrap=mem2_free)
add_test(NAME test_split_live_toggle COMMAND test_split_live_toggle)
set_tests_properties(test_split_live_toggle PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)

# S1-b mp-split-0912: owner-routed gameplay actions for the split-screen guest.
# Compiled with CAVEWII_AGENT_BRIDGE (registered exemption below) so player 0's
# scripted sequence runs through the REAL merged-mask input_pressed/_held/
# _released -- the owner-0 RPC-stream golden is only worth anything if the host
# half is driven by the production input path, not a double. Player 1 is driven
# by a scripted struct input_player_frame, which is exactly what
# input_owner_latch hands main.c. The transport is the REAL svin queue.
# Re-capture the golden on a pre-S1-b SHA by adding S1B_BASE_GOLDEN here.
add_executable(test_split_actions_client
	source/game/agent_input.c
	tests/test_split_actions_client.c)
target_link_libraries(test_split_actions_client cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input_agent
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_compile_definitions(test_split_actions_client PRIVATE CAVEWII_AGENT_BRIDGE)
target_link_options(test_split_actions_client PRIVATE "-Wl,--wrap=time_get")
target_include_directories(test_split_actions_client PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_actions_client PROPERTIES C_STANDARD 99)
add_test(NAME test_split_actions_client COMMAND test_split_actions_client)
set_tests_properties(test_split_actions_client PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)

# Astra option A / S-A2: the CLIENT half -- the Options row's live request/ack
# state machine (screen_options.c) read at the exact seam main.c reads it. Wraps
# clin_input_epoch (inject the stable world-generation stamp without standing up
# a whole server handoff -- that is S-A1's fixture) and config_save (count the
# commit without writing the repo's own config.json). The requests themselves go
# through the REAL svin transport, so the stamp and the request_seq numbering are
# measured, not asserted about intent.
add_executable(test_split_live_options tests/test_split_live_options.c)
target_link_libraries(test_split_live_options cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_live_options PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_live_options PROPERTIES C_STANDARD 99)
target_link_options(test_split_live_options PRIVATE -Wl,--wrap=clin_input_epoch -Wl,--wrap=config_save)
add_test(NAME test_split_live_options COMMAND test_split_live_options)
# S-A5 (R-A8 review note): this target drives a real transport queue and a row
# state machine with its own 5 s ack timeout, so a regression that wedges either
# would otherwise stall the whole ctest run instead of failing it.
set_tests_properties(test_split_live_options PROPERTIES TIMEOUT 120)

# S-A5 / R-A8 A8-L10: SVIN_PENDING_SIZE grew by one for every class when the
# live split switch took its own reserve slot, moving the global cutoff AND the
# ring's modulus with no transport equivalence proof. This is that proof: the
# pre-change per-class capacities and the FIFO drain order (wrapped and
# unwrapped), asserted against constants recorded from the pre-change source.
# No wraps -- it drives the real svin queue through its public entry points.
add_executable(test_svin_pending_equiv tests/test_svin_pending_equiv.c)
target_link_libraries(test_svin_pending_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_svin_pending_equiv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_svin_pending_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_svin_pending_equiv COMMAND test_svin_pending_equiv)
set_tests_properties(test_svin_pending_equiv PROPERTIES TIMEOUT 120)

# S-A12 (plan item A6): the OFF-path byte-equivalence golden. Runs the real
# 600-tick local-server simulation twice (switch OFF, then ON with no guest)
# and asserts both against a hash recorded from main f646e3a6.
add_executable(test_split_off_equiv tests/test_split_off_equiv.c)
target_link_libraries(test_split_off_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_off_equiv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_off_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_split_off_equiv COMMAND test_split_off_equiv)
set_tests_properties(test_split_off_equiv PROPERTIES TIMEOUT 120)

# A1a split-dimensions-plan-0924: explicit world identity. Single-world
# byte-equivalence golden vs the unmodified base (7f1bd8bf + A0), same-coordinate
# isolation between worlds[0]/worlds[1], and stale-generation handle rejection.
add_executable(test_world_context_equiv tests/test_world_context_equiv.c)
target_link_libraries(test_world_context_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_world_context_equiv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_world_context_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_world_context_equiv COMMAND test_world_context_equiv)
set_tests_properties(test_world_context_equiv PROPERTIES TIMEOUT 240)

# A1b split-dimensions-plan-0924: world-qualified tile lifecycle through the
# live server paths -- typed _w tile APIs, one-world save/evict/reload,
# opening-world window binding (backing, publish, invalidation, stale
# generation), world-selected legacy archives, and world-explicit effects.
add_executable(test_a1b_tile_lifecycle tests/test_a1b_tile_lifecycle.c)
target_link_libraries(test_a1b_tile_lifecycle cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_a1b_tile_lifecycle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_a1b_tile_lifecycle PROPERTIES C_STANDARD 99)
add_test(NAME test_a1b_tile_lifecycle COMMAND test_a1b_tile_lifecycle)
set_tests_properties(test_a1b_tile_lifecycle PROPERTIES TIMEOUT 240)

# IMP-TE-CAPS: a synthetic dense world (12,000 note blocks + 300 signs + 300
# furnaces in r=2) saves, evicts and reloads through the live chunk path:
# every chunk loads first try, every tile entity round-trips identically.
add_executable(test_import_te_dense tests/test_import_te_dense.c)
target_link_libraries(test_import_te_dense cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_import_te_dense PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_import_te_dense PROPERTIES C_STANDARD 99)
add_test(NAME test_import_te_dense COMMAND test_import_te_dense)
set_tests_properties(test_import_te_dense PROPERTIES TIMEOUT 240)

# A1b/fix-1 (Fable review F1): the near-player spawner tracker survives
# repeated world reloads (each a new slot-0 generation) instead of stranding rows.
add_executable(test_a1b_spawner_tracker_reload tests/test_a1b_spawner_tracker_reload.c)
target_link_libraries(test_a1b_spawner_tracker_reload cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_a1b_spawner_tracker_reload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_a1b_spawner_tracker_reload PROPERTIES C_STANDARD 99)
add_test(NAME test_a1b_spawner_tracker_reload COMMAND test_a1b_spawner_tracker_reload)
set_tests_properties(test_a1b_spawner_tracker_reload PROPERTIES TIMEOUT 240)

# A1c split-dimensions-plan-0924 (Astra "A2: A1c"): world operations callable
# per struct server_world* -- slot-0 adapter equivalence + same-coordinate
# isolation, empty->occupied second-world isolation, and (dimension,x,z)
# residency anchors/rows + per-world near-unload send. Landed RED (WILL_FAIL,
# the A1b scaffold convention) at 083ec12b; GREEN with the implementation
# (SERVER_LOCAL_A1C_WORLD_OPS in server_local.h), WILL_FAIL removed.
foreach(_a1c_t test_a1c_world_ops_equiv test_a1c_empty_occupied test_a1c_residency_isolation)
	add_executable(${_a1c_t} tests/${_a1c_t}.c)
	target_link_libraries(${_a1c_t} cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
		${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
	target_include_directories(${_a1c_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
	set_target_properties(${_a1c_t} PROPERTIES C_STANDARD 99)
	add_test(NAME ${_a1c_t} COMMAND ${_a1c_t})
	set_tests_properties(${_a1c_t} PROPERTIES TIMEOUT 300)
endforeach()
# A2-S split-dimensions-plan-0924 (Astra astra-a2s-design-0926): the SHARED
# region-handle pool (region_pool.[ch]) -- one admission authority across both
# worlds (cap 4, global LRU, donor settled+closed BEFORE the candidate opens,
# pinned donors refused with BUSY, missing-region creation inside the cap),
# donor faults (sync failure retains, close failure tombstones), and the
# single-world save-bytes equivalence golden captured on the pre-A2-S base.
# All three A2 tests are GREEN with the region_pool.[ch] implementation
# (REGION_POOL_SHARED_ADMISSION landed); no WILL_FAIL remains on this trio.
add_executable(test_save_path_ops_equiv tests/test_save_path_ops_equiv.c)
target_link_libraries(test_save_path_ops_equiv cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB
	glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_save_path_ops_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_options(test_save_path_ops_equiv PRIVATE
	-Wl,--wrap=fopen,--wrap=rename,--wrap=remove,--wrap=time)
if(WIN32)
	target_link_options(test_save_path_ops_equiv PRIVATE
		-Wl,--wrap=stat64i32,--wrap=_mkdir,--wrap=__imp__mkdir,--wrap=_time64)
else()
	target_link_options(test_save_path_ops_equiv PRIVATE -Wl,--wrap=stat,--wrap=mkdir)
endif()
add_test(NAME test_save_path_ops_equiv COMMAND test_save_path_ops_equiv)
set_tests_properties(test_save_path_ops_equiv PROPERTIES TIMEOUT 300)

add_executable(test_save_path_cache_equiv tests/test_save_path_cache_equiv.c)
target_link_libraries(test_save_path_cache_equiv cavex_test_core cavex_test_world
	cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB
	glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_save_path_cache_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_options(test_save_path_cache_equiv PRIVATE
	-Wl,--wrap=fopen,--wrap=rename,--wrap=remove,--wrap=time,--wrap=opendir)
if(WIN32)
	target_link_options(test_save_path_cache_equiv PRIVATE
		-Wl,--wrap=stat64i32,--wrap=_mkdir,--wrap=__imp__mkdir,--wrap=_time64,--wrap=_open,--wrap=__imp__open)
else()
	target_link_options(test_save_path_cache_equiv PRIVATE -Wl,--wrap=stat,--wrap=mkdir,--wrap=open)
endif()
add_test(NAME test_save_path_cache_equiv COMMAND test_save_path_cache_equiv)
set_tests_properties(test_save_path_cache_equiv PROPERTIES TIMEOUT 300)

# P04: exercise the exact Wii branch/wrappers with distinct native/POSIX cwd.
# The real SDK ABI is independently compiled and the release link is audited.
add_executable(test_save_path_native_equiv tests/test_save_path_native_equiv.c
	source/network/save_path.c)
target_compile_definitions(test_save_path_native_equiv PRIVATE PLATFORM_WII)
target_include_directories(test_save_path_native_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/tests/support/p04_sdk
	${CMAKE_CURRENT_SOURCE_DIR}/source)
target_link_libraries(test_save_path_native_equiv ${CMAKE_THREAD_LIBS_INIT})
target_link_options(test_save_path_native_equiv PRIVATE -Wl,--wrap=getcwd)
set_target_properties(test_save_path_native_equiv PROPERTIES C_STANDARD 11)
add_test(NAME test_save_path_native_equiv COMMAND test_save_path_native_equiv)
set_tests_properties(test_save_path_native_equiv PROPERTIES TIMEOUT 60)

foreach(_a2s_t test_a2_region_pool test_a2_region_donor_faults test_a2_save_bytes_equiv)
	# P03: keep the original four-slot fixtures untouched; new capacity-driven
	# fixtures retain their assertions at the measured production capacity.
	if(_a2s_t STREQUAL "test_a2_save_bytes_equiv")
		add_executable(${_a2s_t} tests/${_a2s_t}.c)
	else()
		add_executable(${_a2s_t} tests/${_a2s_t}_capacity.c)
	endif()
	target_link_libraries(${_a2s_t} cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
		${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
	target_include_directories(${_a2s_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
	set_target_properties(${_a2s_t} PROPERTIES C_STANDARD 99)
	add_test(NAME ${_a2s_t} COMMAND ${_a2s_t})
	set_tests_properties(${_a2s_t} PROPERTIES TIMEOUT 300)
endforeach()
# A2-S part 2 (astra-a2s-design-0926): tile_restore_plan -- flat chest/sign
# archives reload through prepare (scratch decode) -> preflight (row mapping +
# reservations) -> allocation-free commit; slot 1 keeps its rows at their
# indices; every fallible step refuses with the old rows intact. RED-keyed on
# TE_RESTORE_PLAN_API (tile_entities_store.h).
foreach(_a2s2_t test_a2_slot0_reload test_a2_restore_atomic)
	add_executable(${_a2s2_t} tests/${_a2s2_t}.c)
	target_link_libraries(${_a2s2_t} cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
		${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
	target_include_directories(${_a2s2_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
	set_target_properties(${_a2s2_t} PROPERTIES C_STANDARD 99)
	add_test(NAME ${_a2s2_t} COMMAND ${_a2s2_t})
	set_tests_properties(${_a2s2_t} PROPERTIES TIMEOUT 300)
endforeach()
# G6 separation (astra-journal-0926): the checkpoint journal is deferred; a save
# carrying an UNRESOLVED or malformed experimental checkpoint.journal is refused
# visibly at load with files preserved (resolved journals are inert).
add_executable(test_a2_journal_refusal tests/test_a2_journal_refusal.c)
target_link_libraries(test_a2_journal_refusal cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_a2_journal_refusal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_a2_journal_refusal PROPERTIES C_STANDARD 99)
add_test(NAME test_a2_journal_refusal COMMAND test_a2_journal_refusal)
set_tests_properties(test_a2_journal_refusal PROPERTIES TIMEOUT 300)
# A2-T -> A2-W (astra-next-0926 row A6; Astra A2 design sec. 1-2): the actor
# transfer transaction (REQUEST -> PREPARE -> COMMIT / ROLLBACK) and the two-
# world lifecycle/scheduler, one ctest per case on the real admission harness.
# RED by construction until SERVER_TRANSFER_API (source/network/server_transfer.h)
# exists; WILL_FAIL per case, dropped as each lands.
# a2t-guest-0927 (lane C): the guest's actor-transition adapter -- durable
# marker, APPLY, own-dimension checkpoint, stale-actor refusal.
add_executable(test_a2_guest_adapter tests/test_a2_guest_adapter.c)
target_link_libraries(test_a2_guest_adapter cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_a2_guest_adapter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_a2_guest_adapter PROPERTIES C_STANDARD 99)
foreach(_cw_a2g_case adapter dwell dwell_off reopen_nether reopen_overworld reopen_refusal nether_respawn record_restore dwell_host_busy join_cap_ok join_cap_refused join_cap_timeout)
	add_test(NAME test_a2_guest_${_cw_a2g_case} COMMAND test_a2_guest_adapter ${_cw_a2g_case})
	set_tests_properties(test_a2_guest_${_cw_a2g_case} PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
endforeach()
# colcap-1006/CC1 (docs/intent/split-column-cap-256.md, ASTRA-COLUMN-CAP-R1 sec. 5):
# production-path split column-cap reproduction. sep256 is RED at the current
# SERVER_TRANSFER_RESIDENT_COLUMN_CAP (176): two disjoint radius-4 footprints need
# 242 live columns. sep256_budget_off is the GREEN control (payloads off = budget off);
# sep0 is the no-regression occupancy baseline (GREEN).
add_executable(test_split_column_cap tests/test_split_column_cap.c)
target_link_libraries(test_split_column_cap cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_column_cap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_column_cap PROPERTIES C_STANDARD 99)
# colcap-1006/CC2: cap 256 (sep256 GREEN), sustained readiness, two real bodies across
# Overworld/Nether (host/guest reversed, coincident cross-world coordinates), movement
# legs, reservation failure/retry RNG, and the R2 item-5 telemetry contract.
foreach(_cw_colcap_case sep256 sep256_budget_off sep0 ow_ow_sep256 ow_ow_sep256_hostfar
		nether_nether_sep256 nether_nether_sep256_hostfar
		mixed_host_ow_guest_nether_sep256 mixed_host_nether_guest_ow_sep256
		mixed_host_ow_guest_nether_coincident mixed_host_nether_guest_ow_coincident
		cap_retry_rng telemetry_contract republish_keeps_sent heap_walk_corrupt
		saturated_transfers_reunion teleporter_parity_pin teleporter_parity_near
		teleporter_parity_existing_pin teleporter_parity_existing_near
		teleporter_parity_secondary_pin teleporter_parity_secondary_near prepare_bounded)
	add_test(NAME test_split_column_cap_${_cw_colcap_case} COMMAND test_split_column_cap ${_cw_colcap_case})
	set_tests_properties(test_split_column_cap_${_cw_colcap_case} PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 600)
endforeach()
# colcap-1006: the Teleporter parity pair -- the "near" leg compares against the
# digest the "pin" leg writes, so it runs after it and never concurrently.
set_tests_properties(test_split_column_cap_teleporter_parity_pin PROPERTIES
	FIXTURES_SETUP colcap_teleporter_parity RESOURCE_LOCK colcap_teleporter_parity)
set_tests_properties(test_split_column_cap_teleporter_parity_near PROPERTIES
	FIXTURES_REQUIRED colcap_teleporter_parity RESOURCE_LOCK colcap_teleporter_parity)
set_tests_properties(test_split_column_cap_teleporter_parity_existing_pin PROPERTIES
	FIXTURES_SETUP colcap_teleporter_parity_existing
	RESOURCE_LOCK colcap_teleporter_parity_existing)
set_tests_properties(test_split_column_cap_teleporter_parity_existing_near PROPERTIES
	FIXTURES_REQUIRED colcap_teleporter_parity_existing
	RESOURCE_LOCK colcap_teleporter_parity_existing)
set_tests_properties(test_split_column_cap_teleporter_parity_secondary_pin PROPERTIES
	FIXTURES_SETUP colcap_teleporter_parity_secondary
	RESOURCE_LOCK colcap_teleporter_parity_secondary)
set_tests_properties(test_split_column_cap_teleporter_parity_secondary_near PROPERTIES
	FIXTURES_REQUIRED colcap_teleporter_parity_secondary
	RESOURCE_LOCK colcap_teleporter_parity_secondary)
add_executable(test_a2_transfer_transaction tests/test_a2_transfer_transaction.c)
target_link_libraries(test_a2_transfer_transaction cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_a2_transfer_transaction PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_a2_transfer_transaction PROPERTIES C_STANDARD 99)
# host set GREEN (2026-09-26); `opposite` (a guest COMMIT) waits for lane C's
# transition adapter + Astra's guest-admission ruling.
set(_cw_a2t_red) # opposite GREEN with lane C's adapter (a2t-guest-0927)
foreach(_cw_a2t_case host_to_nether rollback opposite duplicate once_only death_respawn reload retired_freed reload_reopen memory_refusal capacity_landing landing_timeout landing_stale retire_checkpoint_fail retire_persists_entities unload_secondary autosave_secondary commit_save_fail travel_gate guest_landing portal_flag_off portal_transfer cap_release cap_lifecycle slot0_anchor landing_real guest_landing_real landing_real_coarse a2w_walk owner_loading_hazards owner_loading_concurrent_transfer a2w_batch_failure a2w_guest_pending same_world_back_to_back cap_population cap_population_retry_equiv departed_restore_host departed_restore_empty cap_request_on0 cap_request_on3 cap_request_on5 cap_request_off0 cap_request_off5 cap_teleporter_wait cap_retained_backing cap_activation cap_ready_target cap_radius_on cap_radius_off landing_residency landing_residency_timeout)
	add_test(NAME test_a2_transfer_${_cw_a2t_case} COMMAND test_a2_transfer_transaction ${_cw_a2t_case})
	if(_cw_a2t_case IN_LIST _cw_a2t_red)
		set_tests_properties(test_a2_transfer_${_cw_a2t_case} PROPERTIES
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300 WILL_FAIL TRUE)
	else()
		set_tests_properties(test_a2_transfer_${_cw_a2t_case} PROPERTIES
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
	endif()
endforeach()
# A2-T D4 (lane B, a2t-d4-0927): block click/break/place callbacks act on the
# ACTOR's committed world (block_world, blocks.h) -- a Nether place/dig leaves
# world 0 untouched; the single-world control still lands in slot 0.
add_executable(test_a2_block_world_d4 tests/test_a2_block_world_d4.c)
target_link_libraries(test_a2_block_world_d4 cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_a2_block_world_d4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_a2_block_world_d4 PROPERTIES C_STANDARD 99)
add_test(NAME test_a2_block_world_d4 COMMAND test_a2_block_world_d4)
set_tests_properties(test_a2_block_world_d4 PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
add_executable(test_fluid_plate_world_routing tests/test_fluid_plate_world_routing.c)
target_link_libraries(test_fluid_plate_world_routing cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_fluid_plate_world_routing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_fluid_plate_world_routing PROPERTIES C_STANDARD 99)
add_test(NAME test_fluid_plate_world_routing COMMAND test_fluid_plate_world_routing)
set_tests_properties(test_fluid_plate_world_routing PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
add_executable(test_entity_world_routing tests/test_entity_world_routing.c)
target_link_libraries(test_entity_world_routing cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_entity_world_routing PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_entity_world_routing PROPERTIES C_STANDARD 99)
add_test(NAME test_entity_world_routing COMMAND test_entity_world_routing)
set_tests_properties(test_entity_world_routing PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
# A2-E split-dimensions-plan-0924 (Astra "A4: A2-E", design M6): generation-safe
# entity bind/resolve -- every server entity is bound by (slot, generation);
# a retired generation cannot resolve into, be ticked/queried by, construct
# into (queued arrow/fireball/split jobs) or be mutated through (jockey
# references) its successor world. Landed RED (WILL_FAIL, the A1b scaffold
# convention); GREEN with ENTITY_A2E_GENERATION_BIND (entity/entity.h).
add_executable(test_a2e_entity_generation tests/test_a2e_entity_generation.c)
target_link_libraries(test_a2e_entity_generation cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_a2e_entity_generation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_a2e_entity_generation PROPERTIES C_STANDARD 99)
add_test(NAME test_a2e_entity_generation COMMAND test_a2e_entity_generation)
set_tests_properties(test_a2e_entity_generation PROPERTIES TIMEOUT 300)
# C1 (split-dimensions lane C -> A handoff, Astra next-0926): a guest tile
# window restored from the guest checkpoint is bound to the guest's committed
# world before inventory_create; NULL world / create failure refuse losslessly.
add_executable(test_c1_guest_window_bind tests/test_c1_guest_window_bind.c)
target_link_libraries(test_c1_guest_window_bind cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_c1_guest_window_bind PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_c1_guest_window_bind PROPERTIES C_STANDARD 99)
add_test(NAME test_c1_guest_window_bind COMMAND test_c1_guest_window_bind)
set_tests_properties(test_c1_guest_window_bind PROPERTIES TIMEOUT 240)

# A1a (Astra astra-A1a-a item 4): occurrence-level baseline gate -- fails on any
# NEW use of the deprecated server_local `world` alias (per file, per receiver).
add_executable(test_world_alias_gate tests/test_world_alias_gate.c)
set_target_properties(test_world_alias_gate PROPERTIES C_STANDARD 99)
add_test(NAME test_world_alias_gate COMMAND test_world_alias_gate ${CMAKE_CURRENT_SOURCE_DIR})

# next-gaps-0924/W1A: player movement parity golden -- suffocation (EntityLiving.
# onEntityUpdate:113-115 + Entity.isEntityInsideOpaqueBlock:851-863, host + guest
# through server_local_update), ground friction/ice (moveEntityWithHeading:478-497),
# ladder sneak hold (:522-524) and 0.02F air/water/lava accel (:457/:469/:489).
add_executable(test_player_move_parity_golden tests/test_player_move_parity_golden.c)
target_link_libraries(test_player_move_parity_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_player_move_parity_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_player_move_parity_golden PROPERTIES C_STANDARD 99)
add_test(NAME test_player_move_parity_golden COMMAND test_player_move_parity_golden)
set_tests_properties(test_player_move_parity_golden PROPERTIES TIMEOUT 300)

# S1-a1b mp-split-0912: the host STATE + RPC-ORDER golden proving that giving
# every block/item place / activate / item-use hook the acting actor is a pure
# parameter move on the host path (Astra gate-2 D1: "'pure move' remains a
# claim requiring proof").
add_executable(test_host_damage_golden tests/test_host_damage_golden.c)
target_link_libraries(test_host_damage_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_host_damage_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_host_damage_golden PROPERTIES C_STANDARD 99)
add_test(NAME test_host_damage_golden COMMAND test_host_damage_golden)
add_executable(test_death_telemetry tests/test_death_telemetry.c)
target_link_libraries(test_death_telemetry cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_death_telemetry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
add_test(NAME test_death_telemetry COMMAND test_death_telemetry)
add_executable(test_perf_stress_survival_guard tests/test_perf_stress_survival_guard.c)
target_link_libraries(test_perf_stress_survival_guard cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_perf_stress_survival_guard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_perf_stress_survival_guard PROPERTIES C_STANDARD 99)
add_test(NAME test_perf_stress_survival_guard COMMAND test_perf_stress_survival_guard)
add_executable(test_service_observer tests/test_service_observer.c
	source/platform/service_observer.c source/platform/time.c source/parson/parson.c)
target_include_directories(test_service_observer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_compile_definitions(test_service_observer PRIVATE PLATFORM_PC)
target_link_libraries(test_service_observer ${CMAKE_THREAD_LIBS_INIT} m)
set_target_properties(test_service_observer PROPERTIES C_STANDARD 99)
add_test(NAME test_service_observer COMMAND test_service_observer)
add_test(NAME test_service_observer_collector COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_service_observer_collector.py
	$<TARGET_FILE:test_service_observer> $<TARGET_FILE:test_hw_telemetry>)
add_test(NAME test_death_telemetry_collector COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_death_telemetry_collector.py $<TARGET_FILE:test_death_telemetry>)
# R18: the G9 tapes these read stay private (rig IP); they skip (77) in the public export.
set_tests_properties(test_service_observer_collector test_death_telemetry_collector PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(test_host_damage_golden PROPERTIES TIMEOUT 300)

# drown-fx-0927: Beta's 8 drowning bubbles (client burst + server trigger).
add_executable(test_drown_bubbles tests/test_drown_bubbles.c)
target_link_libraries(test_drown_bubbles cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_drown_bubbles PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_drown_bubbles PROPERTIES C_STANDARD 99)
add_test(NAME test_drown_bubbles COMMAND test_drown_bubbles)
add_executable(test_damage_parity_golden tests/test_damage_parity_golden.c)
target_link_libraries(test_damage_parity_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_damage_parity_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_damage_parity_golden PROPERTIES C_STANDARD 99)
add_test(NAME test_damage_parity_golden COMMAND test_damage_parity_golden)
set_tests_properties(test_damage_parity_golden PROPERTIES TIMEOUT 300)

add_executable(test_knockback_client_motion tests/test_knockback_client_motion.c)
target_link_libraries(test_knockback_client_motion cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_knockback_client_motion PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_knockback_client_motion PROPERTIES C_STANDARD 99)
add_test(NAME test_knockback_client_motion COMMAND test_knockback_client_motion)
set_tests_properties(test_knockback_client_motion PROPERTIES TIMEOUT 300)
# vehicle-smoke-1004 G8: knockback as client velocity
add_executable(test_knockback_client_steer tests/test_knockback_client_steer.c)
target_link_libraries(test_knockback_client_steer cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_knockback_client_steer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_knockback_client_steer PROPERTIES C_STANDARD 99)
add_test(NAME test_knockback_client_steer COMMAND test_knockback_client_steer)
set_tests_properties(test_knockback_client_steer PROPERTIES TIMEOUT 300)

add_executable(test_block_hooks_actor_golden tests/test_block_hooks_actor_golden.c)
target_link_libraries(test_block_hooks_actor_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_block_hooks_actor_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_block_hooks_actor_golden PROPERTIES C_STANDARD 99)
add_test(NAME test_block_hooks_actor_golden COMMAND test_block_hooks_actor_golden)
set_tests_properties(test_block_hooks_actor_golden PROPERTIES TIMEOUT 300)

add_executable(test_stew_bowl_return tests/test_stew_bowl_return.c)
target_link_libraries(test_stew_bowl_return cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_stew_bowl_return PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_stew_bowl_return PROPERTIES C_STANDARD 99)
add_test(NAME test_stew_bowl_return COMMAND test_stew_bowl_return)

add_executable(test_split_view_radius_pending_far tests/test_split_view_radius_pending_far.c)
target_link_libraries(test_split_view_radius_pending_far cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_view_radius_pending_far PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_view_radius_pending_far PROPERTIES C_STANDARD 99)
add_test(NAME test_split_view_radius_pending_far COMMAND test_split_view_radius_pending_far)

# LCE-01 lane B B1 (2026-09-14): the achievement/statistic data model and its
# versioned .cwstats file. test_achievements_persist covers design 9's
# persistence row (round-trip identity, every write-fault stage, .bad/.bak,
# newer-version refusal, saturation, unknown record letter);
# test_achievements_golden pins the exact file bytes against a committed
# golden with the clock injected.
add_executable(test_achievements_persist tests/test_achievements_persist.c)
target_link_libraries(test_achievements_persist cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievements_persist PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievements_persist PROPERTIES C_STANDARD 99)
add_test(NAME test_achievements_persist COMMAND test_achievements_persist)
add_executable(test_achievements_golden tests/test_achievements_golden.c)
target_link_libraries(test_achievements_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievements_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievements_golden PROPERTIES C_STANDARD 99)
add_test(NAME test_achievements_golden COMMAND test_achievements_golden)
# Reads tests/golden/achievements/b1_golden.cwstats by repo-relative path, the
# same way test_sort_presentation reads its golden.
set_tests_properties(test_achievements_golden PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# LCE-01 lane B B2 (2026-09-14): the game_event seam consumer. Design 9 rows
# `test_achievements_core` (the sixteen triggers, the non-triggers, the
# parent-locked drop, repeat-without-a-second-toast, the toast window),
# `test_achievements_owner` (bind rejections, per-slot isolation, stale
# generations, unbind drains+flushes+clears) and `test_achievements_seam`
# (three producer threads, ordering within a thread, and the full stress
# envelope with the drain stalled: counters identical to a single-threaded
# reference model, progress_incomplete never set).
add_executable(test_achievements_core tests/test_achievements_core.c)
target_link_libraries(test_achievements_core cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievements_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievements_core PROPERTIES C_STANDARD 99)
add_test(NAME test_achievements_core COMMAND test_achievements_core)
# ui-wiring-1010 ACH-1: real normal/shift producers, owner attribution,
# prediction/MP exclusion and the consumer parent gate.
add_executable(test_achievement_producers tests/test_achievement_producers.c)
target_link_libraries(test_achievement_producers cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievement_producers PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievement_producers PROPERTIES C_STANDARD 99)
target_link_options(test_achievement_producers PRIVATE
	-Wl,--wrap=game_event_emit -Wl,--wrap=input_pointer_enable)
add_test(NAME test_achievement_producers COMMAND test_achievement_producers)
set_tests_properties(test_achievement_producers PROPERTIES TIMEOUT 120)
add_executable(test_achievements_owner tests/test_achievements_owner.c)
target_link_libraries(test_achievements_owner cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievements_owner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievements_owner PROPERTIES C_STANDARD 99)
add_test(NAME test_achievements_owner COMMAND test_achievements_owner)
add_executable(test_achievements_lifecycle tests/test_achievements_lifecycle.c)
target_link_libraries(test_achievements_lifecycle cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievements_lifecycle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievements_lifecycle PROPERTIES C_STANDARD 99)
add_test(NAME test_achievements_lifecycle COMMAND test_achievements_lifecycle)
if(Python3_Interpreter_FOUND)
add_executable(test_achievements_r2 tests/test_achievements_r2.c)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/achievements_main_slices.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/slice_achievements_main.py
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/achievements_main_slices.h
    DEPENDS source/main.c source/game/gui/screen_stats_save.h tests/slice_achievements_main.py
    VERBATIM)
target_sources(test_achievements_r2 PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/achievements_main_slices.h)
target_include_directories(test_achievements_r2 PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(test_achievements_r2 cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
    ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievements_r2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievements_r2 PROPERTIES C_STANDARD 99)
target_link_options(test_achievements_r2 PRIVATE
    -Wl,--wrap=stats_profile_save -Wl,--wrap=stats_profile_load -Wl,--wrap=stats_save_step
    -Wl,--wrap=input_poll -Wl,--wrap=input_player_frame
    -Wl,--wrap=gfx_width -Wl,--wrap=gfx_height -Wl,--wrap=gfx_clear_buffers
    -Wl,--wrap=gfx_mode_gui -Wl,--wrap=gfx_finish -Wl,--wrap=watchdog_heartbeat
    -Wl,--wrap=audio_menu_event -Wl,--wrap=gfx_texture -Wl,--wrap=gfx_bind_texture
    -Wl,--wrap=gutil_texquad_col -Wl,--wrap=gutil_texquad_rt_any
    -Wl,--wrap=gutil_font_width -Wl,--wrap=gutil_text -Wl,--wrap=gutil_button_draw
    -Wl,--wrap=gutil_prompt_glyph -Wl,--wrap=gutil_hint_row_draw -Wl,--wrap=hw_telemetry_emit_observer
    -Wl,--wrap=achievements_shutdown)
add_test(NAME test_achievements_r2 COMMAND test_achievements_r2)
set_tests_properties(test_achievements_r2 PROPERTIES TIMEOUT 30)
endif()
add_executable(test_achievements_handlers tests/test_achievements_handlers.c)
target_link_libraries(test_achievements_handlers cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievements_handlers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievements_handlers PROPERTIES C_STANDARD 99)
target_link_options(test_achievements_handlers PRIVATE -Wl,--wrap=stats_profile_load -Wl,--wrap=stats_profile_save -Wl,--wrap=stats_save_step)
add_test(NAME test_achievements_handlers COMMAND test_achievements_handlers)
add_test(NAME test_achievements_handlers__shutdown COMMAND test_achievements_handlers shutdown)
add_executable(test_achievements_seam tests/test_achievements_seam.c)
target_link_libraries(test_achievements_seam cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievements_seam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievements_seam PROPERTIES C_STANDARD 99)
add_test(NAME test_achievements_seam COMMAND test_achievements_seam)
# Reads tests/golden/achievements/b3_alias_table.txt + b4_item_registration.txt (the
# reference model of the seam test) by repo-relative path, like test_achievements_golden.
set_tests_properties(test_achievements_seam PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
# LCE-01 lane B B4 (2026-09-14): the public notice / screen-data / status API.
# Design 7 (owner-explicit rows, per-owner notices) and 1.6 (what the two
# screens draw): notice isolation and ack, the grid row's fields and its three
# shading states, Beta's three value formatters against fixed strings, empty
# rows for an unbound or non-existent slot, and a status that reports an
# injected save failure and an event rejection.
add_executable(test_achievements_api tests/test_achievements_api.c)
target_link_libraries(test_achievements_api cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievements_api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievements_api PROPERTIES C_STANDARD 99)
add_test(NAME test_achievements_api COMMAND test_achievements_api)
add_executable(test_achievements_notice tests/test_achievements_notice.c)
target_link_libraries(test_achievements_notice cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievements_notice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievements_notice PROPERTIES C_STANDARD 99)
add_test(NAME test_achievements_notice COMMAND test_achievements_notice)
set_tests_properties(test_achievements_notice PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(test_achievements_notice PRIVATE
	-Wl,--wrap=gfx_texture -Wl,--wrap=gfx_bind_texture -Wl,--wrap=gfx_draw_quads
	-Wl,--wrap=gutil_draw_item_sized -Wl,--wrap=gutil_text_rgb -Wl,--wrap=gutil_prompt_glyph
	-Wl,--wrap=audio_achievement_bell -Wl,--wrap=tex_gfx_load -Wl,--wrap=tex_gfx_clear
	-Wl,--wrap=tex_gfx_lookup -Wl,--wrap=gutil_font_width)
add_executable(test_achievement_bell tests/test_achievements_notice.c source/platform/time.c)
target_include_directories(test_achievement_bell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(test_achievement_bell PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED ACHIEVEMENT_BELL_TEST)
target_link_libraries(test_achievement_bell m)
add_test(NAME test_achievement_bell COMMAND test_achievement_bell)
add_executable(test_achievements_screen tests/test_achievements_screen.c)
target_link_libraries(test_achievements_screen cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_achievements_screen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_achievements_screen PROPERTIES C_STANDARD 99)
target_link_options(test_achievements_screen PRIVATE
	-Wl,--wrap=gfx_texture -Wl,--wrap=gfx_bind_texture -Wl,--wrap=gfx_draw_quads
	-Wl,--wrap=gfx_draw_quads_flt
	-Wl,--wrap=gutil_draw_item_sized -Wl,--wrap=gutil_text_rgb -Wl,--wrap=gutil_prompt_glyph
	-Wl,--wrap=audio_menu_event)
add_test(NAME test_achievements_screen COMMAND test_achievements_screen)

# LCE-02 lane B B3 (2026-09-14): the STATISTICS half of the module.
# test_stats_counters pins every general stat id to the event that feeds it
# and to its column, Beta's three rounding chains (stats_beta.c) against
# hand-computed values incl. the fly > 25 / fall >= 2.0 boundaries and a value
# where the float and double chains disagree, craft += stackSize, a no-stat
# harvest, saturation, and the FOUR-ARRAY alias table derived from
# StatList.replaceAllSimilarBlocks against its golden file.
add_executable(test_stats_counters tests/test_stats_counters.c)
target_link_libraries(test_stats_counters cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_stats_counters PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_stats_counters PROPERTIES C_STANDARD 99)
add_test(NAME test_stats_counters COMMAND test_stats_counters)
# Reads tests/golden/achievements/b3_alias_table.txt by repo-relative path,
# the same rule test_achievements_golden uses.
set_tests_properties(test_stats_counters PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# test_stats_provenance walks every row of the design 5.2 acceptance matrix
# (both bind modes x three authorities x both Beta stat classes) as its own
# case, plus the func_27027_b rejection of a client-owned id over Packet 200,
# an achievement id over Packet 200 through the parent check, and the
# local/remote/total columns read back through the B4 API.
add_executable(test_stats_provenance tests/test_stats_provenance.c)
target_link_libraries(test_stats_provenance cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_stats_provenance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_stats_provenance PROPERTIES C_STANDARD 99)
add_test(NAME test_stats_provenance COMMAND test_stats_provenance)

add_executable(test_column_dedup_lifetime tests/test_column_dedup_lifetime.c)
target_link_libraries(test_column_dedup_lifetime cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
    ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_column_dedup_lifetime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_column_dedup_lifetime PROPERTIES C_STANDARD 99)
add_test(NAME test_column_dedup_lifetime COMMAND test_column_dedup_lifetime)

add_executable(test_beta_import_lighting tests/test_beta_import_lighting.c)
target_link_libraries(test_beta_import_lighting cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
    ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_beta_import_lighting PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_beta_import_lighting PROPERTIES C_STANDARD 99)
add_test(NAME test_beta_import_lighting COMMAND test_beta_import_lighting "${CMAKE_SOURCE_DIR}/tests/fixtures/beta_import")

add_executable(test_lighting_local_publication_equiv
	tests/test_lighting_local_publication_equiv.c)
target_link_libraries(test_lighting_local_publication_equiv
	cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_lighting_local_publication_equiv PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_lighting_local_publication_equiv PROPERTIES C_STANDARD 99)
target_link_options(test_lighting_local_publication_equiv PRIVATE "-Wl,--wrap=mem2_alloc")
add_test(NAME test_lighting_local_publication_equiv
	COMMAND test_lighting_local_publication_equiv)
# R17 S3: terrain FIFO head baseline witness (real local fixture, real handlers).
# --admission (unregistered) is the retained RED receipt; host only.
add_executable(test_g9_fifo_bound tests/test_g9_fifo_bound.c)
target_link_libraries(test_g9_fifo_bound
	cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_g9_fifo_bound PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party
	${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_g9_fifo_bound PROPERTIES C_STANDARD 99)
target_link_options(test_g9_fifo_bound PRIVATE "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=mem2_alloc,--wrap=mem2_alloc_pooled")
add_test(NAME test_g9_fifo_bound COMMAND test_g9_fifo_bound)
set_tests_properties(test_g9_fifo_bound PROPERTIES TIMEOUT 60)


foreach(kind mutex scene_equiv session overlay pressure recovery)
    add_executable(test_column_dedup_${kind} tests/test_column_dedup_${kind}.c)
    target_link_libraries(test_column_dedup_${kind} cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
        ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
    target_include_directories(test_column_dedup_${kind} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    set_target_properties(test_column_dedup_${kind} PROPERTIES C_STANDARD 99)
    add_test(NAME test_column_dedup_${kind} COMMAND test_column_dedup_${kind})
    set_tests_properties(test_column_dedup_${kind} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 120)
endforeach()
# colcap-1006 (ASTRA-CC-R4 points 2-3): column-slot placement equivalence.
# CW_TEST_COLUMN_POOL routes the PC server's column arrays through a host copy
# of the Wii column pool (mem2.c) with the named placement; the goldens and
# digests these tests check were recorded with the unchanged allocator, so a
# pass under "lowest" (and a cross-policy digest match) proves world/block/
# meta/light/entity and mesh bytes do not depend on slot placement.
# CW_TEST_COLUMN_POOL_REQUIRE fails a run that never reached the pool.
set(_cw_placement_env "CW_TEST_COLUMN_POOL=lowest;CW_TEST_COLUMN_POOL_REQUIRE=1")
add_test(NAME test_column_placement_regrow_route COMMAND test_split_column_cap regrow_route)
set_tests_properties(test_column_placement_regrow_route PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 600
    ENVIRONMENT "${_cw_placement_env}")
add_test(NAME test_column_placement_scene_equiv_lowest COMMAND test_column_dedup_scene_equiv)
set_tests_properties(test_column_placement_scene_equiv_lowest PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 120
    ENVIRONMENT "${_cw_placement_env}")
add_test(NAME test_column_placement_a1c_world_ops_lowest COMMAND test_a1c_world_ops_equiv)
set_tests_properties(test_column_placement_a1c_world_ops_lowest PROPERTIES
    TIMEOUT 300 ENVIRONMENT "${_cw_placement_env}")
add_test(NAME test_column_placement_sep256_lowest COMMAND test_split_column_cap sep256)
set_tests_properties(test_column_placement_sep256_lowest PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 600
    ENVIRONMENT "${_cw_placement_env}")
# Cross-policy digest: the pin leg records the Teleporter-landing world digest
# under the ROTATING placement, the near leg recomputes it under LOWEST.
add_test(NAME test_column_placement_parity_pin_rotating
    COMMAND test_split_column_cap teleporter_parity_pin)
set_tests_properties(test_column_placement_parity_pin_rotating PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 600
    ENVIRONMENT "CW_TEST_COLUMN_POOL=rotating;CW_TEST_COLUMN_POOL_REQUIRE=1;CW_PARITY_FILE_TAG=_placement"
    FIXTURES_SETUP colcap_placement_parity RESOURCE_LOCK colcap_placement_parity)
add_test(NAME test_column_placement_parity_near_lowest
    COMMAND test_split_column_cap teleporter_parity_near)
set_tests_properties(test_column_placement_parity_near_lowest PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 600
    ENVIRONMENT "${_cw_placement_env};CW_PARITY_FILE_TAG=_placement"
    FIXTURES_REQUIRED colcap_placement_parity RESOURCE_LOCK colcap_placement_parity)
target_link_options(test_column_dedup_mutex PRIVATE
    -Wl,--wrap=thread_mutex_init -Wl,--wrap=thread_mutex_destroy)
target_link_options(test_column_dedup_pressure PRIVATE -Wl,--wrap=mem2_alloc -Wl,--wrap=mem2_free_sized)
target_link_options(test_column_dedup_recovery PRIVATE -Wl,--wrap=mem2_alloc)
target_sources(test_column_dedup_overlay PRIVATE tests/support/column_dedup_prefix.c)
target_link_options(test_column_dedup_overlay PRIVATE
    -Wl,--wrap=world_build_full_section_leased -Wl,--wrap=mem2_alloc)
target_sources(test_column_dedup_session PRIVATE
    source/network/mp_codec.c source/network/mp_session.c
    source/network/server_remote.c source/network/server_remote_transport.c
    source/network/server_remote_protocol.c source/network/server_remote_entities.c
    source/network/server_remote_chunks.c source/platform/net_socket.c)
target_link_options(test_column_dedup_session PRIVATE
    -Wl,--wrap=gfx_wait_idle
    -Wl,--wrap=column_backing_ref -Wl,--wrap=column_backing_unref
    -Wl,--wrap=column_backing_references -Wl,--wrap=column_backing_matches)
if(WIN32)
    target_link_libraries(test_column_dedup_session ws2_32)
endif()

# Same gather harness, test engine for equivalence and production engine for
# measurement. Link wrapping measures the real worker's thread CPU cost.
foreach(target test_chunk_gather_equiv bench_chunk_gather)
    add_executable(${target} tests/bench_chunk_gather.c)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    target_link_libraries(${target}
        ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
    target_link_options(${target} PRIVATE -Wl,--wrap=thread_create
        -Wl,--wrap=thread_mutex_lock -Wl,--wrap=thread_mutex_unlock
        -Wl,--wrap=thread_stripe_lock -Wl,--wrap=thread_stripe_unlock -Wl,--wrap=chunk_set_light)
    set_target_properties(${target} PROPERTIES C_STANDARD 99)
endforeach()
target_link_libraries(test_chunk_gather_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_link_libraries(bench_chunk_gather cavex_prod_core cavex_prod_input)
target_compile_options(bench_chunk_gather PRIVATE -DNDEBUG)
add_test(NAME test_chunk_gather_equiv COMMAND test_chunk_gather_equiv)

include(cmake/check_column_dedup_goldens.cmake)
foreach(kind state render)
    add_executable(test_column_dedup_${kind}_equiv
        tests/test_column_dedup_${kind}_equiv.c tests/support/column_dedup_walk.c)
    target_link_libraries(test_column_dedup_${kind}_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
        ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
    target_include_directories(test_column_dedup_${kind}_equiv PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    set_target_properties(test_column_dedup_${kind}_equiv PROPERTIES C_STANDARD 99)
    add_test(NAME test_column_dedup_${kind}_equiv COMMAND test_column_dedup_${kind}_equiv)
    set_tests_properties(test_column_dedup_${kind}_equiv PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 1200)
endforeach()

add_executable(test_respawn_residency tests/test_respawn_residency_capacity.c)
target_link_libraries(test_respawn_residency cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_respawn_residency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_respawn_residency PROPERTIES C_STANDARD 99)
add_test(NAME test_respawn_residency COMMAND test_respawn_residency)
set_tests_properties(test_respawn_residency PROPERTIES TIMEOUT 120)

# stage22-0927: the real-Wii save_fail stage 22 (ENTITY_RETIRE) through the
# production tick -- a near eviction meeting a full client RPC pool must be a
# next-tick deferral, not a save failure + 20-tick stall
# (tmp/jobs/split-dimensions-plan-0924/out/SAVE-STAGE22-RCA.md).
# G9 R21: local-only block-update packing == unpacked through the REAL consumer (3 self-spawned arms).
add_executable(test_rpc_block_pack_equiv tests/test_rpc_block_pack_equiv.c)
target_link_libraries(test_rpc_block_pack_equiv cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_rpc_block_pack_equiv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_rpc_block_pack_equiv PROPERTIES C_STANDARD 99)
add_test(NAME test_rpc_block_pack_equiv COMMAND test_rpc_block_pack_equiv)
set_tests_properties(test_rpc_block_pack_equiv PROPERTIES TIMEOUT 900)

# BUG-TICK (R10) hw1030a: the server tick never waits on a full client RPC pool
add_executable(test_rpc_pool_burst tests/test_rpc_pool_burst.c)
target_link_libraries(test_rpc_pool_burst cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_rpc_pool_burst PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_rpc_pool_burst PROPERTIES C_STANDARD 99)
add_test(NAME test_rpc_pool_burst COMMAND test_rpc_pool_burst)
set_tests_properties(test_rpc_pool_burst PROPERTIES TIMEOUT 600)

add_executable(test_evict_retire_pool_full tests/test_evict_retire_pool_full.c)
target_link_libraries(test_evict_retire_pool_full cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_evict_retire_pool_full PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_target_properties(test_evict_retire_pool_full PROPERTIES C_STANDARD 99)
add_test(NAME test_evict_retire_pool_full COMMAND test_evict_retire_pool_full)
set_tests_properties(test_evict_retire_pool_full PROPERTIES TIMEOUT 120)

# Beta Entity.handleLavaMovement / World.isMaterialInBB shared AABB scan.
add_executable(test_lava_aabb_golden tests/test_lava_aabb_golden.c)
target_link_libraries(test_lava_aabb_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_lava_aabb_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_lava_aabb_golden PROPERTIES C_STANDARD 99)
add_test(NAME test_lava_aabb_golden COMMAND test_lava_aabb_golden)

# next-gaps-0924/L1: host/guest player material scans over unloaded chunks follow Beta server semantics (Astra ruling S).
add_executable(test_player_residency_scan_char tests/test_player_residency_scan_char.c)
target_link_libraries(test_player_residency_scan_char cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_player_residency_scan_char PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_player_residency_scan_char PROPERTIES C_STANDARD 99)
add_test(NAME test_player_residency_scan_char COMMAND test_player_residency_scan_char)
# next-gaps-0924/X2: the Giant box = Beta EntityGiantZombie setSize(0.6F*6.0F, 1.8F*6.0F) float bits,
# pinned by cell-boundary goldens on the collision AABB and the contracted lava AABB scan.
add_executable(test_giant_box_golden tests/test_giant_box_golden.c)
target_link_libraries(test_giant_box_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_giant_box_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_giant_box_golden PROPERTIES C_STANDARD 99)
add_test(NAME test_giant_box_golden COMMAND test_giant_box_golden)

add_executable(test_mob_lava_demo tests/test_mob_lava_demo.c)
target_link_libraries(test_mob_lava_demo cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_mob_lava_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_mob_lava_demo PROPERTIES C_STANDARD 99)
add_test(NAME test_mob_lava_demo COMMAND test_mob_lava_demo)

add_executable(test_mob_lava_aabb_golden tests/test_mob_lava_aabb_golden.c)
target_link_libraries(test_mob_lava_aabb_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_mob_lava_aabb_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_mob_lava_aabb_golden PROPERTIES C_STANDARD 99)
add_test(NAME test_mob_lava_aabb_golden COMMAND test_mob_lava_aabb_golden)

add_executable(test_entity_fire_billboard_golden tests/test_entity_fire_billboard_golden.c)
target_link_libraries(test_entity_fire_billboard_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_entity_fire_billboard_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_entity_fire_billboard_golden PROPERTIES C_STANDARD 99)
add_test(NAME test_entity_fire_billboard_golden COMMAND test_entity_fire_billboard_golden)

# lava-aabb-0924/B3: the split partner body's on_fire reaches the other
# viewport every tick through a local-only CRPC (host untagged, guest through
# the generation-tagged guest route), clears without latching, and the draw is
# gated on the corpse-retire condition. Runs the REAL server tick on the split
# residency fixture with a real guest admission.
add_executable(test_split_partner_fire tests/test_split_partner_fire.c)
target_link_libraries(test_split_partner_fire cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_partner_fire PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_partner_fire PROPERTIES C_STANDARD 99)
add_test(NAME test_split_partner_fire COMMAND test_split_partner_fire)
set_tests_properties(test_split_partner_fire PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)

# split-dimensions-plan-0924/C2a: owner-swapped movement policy for the local
# split-screen guest body -- P2 inventory rows #4 (dead/sleep/respawn-plan pose
# barriers, Beta EntityPlayer.isMovementBlocked), #25 (knockback: shared server
# integrator, documented zero-velocity client envelope) and #26 (teleport-scale
# sample: host accepts, guest refuses + corrects) plus the respawn
# ticks_existed reset (Entity.java:51). ONE executable on the split residency
# fixture with a real guest admission; argv[1] selects the case, so each of the
# three ledger names is its own ctest entry (and its own process/fixture).
add_executable(test_owner_swap_pose_dead_sleep_barrier tests/test_owner_swap_pose_dead_sleep_barrier.c)
target_link_libraries(test_owner_swap_pose_dead_sleep_barrier cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_owner_swap_pose_dead_sleep_barrier PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_owner_swap_pose_dead_sleep_barrier PROPERTIES C_STANDARD 99)
add_test(NAME test_owner_swap_pose_dead_sleep_barrier COMMAND test_owner_swap_pose_dead_sleep_barrier barrier)
add_test(NAME test_owner_swap_knockback_fall_momentum COMMAND test_owner_swap_pose_dead_sleep_barrier knockback)
add_test(NAME test_owner_swap_large_pose_delta_policy COMMAND test_owner_swap_pose_dead_sleep_barrier delta)
set_tests_properties(test_owner_swap_pose_dead_sleep_barrier
	test_owner_swap_knockback_fall_momentum
	test_owner_swap_large_pose_delta_policy PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)

# residency-1005: player residency admission (ASTRA-1005 / ASTRA-1005B rulings).
# Real host + guest movement handlers on the split fixture; argv[1] selects the
# case, one fixture per process.
add_executable(test_player_residency_admission tests/test_player_residency_admission.c)
target_link_libraries(test_player_residency_admission cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_player_residency_admission PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_player_residency_admission PROPERTIES C_STANDARD 99)
foreach(_rz_case host guest unknown equiv latency loadahead sources portal feedback lean bounded lifecycle rejoin toasts evictors saturation
		held_controls client_retry wake telemetry_lifecycle runtime_fault
		vehicles knockback bounded_slots lifecycle_missing
		lifecycle_initial_load lifecycle_saved_join lifecycle_nether_respawn
		vehicles_guest_away vehicles_host_away
		lifecycle_nether_respawn_far lifecycle_saved_join_bounded)
	add_test(NAME test_player_residency_admission_${_rz_case} COMMAND test_player_residency_admission ${_rz_case})
	set_tests_properties(test_player_residency_admission_${_rz_case} PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
endforeach()
# residency-1005/2 round 3 item 6: an AUTHENTIC heartbeat tape (real handlers,
# real formatter) and the collector's per-actor residency gate over it, plus
# every single-fault mutation of the same tape (must FAIL/UNPROVEN).
add_test(NAME test_player_residency_admission_tape
	COMMAND test_player_residency_admission tape
		${CMAKE_CURRENT_BINARY_DIR}/residency_tape_pc.ndjson)
set_tests_properties(test_player_residency_admission_tape PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300
	FIXTURES_SETUP residency_tape)
if(Python3_Interpreter_FOUND)
	add_test(NAME test_residency_gate_tape COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tools/wii_telemetry_server.py
		--residency-gate-tape ${CMAKE_CURRENT_BINARY_DIR}/residency_tape_pc.ndjson
		--residency-gate-negatives)
	set_tests_properties(test_residency_gate_tape PROPERTIES
		TIMEOUT 120 FIXTURES_REQUIRED residency_tape)
	# residency-1005/4 closure items 9-11: the fail-closed, segment-graded,
	# requirement/induced-scoped collector gate over raw NDJSON, through the
	# tape CLI AND the live Collector (identical verdicts), plus the
	# run_wii_hardware.ps1 forwarding.
	add_test(NAME test_residency_gate COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_residency_gate.py)
	set_tests_properties(test_residency_gate PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 900)
endif()

# split-dimensions-plan-0924/C2b: no lost partner-fire update after a REFUSED
# first guest body install. Drives the real admission on the split residency
# fixture with the first LOCAL_GUEST_GAMEPLAY_READY install refused through the
# entity-slot seam, a real tick that publishes both fire states while the guest
# has no client body, then the idempotent retry: both partner copies must show
# the burn the server already recorded as sent.
add_executable(test_split_partner_fire_refused_install tests/test_split_partner_fire_refused_install.c)
target_link_libraries(test_split_partner_fire_refused_install cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_partner_fire_refused_install PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_partner_fire_refused_install PROPERTIES C_STANDARD 99)
add_test(NAME test_split_partner_fire_refused_install COMMAND test_split_partner_fire_refused_install)
set_tests_properties(test_split_partner_fire_refused_install PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)

# admit-retry-0925: real-handler contracts for first-body recovery (step 1 RED;
# step 2 implemented the Astra retire-vs-retry ruling, all seven GREEN).
add_executable(test_split_admission_recovery tests/test_split_admission_recovery.c)
target_link_libraries(test_split_admission_recovery cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_split_admission_recovery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_split_admission_recovery PROPERTIES C_STANDARD 99)
# Astra review A (send-back of 81e71dad) added the six boundary/visibility cases.
foreach(_cw_admit_case mixed collision exhaustion overflow stale leave_reset reset
		retry_boundary retry_last overflow_boundary timeout notices cleanup_retry
		notices_single hud_overflow)
	add_test(NAME test_split_admission_recovery_${_cw_admit_case}
		COMMAND test_split_admission_recovery ${_cw_admit_case})
endforeach()
set_tests_properties(
	test_split_admission_recovery_mixed test_split_admission_recovery_collision
	test_split_admission_recovery_exhaustion test_split_admission_recovery_overflow
	test_split_admission_recovery_stale test_split_admission_recovery_leave_reset
	test_split_admission_recovery_reset
	test_split_admission_recovery_retry_boundary test_split_admission_recovery_retry_last
	test_split_admission_recovery_overflow_boundary test_split_admission_recovery_timeout
	test_split_admission_recovery_notices test_split_admission_recovery_cleanup_retry
	test_split_admission_recovery_notices_single test_split_admission_recovery_hud_overflow
	PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)

# split-dimensions-plan-0924/A1d step 1: isolated RED owner-swapped tests on
# A1a's contracts (P2 inventory rows #3, #5, #6, #7, #8, #16 and C2 row #27).
# Every executable drives the REAL guest admission on the split residency
# fixture through tests/support/owner_swap_harness.h (the C2a harness) and
# argv[1] selects ONE case per process. The `base` / `reload` cases are the
# owner-0 reference legs (and the guest legs both owners already share): they
# pass on the base and are ordinary tests. The `red` / `visible` / `nether`
# cases are the owner-swapped guest legs that the A1d wiring (after G1) and A2
# (the second live world) must turn GREEN: registered WILL_FAIL until then, so
# the suite stays 100% while the RED is on record (a RED case exits 1 through
# its check() ledger, never aborts). Beta citations sit in each file header.
# A1d steps 2-5 (after G1) turned GREEN and un-WILL_FAILed: window_state_barriers
# _red (#5), bed_sleep_wake_spawn_red (#3), walk_step_trample_sound_red (#16)
# and dimension_refusal_visible (#27 interim).
# Still WILL_FAIL, named with their blockers (out/A1d-result.md):
#   minecart_mount_control_red -- cart occupancy has no rider identity
#     (entity_minecart / server_set_minecart_occupancy_impl / the host-only
#     RIDE_INTENT + CONTROL handlers are outside lane C's ranges).
# dimension_nether_admit is GREEN with the A2-T saved-dimension admission
# (a2t-guest-0927, Astra a2t-0927); refusal_visible now pins the over-budget
# admission refusal's toast.
# R2 (2026-10-08): boat/pig admission cases pass directly, without inversion.
set(_cw_owner_swap_still_red minecart_mount_control)
foreach(_cw_owner_swap
	window_state_barriers bed_sleep_wake_spawn boat_ride_steer pig_mount
	minecart_mount_control walk_step_trample_sound)
	add_executable(test_owner_swap_${_cw_owner_swap} tests/test_owner_swap_${_cw_owner_swap}.c)
	target_link_libraries(test_owner_swap_${_cw_owner_swap} cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
		${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
	target_include_directories(test_owner_swap_${_cw_owner_swap} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
	set_target_properties(test_owner_swap_${_cw_owner_swap} PROPERTIES C_STANDARD 99)
	add_test(NAME test_owner_swap_${_cw_owner_swap} COMMAND test_owner_swap_${_cw_owner_swap} base)
	add_test(NAME test_owner_swap_${_cw_owner_swap}_red COMMAND test_owner_swap_${_cw_owner_swap} red)
	set_tests_properties(test_owner_swap_${_cw_owner_swap} PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
	if(_cw_owner_swap IN_LIST _cw_owner_swap_still_red)
		set_tests_properties(test_owner_swap_${_cw_owner_swap}_red PROPERTIES
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300 WILL_FAIL TRUE)
	else()
		set_tests_properties(test_owner_swap_${_cw_owner_swap}_red PROPERTIES
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
	endif()
endforeach()
# A1d review C1: the committed asserts for the A1d sleep additions (occupied,
# sleeping-guest checkpoint, damage wakes only the hit body).
add_test(NAME test_owner_swap_bed_sleep_wake_spawn_wake COMMAND test_owner_swap_bed_sleep_wake_spawn wake)
set_tests_properties(test_owner_swap_bed_sleep_wake_spawn_wake PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
add_executable(test_owner_swap_dimension_checkpoint_reload tests/test_owner_swap_dimension_checkpoint_reload.c)
target_link_libraries(test_owner_swap_dimension_checkpoint_reload cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_owner_swap_dimension_checkpoint_reload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_owner_swap_dimension_checkpoint_reload PROPERTIES C_STANDARD 99)
add_test(NAME test_owner_swap_dimension_checkpoint_reload COMMAND test_owner_swap_dimension_checkpoint_reload reload)
add_test(NAME test_owner_swap_dimension_refusal_visible COMMAND test_owner_swap_dimension_checkpoint_reload visible)
add_test(NAME test_owner_swap_dimension_nether_admit COMMAND test_owner_swap_dimension_checkpoint_reload nether)
set_tests_properties(test_owner_swap_dimension_checkpoint_reload PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
set_tests_properties(test_owner_swap_dimension_refusal_visible PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
set_tests_properties(test_owner_swap_dimension_nether_admit PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
# lane C #2: lightning strike effects on both actors, only in the strike's
# world; Nether suppression (EntityLightningBolt.java:62-69, Entity.java:
# 1044-1051, World.java:1787). One executable, argv[1] selects the case.
add_executable(test_owner_swap_lightning tests/test_owner_swap_lightning.c)
target_link_libraries(test_owner_swap_lightning cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_owner_swap_lightning PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_owner_swap_lightning PROPERTIES C_STANDARD 99)
foreach(_cw_c2lcase guest_struck host_struck both_struck other_world nether_quiet
		bbox victims guest_ring)
	add_test(NAME test_owner_swap_lightning_${_cw_c2lcase}
		COMMAND test_owner_swap_lightning ${_cw_c2lcase})
	set_tests_properties(test_owner_swap_lightning_${_cw_c2lcase} PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
endforeach()
# lane C painting-bbox-0927: a painting's Beta Entity.boundingBox (EntityPainting.
# setDirection :123) reaches it for explosions (Explosion.doExplosionA :77-105) and
# lightning (EntityLightningBolt :62-69 -> Entity.dealFireDamage :521-525); either
# breaks it with one Item.painting (EntityPainting.attackEntityFrom :200-208).
add_executable(test_painting_hit tests/test_painting_hit.c)
target_link_libraries(test_painting_hit cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_painting_hit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_painting_hit PROPERTIES C_STANDARD 99)
foreach(_cw_phcase box lightning explosion)
	add_test(NAME test_painting_hit_${_cw_phcase} COMMAND test_painting_hit ${_cw_phcase})
	set_tests_properties(test_painting_hit_${_cw_phcase} PROPERTIES TIMEOUT 240)
endforeach()
# lane C motion-units-0927 S1 (docs/intent/mob-motion-units.md): the authoritative
# double mob motion state + Entity.moveEntity double port + mutation API, inactive.
add_executable(test_mob_motion tests/test_mob_motion.c)
target_link_libraries(test_mob_motion cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_mob_motion PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_mob_motion PROPERTIES C_STANDARD 99)
foreach(_cw_mmcase aabb fall precision wall step web seed mirror live_tick blast_route push_route size)
	add_test(NAME test_mob_motion_${_cw_mmcase} COMMAND test_mob_motion ${_cw_mmcase})
	set_tests_properties(test_mob_motion_${_cw_mmcase} PROPERTIES TIMEOUT 240)
endforeach()
# S2: the routing handoff is carried on the branch (and in MOTION-S2-handoff.patch),
# so blast_route / push_route pass and are enforced (WILL_FAIL dropped).
# lane C motion-units-0927 S2: per-tick mob trajectory goldens through real
# production ticks vs Beta's own arithmetic (EntityLiving.moveEntityWithHeading /
# jump / knockBack, Entity.moveEntity, Explosion.doExplosionA order).
add_executable(test_mob_trajectory_golden tests/test_mob_trajectory_golden.c)
target_link_libraries(test_mob_trajectory_golden cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_mob_trajectory_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_mob_trajectory_golden PROPERTIES C_STANDARD 99)
foreach(_cw_mtcase fall tnt_up tnt_down creeper_up repeated jump death_fall death_slide)
	add_test(NAME test_mob_trajectory_golden_${_cw_mtcase} COMMAND test_mob_trajectory_golden ${_cw_mtcase})
	set_tests_properties(test_mob_trajectory_golden_${_cw_mtcase} PROPERTIES TIMEOUT 240)
endforeach()
# lane C motion-units-0927 S2 (Q3, Fable S1 F2): every mob hit site composes
# EntityLiving.knockBack at the hit from the attacker, byte-exact on the doubles.
add_executable(test_mob_knockback_sites tests/test_mob_knockback_sites.c)
target_link_libraries(test_mob_knockback_sites cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_mob_knockback_sites PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_mob_knockback_sites PROPERTIES C_STANDARD 99)
foreach(_cw_kbcase melee arrow egg fish)
	add_test(NAME test_mob_knockback_sites_${_cw_kbcase} COMMAND test_mob_knockback_sites ${_cw_kbcase})
	set_tests_properties(test_mob_knockback_sites_${_cw_kbcase} PROPERTIES TIMEOUT 240)
endforeach()
# PIGZ-PACK: Beta EntityPigZombie.attackEntityFrom pack anger on every
# player-attributed hit (melee alive/killing/dying, arrow, egg, snowball, fish),
# Beta box membership, own-world only, each member's own Random.
add_executable(test_pig_zombie_pack_anger tests/test_pig_zombie_pack_anger.c)
target_link_libraries(test_pig_zombie_pack_anger cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_pig_zombie_pack_anger PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_pig_zombie_pack_anger PROPERTIES C_STANDARD 99)
add_test(NAME test_pig_zombie_pack_anger COMMAND test_pig_zombie_pack_anger)
set_tests_properties(test_pig_zombie_pack_anger PROPERTIES TIMEOUT 240)
# lane C #20: wolf interest selects an eligible actor of the wolf's own world
# (EntityLiving.updatePlayerActionState :695-710, EntityWolf.onLivingUpdate
# :115-130, World.getClosestPlayer :2166-2180). argv[1] selects the case.
add_executable(test_owner_swap_wolf_interest tests/test_owner_swap_wolf_interest.c)
target_link_libraries(test_owner_swap_wolf_interest cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_owner_swap_wolf_interest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_owner_swap_wolf_interest PROPERTIES C_STANDARD 99)
foreach(_cw_c20case guest_bone host_bone tie_rng no_actor)
	add_test(NAME test_owner_swap_wolf_interest_${_cw_c20case}
		COMMAND test_owner_swap_wolf_interest ${_cw_c20case})
	set_tests_properties(test_owner_swap_wolf_interest_${_cw_c20case} PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
endforeach()
# C2 tiles (lane C, P2 rows #9/#10/#12/#13): either actor opens the tile window
# of the world it clicked in; removal invalidates only that world's window and
# conserves the contents; a stale-bound window refuses clicks. Beta citations
# in the file header. One executable, argv[1] selects the case.
# WILL_FAIL until handoff v3 (tmp/jobs/split-dimensions-plan-0924/out/
# C2tiles-handoff-v3.md) lands and lifts the guest furnace refusal: the
# click-handler refresh before `returned` (A-H1/A-M1), the per-body smelt push,
# the world-bound scatter clear (cross_world) and the guest deferred close.
set(_cw_c2tiles_red)
add_executable(test_owner_swap_tile_windows tests/test_owner_swap_tile_windows.c)
target_link_libraries(test_owner_swap_tile_windows cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_owner_swap_tile_windows PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_owner_swap_tile_windows PROPERTIES C_STANDARD 99)
foreach(_cw_c2case furnace_open dispenser_open chest_break furnace_break
		dispenser_break cross_world stale stale_guest furnace_two_viewers
		dispenser_two_viewers stale_close furnace_quick_move_stale
		furnace_stale_transaction)
	add_test(NAME test_owner_swap_tile_windows_${_cw_c2case}
		COMMAND test_owner_swap_tile_windows ${_cw_c2case})
	if(_cw_c2case IN_LIST _cw_c2tiles_red)
		set_tests_properties(test_owner_swap_tile_windows_${_cw_c2case} PROPERTIES
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300 WILL_FAIL TRUE)
	else()
		set_tests_properties(test_owner_swap_tile_windows_${_cw_c2case} PROPERTIES
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
	endif()
endforeach()
# lane C #19: the nearest eligible actor of the mob's own world governs
# monster despawn (EntityLiving.func_27021_X :667-685, World.getClosestPlayer
# :2166-2180). One executable, argv[1] selects the case.
add_executable(test_owner_swap_monster_despawn tests/test_owner_swap_monster_despawn.c)
target_link_libraries(test_owner_swap_monster_despawn cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_owner_swap_monster_despawn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_owner_swap_monster_despawn PROPERTIES C_STANDARD 99)
foreach(_cw_c19case guest_near host_near both_far tie_rng no_actor guest_no_pos world_local_acquire no_local_acquire world_local_tie world_local_eye_distance ghast_world_local)
	add_test(NAME test_owner_swap_monster_despawn_${_cw_c19case}
		COMMAND test_owner_swap_monster_despawn ${_cw_c19case})
	set_tests_properties(test_owner_swap_monster_despawn_${_cw_c19case} PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
endforeach()
# G8: a blast in either live world damages only its committed actor, even
# when the other world's actor occupies the same coordinates.
add_executable(test_a2w_blast_isolation tests/test_a2w_blast_isolation.c)
target_link_libraries(test_a2w_blast_isolation cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_a2w_blast_isolation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_a2w_blast_isolation PROPERTIES C_STANDARD 99)
foreach(_cw_a2w_blast_case host_nether guest_nether)
	add_test(NAME test_a2w_blast_isolation_${_cw_a2w_blast_case}
		COMMAND test_a2w_blast_isolation ${_cw_a2w_blast_case})
	set_tests_properties(test_a2w_blast_isolation_${_cw_a2w_blast_case} PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
endforeach()
# G8: a committed actor's eye hazard is read from that actor's own world.
add_executable(test_a2w_actor_hazard_world tests/test_a2w_actor_hazard_world.c)
target_link_libraries(test_a2w_actor_hazard_world cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_a2w_actor_hazard_world PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_a2w_actor_hazard_world PROPERTIES C_STANDARD 99)
add_test(NAME test_a2w_actor_hazard_world COMMAND test_a2w_actor_hazard_world)
set_tests_properties(test_a2w_actor_hazard_world PROPERTIES
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
# C3 #27 contract / fault matrix (lane C): pre-activation refusal, Join during
# a switch, failed admission/checkpoint and NULL accessors pass now;
# checkpoint_other_dimension and host_nether_guest_overworld are GREEN with the
# A2-T guest adapter + saved-dimension admission (a2t-guest-0927; Astra
# a2t-0927 ruling).
set(_cw_c27_red)
add_executable(test_c27_guest_contract tests/test_c27_guest_contract.c)
target_link_libraries(test_c27_guest_contract cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
	${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories(test_c27_guest_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_c27_guest_contract PROPERTIES C_STANDARD 99)
foreach(_cw_c27case refusal_preserves join_during_switch failed_admission
		failed_checkpoint null_accessors host_nether_guest_overworld
		checkpoint_other_dimension)
	add_test(NAME test_c27_guest_contract_${_cw_c27case}
		COMMAND test_c27_guest_contract ${_cw_c27case})
	if(_cw_c27case IN_LIST _cw_c27_red)
		set_tests_properties(test_c27_guest_contract_${_cw_c27case} PROPERTIES
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300 WILL_FAIL TRUE)
	else()
		set_tests_properties(test_c27_guest_contract_${_cw_c27case} PROPERTIES
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
	endif()
endforeach()

# --- design-wiring-0923 lane B: appended at the END of the test declarations --
# "END" here means after every other test target is declared but BEFORE the
# finalization below (the -UNDEBUG sweep, the registered-test count gate, the
# manifest/build-graph checks) -- appending after those gates would leave these
# two tests outside every one of them.

# Retained Play frontend session (Astra, GATE3-PLAY-PLAN.md): the ONE session
# the Load / Create / Join tabs share -- fresh-entry reset, tab validation, the
# owned unbounded world identity (aliasing + allocation failure retain rules),
# the name+address+index server identity validated before any mutation, and the
# by-value create-world draft whose editor is reverted on the COPY only.
# FOCUSED target: exactly the sources the test actually exercises -- no engine
# OBJECT pools, and therefore no gfx/input/world/zlib link dependency either;
# config.h and widget_osk.h are header-only here (m-lib lives in third_party/).
add_executable(test_play_session
	tests/test_play_session.c
	source/game/gui/play_session.c
	source/game/gui/cworld_form_model.c
)
target_include_directories(test_play_session PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source
	${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(test_play_session PROPERTIES C_STANDARD 99)
add_test(NAME test_play_session COMMAND test_play_session)

# PC gfx_scissor origin contract (VISUAL-COORDINATION.md section 8): compiles
# the REAL source/platform/pc/gfx.c gfx_scissor body against a recording GL
# sink and pins the top-origin -> GL bottom-origin flip (enable/disable, full
# screen, every split boundary, the approved menu rect). Python because it
# lifts the function text out of the shipped backend -- no GL context, no link
# against the PC platform pool.
add_test(NAME test_pc_scissor_coordinates COMMAND ${Python3_EXECUTABLE}
	${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pc_scissor_coordinates.py)

# ui-wiring-1010 TIPS-1: pure loading policy/layout, real font PNG metrics.
add_executable(test_loading_tips tests/test_loading_tips.c
	source/game/gui/loading_tips.c source/lodepng/lodepng.c)
target_include_directories(test_loading_tips PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_loading_tips PROPERTIES C_STANDARD 99)
add_test(NAME test_loading_tips COMMAND test_loading_tips
	${CMAKE_CURRENT_SOURCE_DIR}/assets/font/default.png)

# T04: production local loading status drawing and remote drawing regression.
add_executable(test_singleplayer_loading tests/test_singleplayer_loading.c source/game/gui/screen_load_world.c)
target_compile_definitions(test_singleplayer_loading PRIVATE PLATFORM_PC CGLM_ALL_UNALIGNED)
target_compile_options(test_singleplayer_loading PRIVATE -O2 -flto -ffunction-sections -fdata-sections)
target_link_options(test_singleplayer_loading PRIVATE -flto -Wl,--gc-sections)
target_link_libraries(test_singleplayer_loading m)
target_include_directories(test_singleplayer_loading PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_singleplayer_loading PROPERTIES C_STANDARD 99)
add_test(NAME test_singleplayer_loading COMMAND test_singleplayer_loading)

# design-crit-1010 LT-001: pure three-block loading indicator layout/timing.
add_executable(test_loading_indicator tests/test_loading_indicator.c)
target_include_directories(test_loading_indicator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
set_target_properties(test_loading_indicator PROPERTIES C_STANDARD 99)
if(NOT MSVC)
	target_link_libraries(test_loading_indicator PRIVATE m)
endif()
add_test(NAME test_loading_indicator COMMAND test_loading_indicator)

# ui-wiring-1010 slice G: GameCube glyph style C (approved 2026-10-06) in
# cells 32-41 of BOTH controller atlases, and gen_cc0_controls.py still
# reproduces assets/controls.png byte-for-byte.
if(Python3_Interpreter_FOUND)
	add_test(NAME test_gc_glyph_style_c COMMAND ${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gc_glyph_style_c.py)
	# Slice T: pixel-exact original branding and the real draw/GX contracts.
	add_test(NAME test_title_logo COMMAND ${Python3_EXECUTABLE} -I
		${CMAKE_CURRENT_SOURCE_DIR}/tests/test_title_logo.py)
endif()

# design-wiring-0923/play-flow: the CREATE TAB's lifecycle through the REAL
# screen_create_world.c, on the same real-Wii input rig as
# test_frontend_no_pointer.
#
# Why a SECOND target instead of adding one source to that one: VISUAL-
# COORDINATION section 14 asked for exactly that, and the build-graph guard
# below refused the configure -- test_frontend_no_pointer already compiles 19
# engine TUs, the guard's ceiling, so Create made it 20. The guard was right;
# section 15 supersedes 14 with this split. This target drops what Create's
# lifecycle does not need -- the pack screen, the creative picker, both MP
# screens and the network codec -- and lands at 17 engine TUs. The screens it
# drops are reached only BY ADDRESS by the ones it keeps, so the rig doubles
# them under TEST_FRONTEND_PLAY_FLOW (which is also how the real Create screen
# replaces the rig's inert vtable and its hand-copied String.trim).
add_executable(test_frontend_play_flow
    tests/test_frontend_play_flow.c
    source/game/gui/screen_create_world.c)
target_link_libraries(test_frontend_play_flow cavex_frontend_input_pool)
target_compile_definitions(test_frontend_play_flow PRIVATE
    PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST
    TEST_FRONTEND_PLAY_FLOW)
target_include_directories(test_frontend_play_flow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_frontend_play_flow PROPERTIES C_STANDARD 99)
target_link_libraries(test_frontend_play_flow m)
# argv[1] = the scratch parent: the run dir goes in the BUILD tree, never the
# source root (a source-root scratch dir flips tools/worktree-stamp.sh
# mid-run -> local_ci "PARTIAL -- tree changed during run", G3 09-26).
add_test(NAME test_frontend_play_flow
    COMMAND test_frontend_play_flow "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(test_frontend_play_flow PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ui-build-1026/T05: the APPROVED Create screen (Beta field order, New seed,
# Create | Details | Cancel, Details card fit, Beta seed chain through the real
# Create press). Same rig + TU set as test_frontend_play_flow.
add_executable(test_create_world_ui
    tests/test_create_world_ui.c
    source/game/gui/screen_create_world.c)
target_link_libraries(test_create_world_ui cavex_frontend_input_pool)
target_compile_definitions(test_create_world_ui PRIVATE
    PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST
    TEST_FRONTEND_PLAY_FLOW)
target_include_directories(test_create_world_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_create_world_ui PROPERTIES C_STANDARD 99)
target_link_libraries(test_create_world_ui m)
add_test(NAME test_create_world_ui
    COMMAND test_create_world_ui "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(test_create_world_ui PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
# ui-build-1026/T05a: the Create World keyboard (big name keys, seed keypad +
# Random, text seeds, Cancel keeps the old text). Same rig + TU set as above.
add_executable(test_create_world_osk
    tests/test_create_world_osk.c
    source/game/gui/screen_create_world.c)
target_link_libraries(test_create_world_osk cavex_frontend_input_pool m)
target_compile_definitions(test_create_world_osk PRIVATE
    PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST
    TEST_FRONTEND_PLAY_FLOW)
target_include_directories(test_create_world_osk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_create_world_osk PROPERTIES C_STANDARD 99)
add_test(NAME test_create_world_osk
    COMMAND test_create_world_osk "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(test_create_world_osk PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# menus-dwell-walker-1026: the menus_dwell WALKER (menu_autopilot_script.c)
# driving the REAL title, Select World, Create World (+ frontend editor) and
# pause screens on the same real-Wii input rig, in production frame order.
# RED on the 09-22..10-26 walker (it opened a name editor its synthetic
# presses cannot reach -- every real-Wii menus_dwell stalled at step 37) and
# on the hw1026b walker (one DOWN on the pause opened Achievements -- stall at
# STEP_WAIT_OPTIONS, 93).
add_executable(test_menus_dwell_create_flow
    tests/test_menus_dwell_create_flow.c
    source/game/gui/screen_create_world.c
    source/game/gui/screen_pause.c
    source/game/gui/menu_header.c
    source/game/gui/screen_rename_world.c
    source/game/gui/screen_multiplayer_setup.c
    source/game/gui/screen_multiplayer_connect.c
    source/network/mp_codec.c
    source/game/menu_autopilot_script.c)
target_link_libraries(test_menus_dwell_create_flow cavex_frontend_input_pool)
target_compile_definitions(test_menus_dwell_create_flow PRIVATE
    PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST
    TEST_FRONTEND_PLAY_FLOW)
target_include_directories(test_menus_dwell_create_flow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/wii_input
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(test_menus_dwell_create_flow PROPERTIES C_STANDARD 99)
target_link_libraries(test_menus_dwell_create_flow m)
add_test(NAME test_menus_dwell_create_flow
    COMMAND test_menus_dwell_create_flow "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(test_menus_dwell_create_flow PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Real shared frontend keyboard and GUI code; only the physical text queue is
# wrapped for deterministic owner/duplicate-input cases. No extra engine profile.
add_executable(test_osk_frontend tests/test_osk_frontend.c)
target_link_libraries(test_osk_frontend cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input)
target_include_directories(test_osk_frontend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_options(test_osk_frontend PRIVATE
    -Wl,--wrap=input_text_active -Wl,--wrap=input_text_poll
    -Wl,--wrap=input_text_mouse_pressed)
target_link_libraries(test_osk_frontend ${CMAKE_THREAD_LIBS_INIT} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
set_target_properties(test_osk_frontend PROPERTIES C_STANDARD 99)
add_test(NAME test_osk_frontend COMMAND test_osk_frontend)
# --- Tests must keep assert() even under -DCMAKE_BUILD_TYPE=Release -----------
# The suite is assert-driven and several tests intentionally place REQUIRED side
# effects inside assert(). Under NDEBUG those checks become no-ops. Apply the
# override after every test target has been declared so later additions cannot
# silently miss the sweep. bench_lighting_budget is registered as a correctness
# gate as well as a benchmark, so its invariant assertions must remain enabled.
get_property(_cw_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			 PROPERTY BUILDSYSTEM_TARGETS)
# The same sweep carries the test seed policy to every test_ target, pooled or
# standalone (a standalone test compiling source/util.c gets the deterministic
# seed too; pooled ones already inherit it from CAVEX_TEST_ENGINE_DEFINES).
foreach(_cw_t IN LISTS _cw_targets)
	if(_cw_t MATCHES "^test_")
		target_compile_options(${_cw_t} PRIVATE -UNDEBUG)
		target_compile_definitions(${_cw_t} PRIVATE CAVEWII_RAND_SEED_TEST)
	endif()
endforeach()
target_compile_options(bench_lighting_budget PRIVATE -UNDEBUG)

if(CAVEWII_REQUIRE_TEST_TOOLS)

	get_property(_cw_registered_tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
				 PROPERTY TESTS)
	list(LENGTH _cw_registered_tests _cw_test_count)
	# Merge reconciliation (2026-08-16): the menus branch carried 245 -> 248
	# (test_death_screen_env, test_osk_env, test_world_manage_env) and the
	# integration branch 245 -> 247 (test_server_shutdown_env,
	# test_compass_clock_recipe). Both are additive, so the merged inventory
	# is the SUM of the two deltas, not either side's number.
	# 2026-08-17: +1 for test_autoload_sentinel_env (the autoload "+new"
	# sentinel leak fix, layers 1+2 regression coverage; layer 3's coverage
	# was added to the existing test_config_fallback binary, so it costs no
	# new registration here).
	# 2026-08-20 (mp-s1 merge): bumped 256 -> 257 for PRE-EXISTING drift, not
	# a merge addition -- the sorted ctest -N name set of pre-merge f3bc39c
	# and the merged tree are byte-identical (257 names both sides; verified
	# empirically with two scratch configures). A rig test landed via the
	# 2026-08-19 merge train without bumping this gate, and no strict
	# configure ran between then and the merge's --regenerate-during-build.
	# 2026-08-20 audit repair train: +3 (test_inventory_load_transactional
	# CR-02, test_sign_recipe CR-03, test_file_read_short CR-07).
	# 2026-08-20 slice wave 2: +2 (test_detector_rail_golden CR-05,
	# test_sign_placement).
	# 2026-08-20 wave 3 (in-world sign text): +2 (test_sign_text_store,
	# test_sign_text_quads).
	# 2026-08-22 Batch C (FH-09/FH-10/FH-11): +1 for test_shapeless_recipes
	# (shapeless engine + all 31 RecipesDyes goldens + cookie + fishing rod;
	# one binary covers all three findings per the batch brief).
	# 2026-08-22 FH-08: +1 for test_egg_zombie_item (zombie spawn egg
	# relocated off Beta's item-id space, 360 -> NET-NEW 500; no save
	# migration needed -- see docs/deviations.md).
	# 2026-08-22 FH-14 sim batch: +1 for test_redstone_wire_dig (instant-break
	# hardness encoding). FH-16 extends the EXISTING test_worldgen_hell binary
	# (no new registration). FH-18 (entity Fire NBT bool -> signed counter)
	# also extends two EXISTING binaries -- test_region_entities.c (archive
	# layer: negative/positive/hostile-clamp coverage) and
	# test_entity_persistence_env.c (server_local.c restore path, end-to-end
	# through a real chunk save/load) -- no new registration either.
	# 2026-08-22 FH-04/FH-05/FH-06 render batch: +1 for
	# test_render_block_beta_uv_golden (portal shouldSideBeRendered predicate
	# + stairs corner-riser / fence post+rail bounds-derived UV).
	# 2026-08-23 FH-17: +2 for test_server_spawner (runtime MobSpawner TE
	# store drives spawn type) and test_mob_spawner_migrate (save version
	# 3->4 region migration, FH-07 pattern). Coordinate note: two sibling
	# lanes may also bump this count concurrently -- expect a textual merge
	# conflict here, not a real inventory mismatch.
# Batch F, lane 1 (2026-08-25): +1 for test_painting_env (Beta EntityPainting/
	# EnumArt/ItemPainting, gap audit #9, ported from the 2026-08-13 painting-slice
	# worktree onto tip).
	# Batch F, lane 2 (2026-08-25): +1 for test_map_item (recipe, onCreated/
	# onUpdate dispatch, colour-scan core, idcounts). Both lanes bumped 276->277
	# in isolation; integration = 278.
# Batch G / FH-13 (2026-08-25): +2 for test_pathfinding_golden (Path heap +
	# PathPoint hash + Pathfinder.createEntityPathTo synthetic-world search, pure/
	# world-free) and test_pathfinding_entity_env (server_tick_monster routes real
	# A* around a wall + the recalc-gate RNG cursor, full env harness).
	# Batch G follow-on (2026-08-25, coordinator-added audit fixes): +1 for
	# test_creeper_charged_explosion; player_melee_damage fix extends the
	# EXISTING tests/test_player_melee.c binary -- no new registration.
	# Integration 2026-08-25: Batch H's +1 (below) and Batch G's +3 both forked
	# from 278; combined inventory = 282.
	# Batch I (2026-08-25): +2 for test_falling_sand_archive (archive-layer
	# "Tile" tag round trip) and test_falling_sand_golden (full end-to-end
	# BlockSand.tryToFall/EntityFallingSand env golden). 282 -> 284. A
	# concurrent lane may also bump this count in source/network/server_remote.c
	# + mp_codec.* territory -- expect a textual merge conflict here, not a
	# real inventory mismatch (per this batch's own lane brief).
	# 2026-08-25 (creative+bobbing slice): +test_creative_mode_env (maintainer
	# exception, docs/creative_mode.md) +test_view_bobbing (Beta parity,
	# camera_bob.h). 284 -> 286.
	# 2026-08-26 Batch J (lane A gui): +1 for test_hud_hurt (heart blink +
	# low-health jitter + hurt/death camera roll, hud_hurt.h). A sibling sim
	# lane is bumping this count concurrently in the same tree (their
	# test_dig_tier_gates/test_region_timestamps) -- expect a textual merge
	# conflict here, not a real inventory mismatch. 288 -> 289.
	# 2026-08-26 creative phase 2 (lane A gui): +1 for test_creative_picker
	# (picker list contract + SRPC_CREATIVE_GIVE handler gates,
	# server_creative.c). Concurrent sibling bumps continue -- textual
	# conflict expected here, not a real mismatch. +1 over whatever the
	# sibling's latest value was (their 290 at the time of writing) -> 291.
	# 2026-08-26 lane C: +1 test_oracle_redstone (per-tick mc173 parity gate,
	# third oracle leg). 291 -> 292.
	# 2026-08-26 lane C: +1 test_giant (EntityGiantZombie parity: def goldens,
	# never-natural invariant, spawner + archive mapping). 292 -> 293.
	# 2026-08-26 lane C: +1 test_map_transport (CRPC_MAP_DATA held-map colour
	# delivery + change gate + pool hygiene). 293 -> 294.
	# 2026-08-26 map render slice: +1 for test_item_map_render_convert (Beta
	# 1.7.3 map item part 2b display-conversion half: MapItemRenderer.
	# renderMap's colour-byte -> RGBA8888 fill loop, item_map_render_
	# convert.c). Merged from the parallel worker branch -- the predicted
	# textual conflict at this line, reconciled. 294 -> 295.
	# 333 -> 334 (2026-09-02, cw_run_args hardening lane): +test_hw_run_args_parse
	# (TOTAL-ness regression suite for hw_run_parse_file/hw_run_parse_args --
	# oversized-file/NUL/control-byte/too-many-tokens rejection, and the
	# core fix: a rejected parse now fully resets cfg rather than leaving
	# earlier flags' side effects, e.g. an armed profile's autopilot_unstick,
	# in place).
	# 334 -> 335 (2026-09-02, cw_run_args hardening lane): +test_run_staged_
	# neutralize_args (PC-side orchestrator proof that run_staged_wii_
	# hardware.ps1's abort/cleanup `finally` neutralizes a staged-but-never-
	# consumed cw_run_args.txt on a zero-heartbeat run, and does NOT on an
	# explicit -NoCollect run).
	# 2026-09-03 merge into candidates-4: 335 (candidates-4) + 2 (run-args lane) -> 337.
	# 337 -> 338 (2026-09-03, wolf/spider-jockey parity lane): +test_spider_
	# jockey_spawn (audit found wolf taming already fully landed/tested;
	# the real gap was the spider jockey mount, entirely absent -- roll
	# [740c583: SpawnerAnimals.creatureSpecificInit], mount linkage, per-
	# tick rider-follow at the spider's overridden getMountedYOffset, and
	# both-direction auto-dismount on death).
	# 338 -> 339 (2026-09-03 merge into candidates-5; Stage-2 LOD S2 far-storage compression,
	# lane/stage2-s2-far-storage): +test_far_storage_equiv (byte-identical
	# display-list proof for "lod.far_storage":"skin" vs the default "voxel"
	# path -- seed x pattern matrix, a real near-chunk seam, the is_far_lod +
	# open-sky self-consistency assertion, and a far-evict/far<->full-
	# promotion churn test; see docs/stage2_lod_plan_2026-09-03.md).
	# 339 -> 341 (2026-09-03 merge into candidates-5; lane/parity-compass-clock): +test_compass_fx
	# +test_watch_fx (TextureCompassFX/TextureWatchFX byte-faithful ports --
	# hand-derived exact-arithmetic rest-frame pins + 2000-tick equivalence
	# sweeps against an independent reference port embedded in each test).
	set(_cw_expected_test_count 346) # ... FH-14 272 + FH-04/05/06 273 + FH-17 275
	# 341 -> 342 (2026-09-03 merge into candidates-5; lane/creative-hud-no-hearts): +test_hud_survival_row
	# (the extracted hearts+armor+air row -- creative-gate + survival-equivalence
	# golden; test_creative_mode_env extended in-place for the sibling void-damage
	# ruling, no further bump needed for that).
	# 342 -> 343 (2026-09-03 merge into candidates-5; lane/parity-mp-player-anim, MP remote-player
	# animation parity, items 1+2): +test_mp_animation_parity_wire (hurt-flash/
	# death-roll shared-formula pins for the new remote-player hurt tint +
	# death roll render). Extended in place by later commits on this lane.
	# 337 -> 338 (2026-09-03, lane/perf-t1-frame-drops, T1-d far-ring
	# frame-drop root-cause lane): +test_chunk_mesher_enqueue_cap_env (real
	# async-mesher golden proof that CHUNK_MESHER_ENQUEUE_CAP is pure pacing
	# -- a burst past the cap defers, never drops, and every deferred chunk's
	# final mesh byte-fingerprint matches its uncapped reference).
	# 338 -> 339 (2026-09-03, lane/perf-t1-frame-drops, T1-a far-ring
	# frame-drop root-cause lane): +test_fb_ring_equiv (self-contained mirror
	# of gfx.c's triple-buffered fb_ring/semaphore handoff -- proves the
	# 3-buffer capacity/token-conservation contract T1-a's investigation
	# confirms is already shipped; see docs/perf_frame_drops_rootcause_2026-09-03.md).
	# 343 -> 344 (2026-09-03, far-reach program Wave 1, lane/far-reach-wave1,
	# W1-c): +test_far_pick_frontier_golden (golden order-equivalence proof
	# that the new cached far-pick frontier in server_local.c's far send
	# loop -- a precomputed, position-independent sorted offset template
	# reused across a tick's whole FAR_SEND_BUDGET instead of a full
	# (2*far_view_distance+1)^2 rescan per sent column -- picks the exact
	# same (cx,cz) sequence as the old full-rescan algorithm, plus the
	# iterations/tick measurement at reach 11 and 16 the item's proof
	# standard requires; see docs/far_reach_program_2026-09-03.md).
	# 337 -> 338 (2026-09-03, telemetry lane, lane/telemetry-far-ring-keepup):
	# +test_far_ring_keepup (far-ring keep-up gate 4: world_far_missing_count
	# receive/evict/recenter bookkeeping + far_ring_keepup.h's pure
	# missing_max/settle_ms running-max state machine). test_hw_telemetry.c
	# is EXTENDED in place for the new heartbeat fields' JSON encoding -- no
	# new registration there.
	# 346 -> 347 (2026-09-03 merge into candidates-6; lane/telemetry-far-ring-keepup):
	# the two independently-numbered "337 -> 338" lanes above (perf-t1-frame-drops'
	# candidates-5 chain landing at 346, and this telemetry lane's own 337 -> 338)
	# both add one real test on top of a shared 337 ancestor; reconciled by keeping
	# HEAD's 346 total and adding this lane's +1 on top: 346 -> 347.
	set(_cw_expected_test_count 347) # ... FH-14 272 + FH-04/05/06 273 + FH-17 275
	# 347 -> 348 (2026-09-03, lane/perf-t1-ramp-slice, P2-b/T1-e round-2
	# frame-drop root-cause lane): +test_chunk_mesher_slice_env (real
	# async-mesher golden proof that chunk_mesher_build's cooperative slice --
	# thread_msleep(1) after each finalized list once elapsed build time
	# passes CHUNK_MESHER_SLICE_MS -- is pure pacing: forcing the slice to
	# fire after every finalized list produces byte-identical mesh output to
	# the same chunk built with the slice forced never to fire).
	# test_server_tick_pacing.c and tests/test_hw_telemetry.c are EXTENDED in
	# place for this lane's other two commits (P2-a load-ramp priority decision
	# assertions; P2-c ms_mesh_worker/ms_mesh_worker_max + mem2 lock-wait
	# telemetry fields) -- no further registration needed for either.
	set(_cw_expected_test_count 348) # superseded below (merge 2026-09-04) # ... FH-14 272 + FH-04/05/06 273 + FH-17 275
	# 348 -> 349 (2026-09-03, lane/mesher-priority, responsiveness lane H-2):
	# +test_mesher_band_priority (real async-mesher golden + scheduling proof
	# that world_build_chunks' band-0/band-1/band-2 priority -- far fully
	# starved while near is unmeshed, band 0 jumps ahead of an equally-dirty
	# band-1 chunk under a scarce token budget -- is pure ordering: byte-
	# identical mesh output regardless of gating). tests/test_hw_telemetry.c
	# and tests/test_world_chunk_walk_guard.c are EXTENDED in place (new
	# heartbeat fields' JSON encoding; the new allow_far parameter) -- no
	# further registration needed for either.
	# 349 -> 350 (2026-09-04 merge, lane/far-send-share, T2 far-send share):
	# 348 -> 349 (2026-09-04, lane/far-send-share, T2 far-send share):
	# +test_far_send_share (lod.far_send_near_owed == 0 reproduces the strict
	# pre-2026-09-04 near_complete far gate byte-for-byte over a scripted walk;
	# N > 0 fills the far annulus from the near loop's LEFTOVER per-tick budget
	# while the frontier still owes <= N; and the promotion-debt guard is proven
	# against the same walk with the guard bypassed -- see
	# docs/platform_perf_notes.md "Far-send share while the near frontier moves").
	# 349 -> 350 (2026-09-04 merge into cand10/keepup-rig): the two
	# independently-numbered "348 -> 349" lanes above (lane/mesher-priority via
	# cand9, and lane/far-send-share) each add one real test on top of a shared
	# 348 ancestor; reconciled by keeping both: 348 + 1 + 1 = 350.
	# 350 -> 351 (2026-09-07, embedded inventory ownership):
	# +test_inventory_ownership (the owned-free decision truth table and an
	# embedded inventory canary when a callback requests disposal).
	# (351 folded into the merged literal below) # ... FH-14 272 + FH-04/05/06 273 + FH-17 275
	# 350 -> 355 (2026-09-07, lane/craft-rpc): the server-authoritative
	# one-batch selected-recipe transaction adds five tests, one per
	# requirement -- +test_recipe_craft_transaction (manual-grid equivalence
	# swept over every registered recipe), +test_recipe_craft_capacity
	# (preflight covers result AND returned containers, rejection mutates
	# nothing), +test_recipe_craft_ticket (at-most-once per actor, stale
	# replies inert, ownership from the queue envelope),
	# +test_recipe_craft_pick_order (hotbar-then-main ascending, exact before
	# wildcard, byte-stable replay), +test_recipe_craft_map_server (durable
	# map ids allocated by the server path, exactly one per batch).
	# 355 -> 356 (2026-09-07, lane/craft-rpc fix round, hostile-review finding
	# 1): +test_recipe_craft_one_batch -- registers a synthetic container_item
	# whose returned containers form a registered pattern (the case a future
	# Beta-external item would create), proves Beta's shift-click recursion
	# really crafts twice on it, and pins the transaction's fail-closed refusal
	# with zero mutation while every shipped recipe still crafts.
	# 2026-09-07 merge: 350 + 1 (inventory ownership) + 6 (craft-rpc) = 357.
	set(_cw_expected_test_count 361) # +test_carve_pool_ledger (2026-09-08 pool/reservation ledger aliasing) # +test_carve_frame_walk_cost (2026-09-08 carve observer far-extract crawl) # ... FH-14 272 + FH-04/05/06 273 + FH-17 275 # +test_split_view_radius_transition (2026-09-11 join/leave) # +test_split_view_radius_pending_far (2026-09-11 in-flight far job at the join)
	# 308 -> 309 (2026-09-02, stage-server subnet + token auth, CWSTAGE4):
	# +test_stage_auth (constant-time token compare, subnet-membership check,
	# stage_token.txt trim -- stage_auth.h, shared verbatim with main.c's
	# cw_stage_auth_gate()).
	# 2026-09-02 merge: 309 -> 310 (ice + cworld + stage_auth).
	# 307 -> 308 (2026-09-01, field fix: ice dig speed + harvest water
	# conversion): +test_ice_dig (tool_dig_delay_ms speed matrix pinning the
	# Material.ice canHarvestBlock-always-true /30 branch, plus a live
	# SRPC_BLOCK_DIG harvestBlock water-conversion check: stone/water below ->
	# flowing water, air below -> stays air).
	# 2026-09-01: 307 -> 308 for test_cworld_form (create-world console form
	# state machine; the level-tag + seed-vector coverage extended the existing
	# test_osk_env binary and costs no registration).
	# 2026-09-02 merge: 308 -> 309 = both of the above (ice dig + cworld form).
	# 306 -> 307 (2026-09-01, floating-snow support fix): +test_snow_canblockstay
	# (BlockSnow.canSnowStay/canPlaceBlockAt -- a snow layer whose support block
	# is destroyed now pops via onNeighbourBlockChange instead of floating;
	# support-loss pop is SILENT per quantityDropped()==0, block_snow.c).
	# 305 -> 306 (field defect fix, Wii name-tag draw-pass coverage gap):
	# +test_name_tag_draw_pass (render_name_tag_draw_pass's previously-
	# untested select_nearest_players/build_batch dict-iteration + gfx
	# dispatch, recorder-gfx rig -- see tests/test_name_tag_draw_pass.c).
	# 304 -> 305 (2026-09-01, ghast spawn-channel fix): +test_ghast_spawn_env
	# (the independent GHAST_SPAWN_CAP channel in server_local_spawn_mobs --
	# overworld rand_src no-op equivalence + Nether ghast-presence/no-bleed
	# measurement over the real natural-spawn path).
	# integration/review-fixes (2026-09-01): six independent fix branches
	# (#1 stage-server hardening, #3 map-ID + #8 saplings, #4 deferred-edit
	# replay log, #2 socket race + #5 data races, #6 painting render, #7 boat
	# rider steering) merged --no-ff onto this 304 baseline and reconciled
	# HERE: only the #3/#8 branch (already reflected above) touched this
	# gate. #4/#6/#7 each extend an EXISTING test file in place
	# (test_mp_s0_env.c, test_painting_env.c, test_boat_env.c were already
	# registered targets pre-merge) rather than registering a new ctest
	# target, so composing all six changes NOTHING here. Verified, not
	# assumed: `ctest -N --test-dir build-verify` on the fully-merged tree
	# reports "Total Tests: 304", matching this literal exactly -- no edit
	# needed. Full run: 303 passed / 1 environmental skip
	# (test_run_wii_power_cycle, no .dol built in this PC-only integration).
	# 303 -> 304 (correctness milestone: sapling canBlockStay self-removal):
	# +test_sapling_canblockstay (BlockFlower.canBlockStay, inherited by
	# BlockSapling -- support-loss/darkness pop a sapling as a dropped item on
	# both the random-tick and neighbour-change paths, block_sapling.c).
	# NOTE: the pre-existing baseline measured 303 (via `ctest -N`) at this
	# commit's parent (the map-id-reuse fix landed just before this one, and
	# extended the existing test_map_item.c in place -- no new target needed
	# for it), not the 302 the prior comment claimed -- that comment was
	# already stale before this change (CAVEWII_REQUIRE_TEST_TOOLS defaults
	# OFF, so this exact-count gate is opt-in and was not actually catching
	# drift in normal `cmake --preset verify` builds). Corrected here to the
	# measured true count rather than perpetuating the drift.
	# 300 -> 301 (2026-08-31, MP visual-parity pass): +test_mp_visual_parity_pose
	# 301 -> 302 (2026-08-31, S3 background music): +test_audio_music_env (the
	# playRandomMusicIfReady timer + double-buffer ring goldens, headless
	# registry scan, music.volume round-trip).
	# (the swing-progress/sneak ModelBiped pose deltas + pickup-flight
	# ease-in-quadratic lerp pure math, render_biped_swing_pose.h +
	# entity_pickup_flight.h). test_name_tag_quads was EXTENDED in place
	# (is_sneaking param + 32-block cap test; no new target needed for it).
	# 299 -> 300 (2026-08-31, task_c75a3ed4): +test_autopilot_death_recovery
	# (the mp_lan_walk_10m spawn-pocket "walker immobility" root-cause fix --
	# autopilot.c's death_screen_ticks tracking + release-then-hold
	# IB_GUI_CLICK forcing on screen_death, and a FORWARD/JUMP regression
	# guard for far_walk_6m's byte-identical contract).
	# 295 -> 297 (2026-08-26, mp-s4/geomip lane): +test_lod_geomip_policy
	# +test_far_geomip_flagoff_equiv (the far-ring error-metric spike's
	# flag-off equivalence + policy gates).
	# 297 -> 298 (2026-08-31, MP name-tag parity): +test_name_tag_quads (the
	# render_name_tag.c pure-geometry golden -- distance cull, centering,
	# background span, billboard yaw, glyph UV).
	# 298 -> 299 (2026-08-31, MP setup screen v1+v2): +test_mp_setup_screen
	# (config.c server-list migration/round-trip + the screen's own
	# add/edit/delete array-mutation hooks, screen_multiplayer_setup.c).
	# + test_region_multichunk_read_equiv 276 (perf(region) 17b0718 -- the lane
	# added the test but missed this bump; caught at merge 2026-08-24)
	# + test_painting_env 277 (painting-slice landing)
	# + test_map_item 278 (map part 2a)
	# Batch H (2026-08-25): +1 for test_dig_speed_batch_h (obsidian 15000/ANY,
	# the 9-block .best->ANY sweep, wooden pressure plate tool-free harvest --
	# items 1-3 of docs/parity_audit_2026-08-25.md). Items 5/6/8/9/10/11 all
	# extended EXISTING test files (test_block_behavior_parity.c, test_decorate.c,
	# test_level_archive_recovery.c, test_window_id_unique.c, test_mp_s0_env.c),
	# no further bump needed for those. 278 -> 279.
	# 2026-09-01 (lane/persistence, correctness fixes): +1 for
	# test_region_load_stat_lru (region-probe-order T1 fix: LRU probed before
	# the stat(2)). 310 -> 311.
	# 2026-09-01 (lane/persistence): +1 for test_nbt_buffer_size_hint (cNBT
	# buffer pre-size T1 fix: buffer_reserve_exact + nbt_dump_binary_hint/
	# nbt_dump_compressed_hint). The region-torn-header-padding, chest/sign-
	# archive bounded-write, and sign-text UTF-8-codepoint-clamp fixes all
	# extended EXISTING test files (test_region_archive_safety.c,
	# test_archive_write_preserves_input.c, test_tile_entities_nbt.c) -- no
	# further bump needed for those. 311 -> 312.
	# 326 -> 327 (2026-09-02, release pipeline): +test_make_release (drives
	# tools/make_release.sh end-to-end via --skip-objdump-gate-for-tests
	# against a fake .dol/.elf/PASS.json; registered only when bash is found,
	# same conditional pattern as the existing PowerShell tooling tests).
	# 332 -> 333 (2026-09-02, lane/owned-assets-importer): +test_import_owned_beta_assets
	# (drives tools/import_owned_beta_assets.py against a synthetic jar/resources
	# fixture -- required-texture gate, dry-run, sound union/dedup/renumber,
	# music merge, record lowercase-rename, and manifest sha256 verification;
	# registered whenever Python3 is found, no bash/git dependency).
	# 2026-09-03 (lane/creative-music): creative-mode playlist add-on
	# (docs/creative_mode.md's maintainer ruling) -- extended BOTH existing
	# test binaries in place rather than registering new ones: section L in
	# tests/test_audio_music_env.c (creative-catalog selection gate,
	# telemetry-band arithmetic, separate-catalog registry scan, headless
	# non-firing) and five new test_* functions in
	# tests/test_import_owned_beta_assets.py (the --modern-minecraft /
	# --modern-minecraft-index creative-music source: happy path + hash
	# verification, dangling/missing-object reporting, index pinning,
	# dry-run). No change to _cw_expected_test_count -- same shape as the
	# 2026-09-01 persistence-fix entries above that extended existing files.
	# Native controller container test applies to every host variant.
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1")
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # local guest lifecycle
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # controller glyphs + notices
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # real Wii adapter + pre-claim prompts
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # split residency
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # lane/dl-ladder-pools: ladder sizing + mem2 pool
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # respawn residency
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # anchor Wii clock ordering
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # lighting memo equivalence
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # column dedup baseline goldens
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # column backing lifetime
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 4") # dedup mutex/scene/session + real gather
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # differential overlay
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # production-capacity recovery/churn
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # re-entrant recovery handshake + episode bound
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # thread CPU observer accounting
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # TPS analyzer + bounded recorder/hash assertion
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # carve observer
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # carve boundary world lifecycle
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # startup retirement equivalence
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # I07 lane 3: G9 drain golden (test_g9_drain_equiv)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # g9-ckpt-1009: checkpoint plane-copy/word-diff equivalence (test_g9_checkpoint_reuse_equiv)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # g9-ckpt-cpu-1009: exact-input compressed checkpoint memo
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ASTRA-G9-R5 point 9: window-max budget gate on the real g9_1 tape
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 15") # G9 RPC publication and concurrent telemetry regressions
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # Beta window clicks
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # lossless item imports
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # inventory sort planner
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # sort executor/admission
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # sort external oracle
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # menu sound events
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # complete menu sound priority arbitration
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # sort presentation
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # chest sound corrections: sort completion, Back, commit-timed item
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # settle accounting + reader
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # cactus side-contact damage
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # run_wii_hardware verdict fail-closed (review fix S5, 2026-09-24)
	# Lane 3 (approved controller events + pause recovery): the distinct
	# debounced event states, the approved card model, the cue map through the
	# real dispatcher, the neutral/fresh-press resume gate, and the low-battery
	# episode. test_input_wii_adapter is EXTENDED in place for the Wii feed.
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 5") # controller events + pause recovery
	# Lane 5 (approved pause / furnace / dispenser / sign / death + the shared
	# header-notice composition): the approved pause action model and cue map,
	# the two container compositions with their P2 acceptance gate, the sign
	# editor's discard/count/OSK flow, the commit-timed death confirmation, and
	# the menu x layout x family disjointness golden.
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 6") # other menus + notice composition
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # approved recipe UI
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # owner-local Pause and recovery
	# 2026-09-07 region-journal batch (far_walk pace regression): +1 for
	# test_region_index_journal_equiv (batched vs per-write publication
	# equivalence + the header-flip crash points).
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # region save-batch journal equivalence
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # experimental local split-screen: join ready + join failure
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # S2 two-phase retirement: test_region_evict_retain
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # M2 mesh blocks: test_displaylist_block_equiv
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # 2026-09-10 MP audit: test_mp_chunk_order (Packet51/52/53 ordering)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # 2026-09-11 chat composer: test_chat_composer
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # 2026-09-17 B1a chat keyboard relayout: test_chat_keyboard
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # 2026-09-11 approved UI geometry: test_ui_geometry
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # reconcile existing 426 registrations after merged lanes; portrait coverage extends test_container_menu
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # dynamic split layout
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # stale-bugs-verify-0924/V1: test_split_stale_reports
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # W1 mp-split-0912: test_mp_admission (radius gate + demote/promote re-encode)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # 2026-09-13 M3 remote readiness: test_load_world_ready
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # F8 mp-split-0912: test_local_player_column_guard (Beta blockExists gate)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # shared controller sheets across texture packs
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # S-A1 mp-split-0912: test_split_live_toggle (server-owned live split transition)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # S-A2 mp-split-0912: test_split_live_options (Options row applies the split switch live)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # S-A5 mp-split-0912: test_svin_pending_equiv (A8-L10 svin pending-queue equivalence across SVIN_SPLIT_SET_RESERVE)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # S-A10 mp-split-0912: test_split_live_seam (R-A9 gaps 4/5: main.c's guest-maintenance block + config-flush falling edge, sliced verbatim)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 13") # V6 F1 round 4 real-dispatch and credit-lifetime guards
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # V6 F1 round 5 sleep/terrain/fatal-replay guards
if(Python3_FOUND)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # V6 F1 stale-only enqueue assertion
endif()
if(Python3_FOUND)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # F1 memory disclosure
endif()
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # S-A12 mp-split-0912: test_split_off_equiv (A6 OFF-path byte-equivalence golden vs main f646e3a6)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # S1-a1b mp-split-0912: test_block_hooks_actor_golden (host STATE + RPC-ORDER golden for the actor-parameterized block/item hooks)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # stew-0925: test_stew_bowl_return (ItemSoup.java:8-10)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # S1-a3 mp-split-0912: test_host_damage_golden (host STATE + RPC-ORDER golden for the actor-parameterized damage/heal pipeline)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # S1-b mp-split-0912: test_split_actions_client (owner-0 RPC-stream golden + owner-1 slot-1 routing/rearm/reply gates)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # S1-d mp-split-0912: test_split_actions_counters (per-actor accepted-action counters + the main-thread mirror)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # robot-recovery-0917: test_split_recovery_script (split_recovery_6m phase driver: happy path, timeout, refusal, save failure, premature title, disabled profile)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # b1-chat-keyboard-0917: test_chat_login_script (mp_chat_login_3m phase driver: happy path through the real screen_chat/widget_osk handlers, every phase timeout, wrong focus, refused send, stale ack, disabled profile, other profiles + the memcmp owner-frame proof, owner isolation)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/X2: test_giant_box_golden (Giant box = Beta float product)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/GROWTH-A: test_growth_leaves_crops_golden (dark crops pop, Beta canBlockStay)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # Pack Helper audio seam: runtime + source guard
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 4") # c1-pack-helper-fix-0917/5: Pack Helper Python suite registered per module (core, conversion, desktop, gui)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # bootdeath-0917: test_check_sd0_dma_buffer (K12 SD DMA buffer gate: 4 KB-crossing boundaries, 64-byte alignment, size rule, fail-closed paths, preserved ELFs)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # hw1029b: test_build_gate_deps (foreign-root .d / stale .o refusal gate for Wii builds)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # c2-audio-coverage-0917/2: test_audio_registry_priority (approved UI package + note.* load first)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # b1-chat-keyboard-0917: test_chat_login_evidence (mp_chat_login_3m collector contract: marker set/order/seq, the p2.trace cadence, the ack ring slot, and the mc173-diag receipt, replayed offline)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # splitdims-profile-0926: test_split_dims_collector (split_dims_6m SplitDimsEvidence verdicts)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # telreliable-0926: test_telemetry_heartbeat_gap (collector attributes a heartbeat tick gap to the device sender counters)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # c2-audio-coverage-0917/4: test_audio_deferred (opt-in deferred cave-ambience loader: state machine, reconciliation, scratch guards, PCM equivalence)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # c2-audio-coverage-0917/5: test_audio_deferred_registry (opt-in deferred cave key through the real registry scan: OFF byte-identity, probed pool, deferred-vs-resident PCM equivalence, precedence, ceiling, unavailable rollback)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # c3-loading-sounds-0919: test_loading_notice (authored static loading-notice geometry + wording; strip layout identical to the pre-move strip)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # b1-chat-opener-0919: test_chat_gesture (the pure chat-opener gesture: the Wii Remote 2 tap window, the Classic Plus release-to-open with its Minus cancel, poll-gap/owner/disconnect clearing)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # b1-chat-opener-0919/03: test_chat_chord_evidence (mp_chat_chord_1m marker contract: both chord press orders, no chat opened, the opener after a neutral rearm, the overlay left OFF)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # b1-chat-opener-0919/07 B: test_sneak_toggle (the pure Wii Remote sneak-toggle machine: one toggle per fresh press, the death/respawn and vehicle/bed clears, screen preserve + suppress + neutral rearm)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # b1-chat-opener-0919/05: test_profile_registration (every hw_run profile registered in every tool ValidateSet + the collector; the stage-time endpoint/nonce condition)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # b1-chat-opener-0919/08: test_controls_sheet (docs/controls_sheet.md vs the EFFECTIVE bindings, modes and gestures; the pointer-look preset's coherence and the no-hybrid rule)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # b1-chat-opener-0919/15: test_player_ysize (the Beta sneak camera dip, Entity.ySize: float goldens, the constant held-sneak render sample, zero-state camera byte-identity, the wire/feet/collision byte-identity fold-out for both owners, the dipped block-pick ray)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/G7: test_player_step_ysize (Beta half-step remainder/decay, integer-height guard, partial-tick camera eye)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # b1-chat-opener-0919/16 G-0b: test_menus_dwell_contract (the collector's min_map_step IS menu_ap_script_step_required_for_pass, superseded ordinals named and refused)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/C7: test_rig_stamp_short7 (rig rev-parse --short=7 sweep, registered so the guard can't go orphan)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/B3: test_mp_play_core (the mp_play_10m pure phase machine against a reacting fake world: the reserve rule, the continuous latches, every phase budget, the snapshots)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/B27: test_mp_pause_menu_core
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/B4b: test_mp_play_map (mp_play_10m's pure translations: the MPP_ACT_* -> enum input_button table, the three slot numberings round-trip for all 36, the enum side -> Beta face table, the observation-ring copies and the marker line)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/B5: test_mp_play_evidence (mp_play_10m collector contract: marker set/order/seq, both generations' readiness and login deadlines, 1:1 receipt matching with consume-once, the floors/spread/displacement rules, both snapshots, and the offline replay entry point)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/A1: test_frontend_no_pointer (the real front-end update handlers with no pointer, on all three controller families' shipped bindings plus a remap: Close role, page roles, directional reachability, hotbar-alias collisions, stationary-vs-moved pointer arbitration)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/A5: test_menu_activate_rule (Astra ruling 2's ONE activation rule as a truth table: the hand-written rows plus all 288 input combinations against the ruling's own invariants)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/B10: test_dig_speed_parity (Beta's in-water / !onGround dig penalties: the 4-environment golden table, the unmoved dry rows, the cannot-harvest branch that takes no penalty, and the mc173 acceptance-gate inequality)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/B18: test_dig_crack_overlay (review B17 H1: the ten-stage crack overlay ages on the dig's own delay -- 6 block/tool cases x 4 environments, the 675 ms-of-3750 regression numbers, the no-penalty cannot-harvest arm, and the never-digs divide-by-zero floor)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # parity-render2-0924/G1: test_crack_overlay_depth (the crack pass's render-state decisions through the real render_block.c against a recording gfx sink + 24-bit depth oracle: LEQUAL + a bounded toward-the-eye depth-range bias so cracks cover an alpha-tested face's see-through texels, no draw through a nearer wall, opaque blocks unchanged, every state restored)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/B18: test_autopilot_route_rank (review B17 H4: ap_script_route_rank proven BOTH ways -- off the seam the config walker keeps the dirs[] table order, including across a RELEASED 90-degree leg; on the seam headings 0/90/180/270/45 each decide the first pick)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/B25: test_mp_pause_parity
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/C34: test_rain_quench_golden (host, guest, mob rain quench and dry controls)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/C40: test_weather_force_end_to_end (rain quad camera modelview)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/C42: separate rain renderer regression
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # design-wiring-0923: test_play_session (the retained Play session: fresh reset, tab validation, owned unbounded world identity with aliasing/alloc-failure rules, validate-before-mutate server identity, by-value draft reverted on the copy only)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # frontend keyboard mutation, caret, geometry and owner gating
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # design-wiring-0923: test_pc_scissor_coordinates (the PC gfx_scissor top-origin -> GL bottom-origin contract, compiled from the real backend source against a recording GL sink)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # design-wiring-0923/play-flow: test_frontend_play_flow (the Create tab's lifecycle through the REAL title/Load/Create screens and the real OSK: the tab route, the approved Create geometry by pointer, the draft retained across a Create->Load->Create round trip, modal Done/Cancel, the modal's tab lock, the Details round trip, the fresh-title reset, same-frame and held edges)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # menus-dwell-walker-1026: test_menus_dwell_create_flow (the menus_dwell walker against the REAL title / Select World / Create World / pause screens: step 0 to in-game, the pause Options + Save and quit walks, Create actioned with and without a player-0 owner, the editor root cause pinned)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # saves archive validator, PowerShell selftest and fake-card protocol suite
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # push-0921/C64: test_lava_aabb_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/MOBLAVA: test_mob_lava_aabb_golden (box minY = pos + y_size)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # lava-aabb-0924/A1: test_mob_lava_demo
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # lava-aabb-0924/B1: test_entity_fire_billboard_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # remote-mob-fire-0924/R1: test_remote_mob_fire
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # lava-aabb-0924/B3: test_split_partner_fire
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # parity-render2-0924/U1: underwater overlay tint
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/L1: test_player_residency_scan_char
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # fire-anim-0924/F1: test_flame_fx_two_instances
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # fire-anim-0924/F1: test_anim_fire_strip_fallback
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/X1: test_fire_block_uv_flip
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # fire-fix-0925: test_fire_dispenser_parity (BlockFire:119, BlockDispenser:118,129)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/G8: test_death_tilt (Beta rotateCorpse angle per mob type + feet pivot / forward axis)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/G8b: test_dying_red_tint (red overlay while hurtTime > 0 || deathTime > 0, MP + SP client paths)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # mp-velocity-0926: test_mp_local_velocity (remote Packet28 -> local player setVelocity)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # b1stamp-remote-0926: test_b1_remote_stamp (remote-origin RPCs carry the session identity)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G6 S1 (b1stamp-remote-0926): test_b1_stamp_namespace (disjoint local/remote world identities, both orders)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/R1b: test_hurt_wash_parity (Beta RenderLiving wash (brightness,0,0) at 0.4)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/G2: test_explosion_beta_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W1C: test_item_env_parity_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W2-MOVE P3: test_water_flow_push_golden (handleMaterialAcceleration + getFlowVector on player/mob/item)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W2-MOVE P4: test_entity_push_golden (applyEntityCollision from onLivingUpdate, Beta-ordered query)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W2-MOVE P12: test_cobweb_golden (BlockWeb isInWeb -> moveEntity 0.25/0.05F, motion zeroed)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W2-MOVE P4b: test_entity_query_local_equiv (section-list query == exhaustive ordered snapshot)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # integ-1011/1b: offered guest physical membership
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/GOLDEN-0925: test_golden_move_divergence (first-divergence scenes of the five re-captured whole-sim goldens)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W1A: test_player_move_parity_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W1F: test_difficulty_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W3-s1: test_heal_hurt_window_golden (every heal path closes the hurt window, EntityLiving.heal heartsLife = 10)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W3-s4: test_spawn_fallback_collision_golden (world-spawn push-up = Beta collision boxes on the populated spawn chunk)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W1D: crop/sapling light golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # growth-b-0925: test_growth_misc_golden (mushroom/sapling/cactus/reed random-tick parity)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # lava-aabb-0924/H1: test_autosave_entity_storm (region save entity storm under an allocator hammer + the current==NULL fail-closed stage)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # lava-aabb-0924/H2: test_entity_preserve_equiv (owner-index merge byte-identical to the linear-scan oracle)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # lava-aabb-0924/H3: test_entity_same_pos_save (same-position entities match by runtime id, the chunk save no longer fails closed)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # IMP-ENTITY-PRESERVE: test_imported_entity_roundtrip (imported Beta painting/cart/boat + mobs at non-float doubles survive unchanged/removed/moved saves losslessly)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/G5: test_redstone_lossless_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W1B: test_mob_move_parity_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W1B-b: test_mob_suffocation_equiv
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W1E: test_leaf_decay_golden (Beta leaf decay: flag on removal + connected walk)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W1E-b: test_leaf_decay_scratch_equiv (11^3 scratch == Beta 32^3, T1)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W1E-c: test_leaf_populate_flag_golden (trunk-over-canopy removal flag, WorldGenTrees:56-65)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W1E-d: test_tree_leaves_opaque_golden (leaves opaque to canopy placement, Block.java:151)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W1E-e: test_bigtree_leaves_keep_golden (big-oak discs skip existing leaves, Chunk.java:279)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # tnt-evidence-0925: lifecycle, collector, staged forwarding
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # split-dims B1e: client context routing and lifetime
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # split-dims b1-prep-0925: test_client_ctx_red_stale_gen (B1 contract, promoted 2026-09-25)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # split-dims b1-prep-0925: test_client_ctx_red_reset_quiesce (B1 contract, promoted 2026-09-25)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # split-dims b1-prep-0925: test_client_ctx_red_switch_drain (B1 contract, promoted 2026-09-25)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # split-dims B1 phase 1: test_client_ctx_red_exhaustion + test_client_ctx_red_cleanup_resume (rulings #1/#2) + test_client_ctx_red_stale_queue (ASTRA-B1-PHASE1 #2/#4: stream reset + reconnect), all green 2026-09-25
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # split-dims B1 phase 2 (b1-wire-0925): test_b1_registry + test_b1_guest_eye_heartbeat + test_b1_route_families
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 8") # split-dims lane B B-NOW (bnow-0926): test_b1_reset_{stale_terrain,stale_time_weather,session_reuse} (base + WILL_FAIL _red each) + test_b1_reset_{mesher_lease,pooled_release} (green)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # split-dims B2 prep (b2-0926): test_b2_world_payloads (world-bound client payloads, default OFF)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # split-dims B2 F5 (b2-0926): test_b2_gpu_lifetime (slot-1 GPU/display-list lifetime)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # split-dims B2 (b2-0926): test_b2_view_scope (view scope T0 + guest-world sound T5)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # split-dims B2 (b2-0926): test_b2_frame_worlds (main.c world passes T1-T3)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # split-dims B2 (b2-gates-0926): test_b2_gates (guest in-place reset + world resend)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # split-dims B2 (b2-a2t-0926): test_b2_a2t_client (A2-T concurrent client worlds / host join / BEGIN scope)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # b2-a2t-cap-0927: test_b2_a2t_admission (A2-T client capacity + landing ready)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # world-retire-srv-0927: test_world_retire_srv (vacated LIVE world -> zero columns, one CRPC_WORLD_RETIRE, clean rebuild)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # drown-fx-0927: test_drown_bubbles (Beta 8 drowning bubbles: client burst + server trigger order)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W2-MOVE RED: test_entity_swept_collision
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W2-R1: test_block_click_walk_env (onBlockClicked/onEntityWalking, Block.java:480-486)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/F1: test_block_light_value_golden (Beta getBlockLightValue neighbour max + weather subtract)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/P7: test_sleep_spawn_golden (Beta performSleepSpawning + bed empty-cell lookup)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/w2: test_skylight_tick_golden (skylightSubtracted cached per tick)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/w5: test_sleep_spawn_stress_fixture (sleep-engaged perf fixture)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/m1: test_mp_arm_swing (Packet18 at every EntityClientPlayerMP.swingItem site, EntityClientPlayerMP.java:118-121)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/m2: test_mp_remote_bobber (Packet23 type 90 -> visible hook, NetClientHandler.java:80-82)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/m3: test_mp_remote_falling_sand (Packet23 types 70/71 -> visible falling sand/gravel, NetClientHandler.java:109-116)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/W3-a2r: test_block_ray (Beta World.func_28105_a)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # split-dimensions-plan-0924/A1a: test_world_context_equiv (single-world golden + same-coordinate isolation + stale generation) and test_world_alias_gate (deprecated world-alias baseline)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # place-fix-0925: test_place_fence_torch_trapdoor (BlockFence/BlockTorch/BlockTrapDoor placement)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # run_wii_hardware failure paths power-cycle once (PWRCYCLE 2026-09-25)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ASTRA-SEED-POLICY-0925: test_rand_seed_policy
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # lever-piston-0925: test_lever_place_golden + test_piston_step_compare_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # furnace-0925: test_furnace_break_scatter (BlockFurnace.onBlockRemoval)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/STEPCAM: test_local_step_camera_golden (local step-up ySize bank = Beta exact face minY)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/STEPCAM: test_local_step_gate_golden (Beta step gate + horizontal-only step acceptance)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/STEPCAM: test_local_step_height_golden (EntityLiving.stepHeight 0.5F: slab steps, bed stops)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # itement-fix-0925: test_item_entity_parity (EntityItem despawn/pickup box/pop)
math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # KANBAN 09-14 stack gate: test_check_wii_stack_usage (LIBOGC_LIB forwarding + --scan-only)
	# split-dimensions-plan-0924/C2a: +3 ctest names on ONE executable
	# (test_owner_swap_pose_dead_sleep_barrier: barrier | knockback | delta).
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3")
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # split-dimensions-plan-0924/C2b: test_split_partner_fire_refused_install
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 15") # admit-retry-0925: test_split_admission_recovery (Astra retire-vs-retry ruling + review A cases)
	# split-dimensions-plan-0924/A1d step 1: +15 ctest names on SEVEN executables
	# (six test_owner_swap_* x {base, _red} + test_owner_swap_dimension_checkpoint_reload
	# x {reload, _refusal_visible, _nether_admit}); the 8 RED names are WILL_FAIL.
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 15")
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # h1-crumb-0925: test_stripe_wait_breadcrumb (H1 discriminator)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # a1b-red-0925: test_world_tile_isolation (A1b RED, dimension tile isolation)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # a1b-impl-0925: test_a1b_tile_lifecycle (A1b live-path tile/window/archive/effect isolation)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # a1b-fix-1: test_a1b_spawner_tracker_reload (Fable F1 tracker rows across reloads)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # c1-0926: test_c1_guest_window_bind (C1 guest restored-window binding)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # A1d review C1: test_owner_swap_bed_sleep_wake_spawn_wake
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # A1d review L1: test_sleep_spawn_all_sleepers (WILL_FAIL)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # laneC/C-DEADTARGET 2026-09-26: test_attack_dead_target_golden (EntityCreature dead-target drop/re-acquire; no attack on a corpse)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # WORLDGEN/REED 2026-09-26: test_reed_placement_sweep (Beta canPlaceBlockAt sweep)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # DECOR/1: test_decoration_placement_sweep
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # next-gaps-0924/GLYPH-1: test_controller_glyph_pack_proof (Astra pack-0926 #1)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # boot-sound-measure-0926: test_audio_load_counters
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # sounddir-0926: test_audio_dir_index_equiv
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # music-readahead-0926: test_music_readahead_equiv (32-half ring PCM == legacy 4-half ring)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # a1c-0926: test_a1c_world_ops_equiv + test_a1c_empty_occupied + test_a1c_residency_isolation (world operations per struct server_world*)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # damage-fix-0925: test_damage_parity_golden (knockBack, hazard order, drowning bubbles)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # damage-client-0926: test_knockback_client_motion (Beta knockBack motionY delivered + integrated by the hit body's client)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # laneC/TREE-HEIGHTMAP: test_tree_heightmap_live
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # asyncsnd-0926: test_audio_async_load
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # a2e-0926: test_a2e_entity_generation (M6 generation-safe entity bind/resolve; per-world entity sections)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 45") # A2-T transaction, two-world lifecycle, client admission, and G8 cap/Teleporter regressions.
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G8 sign publication is scoped to its world and generation.
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G8 secondary CHUNK/sign/entity publication and scoped resend.
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 12") # a2t-guest-0927 (lane C): test_a2_guest_adapter x12 (adapter, dwell, dwell_off, reopen_nether, reopen_overworld, reopen_refusal, nether_respawn, record_restore, dwell_host_busy, join_cap_ok, join_cap_refused, join_cap_timeout)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # lane C #14: test_arrow_pickup_actors
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # lane C #18: test_minecart_body_collision
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ANCHORGATE/1: test_wii_anchor_gap
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 13") # C2 tiles: test_owner_swap_tile_windows x13 cases
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # a2s-0926: test_a2_region_pool + test_a2_region_donor_faults + test_a2_save_bytes_equiv (shared region-handle pool; single-world save bytes)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 11") # lane C #19: test_owner_swap_monster_despawn x11 cases
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # G8: test_a2w_blast_isolation x2 cases
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G8: two-world actor suffocation ownership
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 8") # lane C #2: test_owner_swap_lightning x8 cases
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 7") # C3 #27: test_c27_guest_contract x7 cases
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # a2s-0926 part 2: test_a2_slot0_reload + test_a2_restore_atomic (tile_restore_plan prepare/preflight/commit)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # a2s-g6-0926: test_a2_journal_refusal (unresolved experimental journals refused, preserved)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 4") # lane C #20: test_owner_swap_wolf_interest x4 cases
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # pack-cache-0927: test_audio_pack_cache (opt-in audio pack cache: cached == fresh, hit/miss/invalidate/corrupt/torn/unwritable, both producers)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # pack-cache-0927: test_tex_pack_cache (opt-in texture pack cache: restored == fresh, manifest/source-list invalidation, corrupt/torn, Wii target) + test_tex_gx_conv_equiv (shared GX encoders == original Wii code)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # spawner-opacity-0927: test_spawner_opacity
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # wolf-tilt-0927: test_wolf_interest_hold (EntityWolf.onUpdate numTicksToChaseTarget=10 pin, Fable C20 F3a)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # lane C painting-bbox-0927: test_painting_hit x3 cases
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # explosion-impulse-0927: test_explosion_knockback_vertical x3
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # stage22-0927: test_evict_retire_pool_full (near eviction on a full client pool = next-tick deferral, not save failure stage 22)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 12") # lane C motion-units-0927 S1: test_mob_motion x12 cases
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # lane B a2t-d4-0927: test_a2_block_world_d4
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 8") # lane C motion-units-0927 S2: test_mob_trajectory_golden x8 cases (+ death-motion-0927 death_fall, death_slide)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 4") # lane C motion-units-0927 S2: test_mob_knockback_sites x4 cases
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # lane C motion-proof-0927: test_mob_motion_probe + test_mob_motion_rig_env + test_mob_motion_evidence
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G8 A2W batched-eviction publication failure/retry regression
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G8 owner-local loading: held-body hazards and cross-world pickup
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G8 guest transfer control replies during host loading ACK
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G8 Nether fireball publication regression
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G8 remaining projectile/dispenser world routing
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # projectile-small-1001 thrown-item billboard Python/C regression
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # G8 respawn recovery: real FIFO, invalid epoch, recovered retry
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 4") # G8 own-world recovery: vacant/peer, late NACK, unavailable center
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # arrow-parity-1002 mesh/texture recorder
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 108") # G8-VEHICLE-SEAMS-01 step 1: owner/client/lifecycle seam cases + test_local_guest vehicle-record/publication-retry modes
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 20") # ride-r2-1008: four R1 client cases, admission/disconnect/SP traces x2, six guest switches, destruction x2 kinds x2 worlds
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 17") # vehicle-smoke-1004: test_vehicle_smoke_script (host_vehicle_smoke_8m probe/driver: serializer, FIFOs, fixture, ride x3, switch x3, guest probes, misroute, death, pending/transfer, save, dormant, driver, hw_run)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 (G01): test_vehicle_smoke_evidence (host_vehicle_smoke_8m collector contract, synthetic tapes through the live parser)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # vehicle-smoke-1004 O02 (CONTRACT-v1.1 Q3/Q5): test_vehicle_smoke_script client_dispatch + epochs
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 O03: test_vehicle_smoke_script cart_route (guest never resends a host cart control tuple)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 O06: test_minecart_rail_collision_golden (EntityMinecart.onUpdate:316 moveEntity block collision on rail travel)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 O08: test_minecart_dismount_separation (G4 dismount -> impulse -> separation -> remount)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # vehicle-smoke-1004 O11: test_vehicle_smoke_script dispatch-seam + dispatch-guest + pickup-negative (S03 receipts at the accepted send; real pickup negative)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 O13: test_death_scatter_golden (G6 Beta death scatter)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 O13: test_vehicle_smoke_script death-scatter-host (G6 production host Respawn path)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 4") # vehicle-smoke-1004 O14: test_vehicle_smoke_script two-home geom/envelope/far/lethal
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 O15: test_a2_transfer_landing_real_coarse (READY observed at the real send)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # residency-1005/2: test_a2_transfer_landing_residency(_timeout) (COMMIT only onto a resident landing footprint)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 O15: test_vehicle_smoke_script hull-aim (pig switch_timeout_st0)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 O25: test_vehicle_smoke_script boat-yaw (HW5b ride_boat_st1)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 G8: test_knockback_client_steer
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # I02: test_entity_rng_streams (626edc43 gate + projectile spread goldens)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # I03: test_spawn_candidate_rng
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 O16: test_vehicle_smoke_script corner-guard (R06 P2 real pickup corner)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 O17: test_camera_ray_pick (camera_ray_pick hang)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 G1: test_minecart_offrail_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 O19: test_vehicle_rig_slot
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 G7: test_player_drop_rng_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 G3: test_mob_vehicle_solidity
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 G2: test_minecart_entity_collision_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # vehicle-smoke-1004 G5: test_player_push_decay_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # capture-fix-1003: test_remote_capture + test_remote_capture_service
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G10: test_world_tick_order_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9: test_light_drain_order_golden (RED gate, WILL_FAIL)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 9") # G9 B1: test_light_drain_order_golden split into 9 consumer cases + __settled
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # L1 no-downgrade gate
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # worldgen block parity vs original JAR
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 6") # G9 B4/B5: test_light_queue_equiv (6 cases)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9B R1 actual-history observer
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 B3: test_light_queue_equiv__phases
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # G9 R2: test_light_queue_equiv__drain_loss + __drain_stop_update
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R3: test_light_queue_equiv__contain
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R1: test_light_queue_equiv__checkpoint
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R4: test_light_queue_equiv__beta_oracle
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # G9B R8: original-JAR Nether setters, World.rand load32 and plain population rollback
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9B R9: original-JAR runtime lava notify callbacks
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9X-2: test_light_queue_equiv__g9x_callbacks (same-ID metadata removal callbacks)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9X-3: test_light_queue_equiv__g9x_dirty_reload (metadata beyond the population quadrant persists)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9X-4: test_light_queue_equiv__g9x_snow_oom (population transaction owns snow and its notifications)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9X-1d: test_light_queue_equiv__g9x_observer_reuse (history observer registers a reload at a reused ids address)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9X-5: test_light_queue_equiv__g9x_region_budget (bounded lossless generation light-region queue, 1,472 KiB ceiling)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9X-5b: test_light_queue_equiv__g9x_memory_progress (per-increment memory preflight + pressured checkpoint journal; ordered retry follows)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9X-6: test_light_queue_equiv__g9x_publication (already-sent column changed by neighbour generation, then retirement/re-admission; real client)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9X-7: test_light_queue_equiv__g9x_drain_t1_equiv (T1 generation relax vs frozen reference: ordered events + bytes, both dims)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R4: test_light_queue_equiv__consumers
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 RULING-3: test_light_queue_equiv__generation_beta_parity (blocking since G9X-7)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 RULING-3: test_light_queue_equiv__generation_parity_debt_golden (gated)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R6: test_light_queue_equiv__workloads
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # G9 R3 item 2: test_light_queue_equiv__packet_delivery(_nether)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R3 item 6: test_light_queue_equiv__hw_workload
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # P01 archive path-op census + full-byte equivalence
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # P04 path-cache corpus equivalence
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # P04 native path/cwd concurrency proof
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # aim-snap-1005/A01: test_attack_assist
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ir-jitter-1005/J01: test_menu_ir_jitter_golden
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # W01 test_mob_windup_cue
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 16") # residency-1005: test_player_residency_admission (host | guest | unknown | equiv | latency | loadahead | sources | portal | feedback | lean | bounded | lifecycle | rejoin | toasts | evictors | saturation)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # residency-1005/2: test_player_residency_admission_tape (authentic heartbeat tape)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # residency-1005/4 item 1: test_player_residency_admission_held_controls
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # residency-1005/4 item 3: test_player_residency_admission_client_retry
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # residency-1005/4 item 5: test_player_residency_admission_wake
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # residency-1005/4 item 8: test_player_residency_admission_telemetry_lifecycle
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # residency-1005/4 item 12: test_player_residency_admission_runtime_fault
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # residency-1005/4 items 2+4: test_player_residency_admission_vehicles | knockback | bounded_slots
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # residency-1005/4b item 6: test_player_residency_admission_lifecycle_missing
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # residency-1005/5 item 2: test_player_residency_admission_lifecycle_initial_load | lifecycle_saved_join | lifecycle_nether_respawn
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # residency-1005/6 R1: test_player_residency_admission_lifecycle_nether_respawn_far
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # residency-1005/6 R2: test_player_residency_admission_lifecycle_saved_join_bounded
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # residency-1005/5 item 3: test_player_residency_admission_vehicles_guest_away | vehicles_host_away
	if(Python3_Interpreter_FOUND)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # W01 test_mob_windup_render
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # residency-1005/2: test_residency_gate_tape (collector gate + negatives)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # residency-1005/4 items 9-11: test_residency_gate
	endif()
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 8") # achievements-1005/A: dormant achievements foundation (test_achievements_persist/golden/core/owner/seam/api + test_stats_counters/provenance, from lane/achievements 1b9f1a76)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ach-b1-1008: durable two-profile lifecycle
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # achievements-1005/D: native board and pause entry
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # B2: production lifecycle + actual producer shutdown
	if(Python3_Interpreter_FOUND)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # B2 R2 save recovery and production quit loop
	endif()
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # achievements-1005/E: FIFO notice dependency (RED)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # achievements-1005/E2: shared Warm bell device-boundary check
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # worlds2-pistons-1007: cross-dimension action/completion
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 14") # vehicle-worlds: both actor/world permutations and refusal
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # sound-fix-1006: hoe till
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # sound-fix-1006: ItemReed place
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # sound-fix-1006: fire punch fizz
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # sound-fix-1006: torch burnout fizz
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 24") # colcap-1006: 23 existing cases + warmed regrow route
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # colcap-1006/R3 item 2: test_anchor_pool_collector
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 5") # colcap-1006/R4 item 2: column-placement equivalence (scene_equiv, a1c, sep256 under lowest; rotating->lowest parity pair)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 3") # integ-1008: colcap test_pool_selftest + test_pool_selftest_collector + test_regrow_collector were registered on colcap-1006 but never counted
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R7 IRQ sampler core
	if(Python3_Interpreter_FOUND)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R7 collector synthetic/PC tapes
	endif()
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R17 remote ingress
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # L205: PC and host-Wii retirement
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R19 bounded local priority lease
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R22 B production MAIN handoff
	if(_cw_audio_sdk_headers)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R17 S1 native audio feasibility
	endif()
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R23 A local publication
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R17 S3 terrain FIFO head witness
	if(WIN32)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R24 R15 host starvation diagnosis
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 tick-wide boost gate
	endif()
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 13") # G9 R19 save IO: equiv world/session, durability faults/stalls/powercut, 8 deferred-mode regressions
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 decode deadline promotion
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R20 Q3 worst-tick probe
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R20 pending-ring burst
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # G9 R21 block-update packing: pack_barrier, pack_exhaustion
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 R21 test_rpc_block_pack_equiv
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # BUG-TICK hw1030a test_rpc_pool_burst
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 41") # R22 opt-in zlib decoded/codec keys and unchanged durability suites at levels 1..5
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # g9-trial-1009 test_g9_trial_kernel_equiv
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 6") # g9-savemiss-1009 test_g9_readback_memo_equiv (+ zlib 1..5)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R24 gate 4: retained-shadow cap overflow fallback
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # G9 authoritative death snapshot + collector
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # G9 diagnostic survival guard
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # G9 service observer core + derived-row collector
	if(Python3_Interpreter_FOUND)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # L225 heartbeat FPS windows
	endif()
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # L214 map/painting world routing
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 26") # L211 mounted portal + split splash/wolf ownership
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 40") # DROPS-WORLD: 20 drop/lifetime cases x two slots
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # fluid-world-1008: fluid/plate world routing
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # entity-world-1008: own-world entity routing
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # PIGZ-PACK: test_pig_zombie_pack_anger
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-wiring-1010 slice TIPS-1: test_loading_tips
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # design-crit-1010 LT-001: test_loading_indicator
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # T04 singleplayer loading
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # design-crit-1010 DC-GRASS G1: test_grass_render
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # design-crit-1010 DC-GRASS G2: test_mesher_nongrass_equiv
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # spruce-leaf-1030: test_leaves_graphics_level (Beta fast/fancy leaves follow render.fancy_graphics)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # design-crit-1010 DC-GRASS G3: test_grass_filter
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-wiring-1010 ACH-1: test_achievement_producers
	if(Python3_Interpreter_FOUND)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-wiring-1010: test_gc_glyph_style_c
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-wiring-1010 slice T: test_title_logo
	endif()
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # SNOW-001: test_itemreed_snow_place (reed/cake/repeater on a snow layer place in the snow cell)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ead-fx-ramp: test_anim_fx_ramp
	if(Python3_Interpreter_FOUND)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # EAD-007: test_anim_pack_colours
	endif()
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 5") # integ-1011/1e: offered piston driver
	if(Python3_Interpreter_FOUND)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # offered piston collector
	endif()
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # L2 original Beta import lighting
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 6") # G9B-3 R4/R5/R6 actual-JVM generation/load regressions
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # spawner-cap: generated domain exceeds 64 resident TEs
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # IN-008: test_mp_walk_session (real-Beta walk stream keeps the session)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # REL-MP-FIX-tests: test_mp_walk_session_fresh + _fresh_control (fresh-terrain edit flood keeps the session)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-MP-JOINSPEED: test_mp_walk_session_vanilla_join (fresh vanilla join: lossless oracle + compaction bounds)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # IN-009b: test_mp_fold_equiv (retained columns fold every Packet51 body losslessly)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # IN-009e: test_mp_end_reason (a write failure is latched as itself, mp_end 7)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # BUG-SPAWN: test_spawn_safety (fresh world never buries the player at spawn)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-03-tests: test_spawn_oracle (spawn search + first join vs original Beta, pinned spawn RNG)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R3 sweep: test_spawn_oracle_sweep (20-seed spawn vs original Beta + body never inside a block)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # online lightning: test_lightning_render (RenderLightningBolt geometry, bolt lifecycle, sky-flash curve)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-03b: test_spawn_search_failure (failed spawn search stops world creation)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R3 create path: test_spawn_create_path (GUI create yields the Beta spawn via the server half)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-05-tests: test_light_halt_forced (real forced light halt at the P1 boundaries, committed save intact)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # IN-003: test_client_sky_matches_server (client light == server stored light after initial publication)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-FLK-tests: test_chunk_refresh_draw_continuity (resident-column refresh keeps drawing retained geometry)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-TNT-tests: test_tnt_light_halt (powered TNT AIR light refusal halts through P1, committed save intact)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # REL-10-tests: test_main_tick_pacing (whole-tick eligibility) + test_main_tick_pacing_equiv (pacing off/on simulation bytes)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-MP-GATE: test_mp_session_gate_collector (mp session-ended gate on real hw1018/hw1014_e5 tapes)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-16a: test_soak_idle_play_core (soak_idle_play timer/phase core, fake-time fail-closed controls)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-16-collector: test_soak_idle_play_collector (soak_idle_play PC grader, fake-time controls)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-18e: test_clean_export (allowlisted clean export, synthetic repo)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-18e2: test_release_scan (export tree + zip quote/secret/rights scanner, synthetic)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R18-E1: test_release_allowlist (real allowlist exports HEAD with required inputs and pins)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # IMP-QUIT-UNSAVABLE: test_quit_unsavable_env (unsavable chunk at quit -> dialog state, Try again, Quit anyway saves the rest)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-11a: test_survival_journey_core (pure mine/craft/chest/furnace journey driver: success events vs rejected/uncommitted/timeout/missing screen/fixture/no progress)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # PAR-WIRE-POP: test_redstone_wire_support_drop_env (wire support-drop: air + 1 redstone item + line cut)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # PAR-WIRE-REMOVAL-ORDER: test_redstone_wire_removal_order_golden (wire removal recalculates at the removed cell, BlockRedstoneWire:227)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # PAR-BED-META: test_bed_meta_migrate (save v5 legacy bed metadata -> Beta, CaveWii-created worlds only, marker-guarded retry)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # IMP-PLAYER-DEFAULT: test_level_archive_player_health (level.dat without Player = 20 HP fresh player; Player without Health = Beta 10 HP)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # PLAYER-HEALTH-NBT: test_player_nbt_beta_types (Player.Health = Beta half-hearts on disk; Beta import 20 -> full; pre-fix CaveWii 160 -> full; exact round trip)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # IMP-TE-CAPS: test_te_pool_growth (growable tile tables) + test_import_te_dense (12k-note world reload)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-MP-COREFIRST: test_mesher_core_first (remote-loading core-first mesh order; local order + bytes unchanged)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # LIGHT-CLIENT-LOSSLESS: test_lighting_client_lossless (client relight past the 500k ceiling / low memory == full recompute; no relight against an absent column)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # IMP-MIGRATE-FAST: test_import_migrate_fast_equiv (one-pass survey chain == staged chain, byte-identical worlds + stats)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-build-1026/T09a: test_convert_only
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # dropin-1029: test_dropin_beta_import (Beta worlds drop-in, one level.dat save, Beta wall signs 4/5)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # critique-1028 eat: test_use_item_no_target (no-target use: food eats, throw/bow fire, block target unchanged, Packet15 -1 bytes)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # BUCKET-WATER-OFFLINE: test_bucket_liquid_local (no-target bucket: Beta liquid ray, sources only, face place, nether evaporate, creative keep)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # convert-notice-1024: test_import_convert_notice (Beta world convert notice, Option B; two-line confirm unchanged)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # java-load-1023: test_entity_nbt_beta_types (Beta tag types for every entity/tile-entity kind; legacy int saves load + delete)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # hw1023c: test_entity_direct_colour_bracket (every entity gfx_draw_quads_flt inside gfx_lighting(false)/(true), source lint)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-build-1026/T01: test_menu_one_click_rows (one pointer click chooses a Load / Texture packs row; Play/Apply still need their own press)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-build-1026/T06: test_texturepacks_screen
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-build-1026/T07: world_copy
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-build-1026/T03: test_title_tutorial (title Tutorial world row: shipped-world load, honest missing dialog, walker seam)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-build-1026/T05: test_create_world_ui (approved Create screen: Beta field order, New seed, Details reach + fit, Beta seed chain)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-build-1026/T05a: test_create_world_osk (Create keyboard: big name keys, seed keypad + Random, text seed, Cancel keeps old text)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-build-1026/T11: test_inventory_page_hold
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-build-1026/T15c: test_menu_autopilot_input_level (walker held/release edge; hardware unchanged when inactive)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ui-build-1026/T16a: test_world_copy_profile (world_copy_convert route: fixture-only activates, variants, markers)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # BOOT-CREDITS-1028: test_startup_credits (boot credit cards: timing, A, footer, marker)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # BG-BAKE-1028/3: test_backdrop_cache (generated backdrop cache: author untouched, identity, crash-safe publish, failures)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # BOOT-CREDITS-1028 T5: test_backdrop_settle_equiv (fast settle == legacy settle, real light)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # BG-BAKE-1028/2: test_backdrop_bake (storage-free scene bake: lifecycle, alloc failure, cancel, readiness gate, byte-identical pixels)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # hw1028b: test_stage_fetch_path (CWFETCH1 path = rest of line, "(backup)" fetchable, bounded, ".." refused)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # critique-1028: test_door_bounds_golden (BlockDoor selection/collision/render box, 16 meta x wood/iron)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # critique-1028: test_block_bounds_golden (audited Beta selection/collision bounds)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # critique-1028/mp-effects: test_mp_effects (0x36 note/piston dispatch, 0x3D aux effects, own 0x26/0x28, 0x10 resend)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # hw1029c/HEAP: test_dropin_heap_budget (real Beta world load stays inside the CavEX-world malloc-heap range; per-site counting allocator)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # hw1029c: test_dropin_spawn_chunk_te (no-Player Beta map keeps its spawn chunk's tile entities through load/unload/reload)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # hw1029c (a): test_furnace_load_beta (chunk load never rewrites a furnace block id; lit+BurnTime 0 stays lit)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # save-enc-1030: test_save_beta_encoding (LastUpdate = world time, empty lists type 1, per-class mob tags, double Pos/Motion, legacy records deletable)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # critique-1028 NETHER-LAG: test_nether_stream_cost (Nether vs overworld per-column streaming cost)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # nether-drain-1030: test_nether_light_hash (shipping vs reference Beta light relax: identical ring light arrays)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # world-reload-1030: test_world_reload_light_determinism (build/destroy/rebuild one world in a process: identical blocks+light)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # critique-1028 P3 MP-MISC: test_mp_misc (0x47 lightning sounds, 0xC8 -> achievements, online sign editor -> Packet130, malformed input)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # R12: test_save_reload_journey__journey + __powercut (journey save/quit/fresh-process reload; kill at every traced save transition)
	if(Python3_Interpreter_FOUND)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # REL-06a: test_fastcmp_capture (fastcmp capture override precedence/refusal + overridden-capture equivalence)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # sound-coverage: test_sound_import_coverage (every engine sound key searched by the importer or allowlisted; synthetic)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # tutorial-1021: test_tutorial_worldgen_lib (Python world writer + fixture) + test_tutorial_world_load (real C reader/writer on the generated save)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # tutorial-1021: test_tutorial_build (shipped tutorial world builder)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 2") # tutorial-ship: test_tutorial_world_real_build (fixture: the shipped world) + test_tutorial_world_real_load (every chunk, signs/chests, moved animal, broken chest)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # rawjar-1030: test_tex_beta_compose (raw b1.7.3.jar unzip: loader gui_2/anim == Pack Helper output, 1x/2x/4x; gate)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ingame-import-1030 A: test_beta_import_e2e (in-game importer pack == Pack Helper pack on a fixture jar)
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R12 save_reload_3m: test_save_reload_collector
		math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ingame-import-1030/B: test_beta_import_sound (in-game sound phase == Pack Helper bytes; missing folder = textures only; cancel)
	endif()
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # R12 save_reload_3m: test_save_reload_script
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ingame-import-1030: test_beta_import_ui
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ingame-import-1030 A: test_beta_import (zip reader, sha256, swap/power-cut)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # ingame-import-1030 A: test_pack_path_spaces (spaced pack folder loads; manifest display name)
	math(EXPR _cw_expected_test_count "${_cw_expected_test_count} + 1") # hw1030n: test_spawn_search_wait (heartbeat + probe count through a long spawn search)
	if(NOT _cw_test_count EQUAL _cw_expected_test_count)
		message(FATAL_ERROR
			"CaveWii test inventory mismatch: ${_cw_test_count} registered, expected exactly ${_cw_expected_test_count}")
	endif()
	message(STATUS "Complete CaveWii test inventory: ${_cw_test_count} tests")
endif()

# ---- Parallel ctest isolation (ctest-parallel-1006, maintainer-approved -j4) ----
# The suite runs `ctest -j4`. Fixture dirs are pid-suffixed and script tests
# bind port 0 (audit: tmp/jobs/ctest-parallel-1006/AUDIT.md), so most tests
# share safely. Two exceptions:
#  - cw_timing: tests that ASSERT on wall-clock time run RUN_SERIAL (alone),
#    so a busy CPU cannot fail them.
#  - cw_heavy: long CPU/RAM-heavy tests share one RESOURCE_LOCK, so at most
#    one of them runs at a time next to the light tests.
# A name that no longer exists is a configure error, so a rename can never
# silently drop a test's isolation. New timing-asserting or heavy tests: add
# them here (docs/build_conventions.md "Parallel ctest").
if(BUILD_TESTING)
	set(_cw_timing_tests
		bench_far_chunk bench_lighting_budget test_server_shutdown_env
		test_chunk_mesher_enqueue_cap_env test_chunk_mesher_shutdown_env
		test_settle_observer test_hw_telemetry
		test_main_tick_pacing) # REL-10-tests: real 100/400 ms sleep windows
	set(_cw_heavy_tests
		test_redstone_torch_clock_storm_env test_sleep_spawn_stress_fixture
		test_sleep_spawn_all_sleepers test_skylight_tick_golden
		test_worldgen test_worldgen_hell
		test_column_dedup_state_equiv test_column_dedup_render_equiv
		test_mob_motion_rig_env test_water_flow_push_golden
		test_entity_push_golden test_golden_move_divergence test_cobweb_golden
		test_entity_query_local_equiv
		test_main_tick_pacing_equiv) # REL-10-tests: 13 production-session arms, 600 s timeout
	foreach(_cw_t IN LISTS _cw_timing_tests _cw_heavy_tests)
		if(NOT TEST ${_cw_t})
			message(FATAL_ERROR "Parallel ctest isolation names an unknown test: ${_cw_t}")
		endif()
	endforeach()
	set_tests_properties(${_cw_timing_tests} PROPERTIES RUN_SERIAL TRUE)
	set_tests_properties(${_cw_heavy_tests} PROPERTIES RESOURCE_LOCK cw_heavy)
endif()

# ---- Tool-script tests (label cw_tools, maintainer ruling 2026-10-12) ----
# Python drivers that exercise tools/ or .claude/hooks scripts (Wii runner,
# stager, release maker, collectors, hooks), not the engine. The lead's
# one-ctest.sh skips label cw_tools when the change under test touches none of
# tools/, .claude/hooks/, tests/*.py|*.ps1, CMakeLists.txt or cmake/; CI and a
# plain ctest still run them. Several are only defined when Python/PowerShell
# are found, so a missing name is skipped, not an error: a renamed test simply
# loses the label and always runs (the safe direction).
if(BUILD_TESTING)
	set(_cw_tools_tests
		test_run_wii_hardware_failure_paths test_stage_saves_tools test_wii_profile_matrix_resume
		test_make_release test_run_staged_power_cycle test_remote_capture_service
		test_wii_profile_matrix_power_cycle_between test_run_staged_neutralize_args
		test_check_sd0_dma_buffer test_build_gate_deps test_mp_play_evidence test_require_hw_proof
		test_wii_profile_matrix test_wii_telemetry_server test_import_owned_beta_assets test_sound_import_coverage
		test_chat_login_evidence test_run_staged_tnt_forwarding test_hardware_proof_nudge
		test_regrow_collector test_wii_lighting_bench_runner test_run_wii_power_cycle
		test_run_staged_mailbox test_anchor_pool_collector test_split_dims_collector
		test_anchor_measurement_clock test_pool_selftest_collector test_remote_capture
		test_put_wii_stage_file_sha_mismatch test_fetch_wii_stage_file_sha_mismatch
		test_collect_staged_validator test_rig_stamp_short7 test_chat_chord_evidence
		test_wii_anchor_gap test_telemetry_heartbeat_gap test_tnt_evidence
		test_profile_registration test_heartbeat_fps_window test_anim_pack_colours
		test_gameplay_assess test_no_blanket_add test_vehicle_smoke_evidence test_title_logo
		test_split_offer_piston_collector test_save_reload_collector test_service_observer_collector
		test_residency_gate_tape test_residency_gate test_mob_motion_evidence
		test_gc_glyph_style_c test_g9_window_max_collector test_g9_cpu_evidence
		test_mp_session_gate_collector test_soak_idle_play_collector test_clean_export test_release_scan test_release_allowlist
		test_analyze_tps_walk test_analyze_settle)
	foreach(_cw_t IN LISTS _cw_tools_tests)
		if(TEST ${_cw_t})
			set_property(TEST ${_cw_t} APPEND PROPERTY LABELS cw_tools)
		endif()
	endforeach()
endif()

# ---- Build-graph validation (review batch, 2026-08-27) ----
# Runs at configure time in every BUILD_TESTING config (the CI verify leg).
# 1. Source-manifest drift: every on-disk source/**/*.c must be declared in
#    exactly one manifest; every manifest entry must exist on disk.
set(_cw_all_manifest ${CAVEX_ENGINE_SOURCES} ${CAVEX_APP_ONLY_SOURCES}
	${CAVEX_MP_SOURCES} ${CAVEX_AGENT_APP_SOURCES} ${CAVEX_NON_PC_SOURCES})
list(LENGTH _cw_all_manifest _cw_manifest_len)
set(_cw_manifest_dedup ${_cw_all_manifest})
list(REMOVE_DUPLICATES _cw_manifest_dedup)
list(LENGTH _cw_manifest_dedup _cw_manifest_dedup_len)
if(NOT _cw_manifest_len EQUAL _cw_manifest_dedup_len)
	message(FATAL_ERROR "source-manifest drift: a file appears in more than one CAVEX_*_SOURCES manifest")
endif()
file(GLOB_RECURSE _cw_disk_sources RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/source/*.c)
foreach(_cw_f ${_cw_disk_sources})
	if(NOT _cw_f IN_LIST _cw_all_manifest)
		message(FATAL_ERROR "source-manifest drift: ${_cw_f} is on disk but in "
			"no manifest -- add it to the right CAVEX_*_SOURCES list (or "
			"CAVEX_NON_PC_SOURCES if it is not a PC translation unit)")
	endif()
endforeach()
foreach(_cw_f ${_cw_all_manifest})
	if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_cw_f})
		message(FATAL_ERROR "source-manifest drift: manifest entry ${_cw_f} does not exist on disk")
	endif()
endforeach()

# 2. Target sweeps: unregistered full-engine tests, new compile profiles,
#    duplicate base+override TUs, and the playable-target test boundary.
set(_cw_registered_full_engine_tests "") # step 4 emptied it; keep it empty
# Per-target define exemptions beyond the shared profile/test sets. Adding a
# define anywhere else = a NEW compile profile = configure error (register it
# here, with a comment, or use a runtime-armed seam instead per CODESTYLE).
set(_cw_defs_exempt_test_minecart_client CAVEWII_AGENT_BRIDGE)
set(_cw_defs_exempt_test_mp_pause_parity CAVEWII_AGENT_BRIDGE) # synthetic held movement through production input
set(_cw_defs_exempt_test_split_actions_client CAVEWII_AGENT_BRIDGE)
set(_cw_defs_exempt_test_mp_arm_swing CAVEWII_AGENT_BRIDGE) # next-gaps-0924/m1: player-0 clicks through the production input path, as test_split_actions_client
set(_cw_defs_exempt_test_audio_env SOUND_TRANSCODE_NO_MAIN SOUND_TRANSCODE_EXTERN_VORBIS)
set(_cw_defs_exempt_test_tnt_primed_env FCAVEX_MAX_EXPLOSIONS_PER_TICK=2)
get_target_property(_cw_frontend_defs cavex_frontend_input_pool COMPILE_DEFINITIONS)
set(_cw_frontend_expected_defs PLATFORM_WII CGLM_ALL_UNALIGNED CAVEWII_CLIENT_INTERFACE_TEST
	CAVEWII_RAND_SEED_TEST)
list(SORT _cw_frontend_defs)
list(SORT _cw_frontend_expected_defs)
if(NOT "${_cw_frontend_defs}" STREQUAL "${_cw_frontend_expected_defs}")
    message(FATAL_ERROR "build-graph: frontend pool compile profile drift")
endif()
foreach(_cw_s IN LISTS CAVEX_FRONTEND_INPUT_TEST_SOURCES)
    if(NOT _cw_s IN_LIST CAVEX_ENGINE_SOURCES)
        message(FATAL_ERROR "build-graph: frontend pooled source outside canonical engine manifest: ${_cw_s}")
    endif()
endforeach()
get_property(_cw_all_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
foreach(_cw_t ${_cw_all_targets})
	get_target_property(_cw_type ${_cw_t} TYPE)
    get_target_property(_cw_srcs ${_cw_t} SOURCES)
    get_target_property(_cw_defs ${_cw_t} COMPILE_DEFINITIONS)
    get_target_property(_cw_links ${_cw_t} LINK_LIBRARIES)
	get_target_property(_cw_interface_links ${_cw_t} INTERFACE_LINK_LIBRARIES)
    set(_cw_frontend_edges "${_cw_srcs};${_cw_links};${_cw_interface_links}")
    if(_cw_frontend_edges MATCHES "cavex_frontend_input_pool" )
        if(NOT _cw_t MATCHES "^(test_frontend_(no_pointer|play_flow)|test_menus_dwell_create_flow|test_menu_one_click_rows|test_title_tutorial|test_texturepacks_screen|test_create_world_ui|test_create_world_osk|test_beta_import_ui)$")
            message(FATAL_ERROR "build-graph: frontend input test pool may only serve its five registered test drivers")
        endif()
        foreach(_cw_s IN LISTS _cw_srcs)
            if(_cw_s IN_LIST CAVEX_FRONTEND_INPUT_TEST_SOURCES)
                message(FATAL_ERROR "build-graph: ${_cw_t} duplicates frontend pooled TU ${_cw_s}")
            endif()
        endforeach()
        foreach(_cw_required IN LISTS _cw_frontend_expected_defs)
            if(NOT _cw_required IN_LIST _cw_defs)
                message(FATAL_ERROR "build-graph: frontend consumer ${_cw_t} missing ${_cw_required}")
            endif()
        endforeach()
        foreach(_cw_d IN LISTS _cw_defs)
            if(NOT _cw_d MATCHES "^(PLATFORM_WII|CGLM_ALL_UNALIGNED|CAVEWII_CLIENT_INTERFACE_TEST|CAVEWII_RAND_SEED_TEST|TEST_FRONTEND_PLAY_FLOW)$")
                message(FATAL_ERROR "build-graph: unexpected frontend input profile ${_cw_d}")
            endif()
        endforeach()
    endif()
	if(NOT _cw_type STREQUAL "EXECUTABLE")
		continue()
	endif()
	get_target_property(_cw_srcs ${_cw_t} SOURCES)
	get_target_property(_cw_defs ${_cw_t} COMPILE_DEFINITIONS)
	get_target_property(_cw_links ${_cw_t} LINK_LIBRARIES)
	if(NOT _cw_defs)
		set(_cw_defs "")
	endif()
	if(NOT _cw_links)
		set(_cw_links "")
	endif()
	set(_cw_links_test_pool FALSE)
	foreach(_cw_l ${_cw_links})
		if(_cw_l MATCHES "^cavex_test_")
			set(_cw_links_test_pool TRUE)
		endif()
	endforeach()
	set(_cw_engine_hits "")
	foreach(_cw_s ${_cw_srcs})
		if(_cw_s IN_LIST CAVEX_ENGINE_SOURCES)
			list(APPEND _cw_engine_hits ${_cw_s})
		endif()
	endforeach()
	list(LENGTH _cw_engine_hits _cw_engine_hit_count)
	if(_cw_t MATCHES "^(test_|dump_)")
		# 2a. duplicate base+override TUs: a target linking a shared pool
		# must not ALSO compile an engine-manifest file itself.
		if(_cw_links_test_pool AND _cw_engine_hit_count GREATER 0)
			message(FATAL_ERROR "build-graph: ${_cw_t} links a cavex_test_* pool "
				"AND compiles engine TU(s) itself (${_cw_engine_hits}) -- duplicate "
				"symbols / divergent flags. Use the pool or a variant flavor, not both.")
		endif()
		# 2b. unregistered full-engine test: focused tests may compile a FEW
		# engine TUs (2026-08-27 survey: the honest maximum is 15, the
		# lighting/relight pair; a full engine is 283) -- a per-target engine
		# rebuild is exactly what the pooling migration removed.
		if(NOT _cw_links_test_pool AND _cw_engine_hit_count GREATER 19
				AND NOT _cw_t IN_LIST _cw_registered_full_engine_tests)
			message(FATAL_ERROR "build-graph: ${_cw_t} compiles ${_cw_engine_hit_count} "
				"engine TUs without linking a shared pool -- unregistered full-engine "
				"test. Link the shared pool set (cavex_test_core cavex_test_world "
				"cavex_test_tickq cavex_test_input, or a flavor swap) instead.")
		endif()
		# 2c. new compile profiles: a target that links the shared pools must
		# compile its own TUs with the pool's define set (plus registered
		# exemptions) -- anything else is a NEW engine compile profile.
		# Focused tests (no pool link) own their few TUs and may define what
		# they need; 2b keeps them from quietly becoming full-engine.
		if(_cw_links_test_pool)
			foreach(_cw_d ${_cw_defs})
				if(_cw_d IN_LIST CAVEX_TEST_ENGINE_DEFINES)
					continue()
				endif()
				if(DEFINED _cw_defs_exempt_${_cw_t} AND _cw_d IN_LIST _cw_defs_exempt_${_cw_t})
					continue()
				endif()
				message(FATAL_ERROR "build-graph: ${_cw_t} carries unregistered compile "
					"definition '${_cw_d}' -- a NEW test compile profile. Prefer a "
					"runtime-armed seam (CODESTYLE); otherwise register an exemption "
					"above with a justification comment.")
			endforeach()
		endif()
	else()
		# 2d. playable/production boundary: no test defines, no test pools,
		# no test-marker source. (Post-link marker gates back this up on the
		# app and the .dol.)
		foreach(_cw_d ${_cw_defs})
			if(_cw_d STREQUAL "CAVEWII_TESTING" OR _cw_d MATCHES "^CAVEWII_.*_TEST$")
				message(FATAL_ERROR "build-graph: playable/production target ${_cw_t} "
					"carries test define '${_cw_d}'")
			endif()
		endforeach()
		if(_cw_links_test_pool)
			message(FATAL_ERROR "build-graph: playable/production target ${_cw_t} "
				"links a cavex_test_* pool")
		endif()
		if("tests/support/test_engine_marker.c" IN_LIST _cw_srcs)
			message(FATAL_ERROR "build-graph: playable/production target ${_cw_t} "
				"compiles the test marker source")
		endif()
	endif()
endforeach()
message(STATUS "build-graph validation passed: manifests consistent, profiles registered, test/playable boundary clean")
endif()

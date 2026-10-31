# Complete container draw lists

`test_sort_presentation` compares these byte-for-byte at the graphics boundary.
Each fixture includes the complete single, large, or compact chest draw, with
Nunchuk, Classic, or GameCube prompts and header/menu/held-stack-blocked states.
Texture binds, texture enable state, every quad vertex/color/UV, and item draw
commands are recorded. No coordinate, texture, sidebar, or footer filter exists.
Items are recorded as item/position/layer commands; their model renderer is not
part of this container-layout oracle. Font advances come from the locked Beta
prototype and pass through the normal font reset/measurement/rendering path.

Sort was parked on 2026-10-25 (maintainer: a CavEX addition, not Beta 1.7.3).
Fixtures were regenerated then: no list contains a Sort entry or panel. Full
views run `screen_chest.reset/update/render2D`, navigating from the initial
hotbar selection through sections and pressing UP off the top row (the former
Sort entry path); the "menu" state must equal the "header" state byte for byte.
Compact fixtures run the complete adapter after a step that clears any stale
Sort state. `test_input_wii_adapter` checks that no device (Nunchuk, Classic,
GameCube) gets any Sort draw.

To deliberately update fixtures, run `test_sort_presentation` from the project
root with `CW_UPDATE_SORT_GOLDENS=1`, inspect the full diff and PC captures, then
rerun with the variable absent. Tests never update fixtures by default.

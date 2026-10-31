/*
	test_window_registry_unregistered -- the NEGATIVE test. This file MUST FAIL
	TO COMPILE; ctest runs it via `cmake --build --target ...` with WILL_FAIL
	set, so a successful build is the test failure.

	It is the whole point of the registry refactor. Three consecutive parity
	slices (dispenser D8/D9/D10, minecart-chest) shipped a bug because a new
	window type had to be registered in six independent hardcoded dispatch
	tables and missing any one compiled cleanly, passed isolated unit tests,
	and failed SILENTLY at runtime -- differently at each site (CODESTYLE.md
	S3.3). The guard in window_registry.h turns that into a BUILD failure.

	Below, an "ender chest" slice author adds their row and nothing else. If
	this file ever compiles, the guard has been removed or defanged and the
	defect class is back.
*/

#define WINDOW_REGISTRY_EXTRA_ROW(X)                                           \
	X(ender_chest, 8, WINDOW_TYPE_CHEST, CHEST_SIZE, 0, CHEST_SLOT_MAIN,       \
	  CHEST_SLOT_HOTBAR, CHEST_SLOT_STORAGE, CHEST_SIZE_STORAGE,               \
	  IRON_CHEST_SIZE_STORAGE, WINDOW_RECOVERY_TE_CHEST, WINDOW_KEY_BLOCK)

#include "../source/network/window_registry.h"

int main(void) {
	return 0;
}

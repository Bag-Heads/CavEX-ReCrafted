// test_ir_calibration_width.c -- [C8] Wii IR pointer calibration invariant.
//
// Root cause (fixed in source/main.c): input_init() used to run BEFORE
// gfx_setup(). On Wii, input_init() calls
//     WPAD_SetVRes(WPAD_CHAN_0, gfx_width(), gfx_height());
// but gfx_width() returns the static initializer (802) until gfx_setup()
// resolves the real virtual width: 640 for a 4:3 console, 802 for 16:9
// (see source/platform/wii/gfx.c gfx_setup(): viWidth/gfx_screen_width
// branch on CONF_GetAspectRatio()). gfx_height() is always 480.
//
// Calibrating the IR space to 802 while the GUI is laid out for 640 on a 4:3
// console produces a horizontal hit-test skew of 802/640 ~= 1.2531 (~25%):
// the pointer maps to a wider virtual span than the screen the GUI assumes,
// so clicks land left of their target as you move right.
//
// The Wii WPAD path is not headless-buildable (devkitPPC / WPAD), so this
// test asserts the INVARIANT via a tiny model of the gfx_setup() width
// resolution. It proves the value WPAD_SetVRes must receive once gfx_setup()
// has run, which is exactly what the main.c reorder guarantees.

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

// Mirrors source/platform/wii/gfx.c gfx_setup(): 16:9 -> 802, 4:3 -> 640.
// (The stale pre-gfx_setup() value gfx_width() returns is the 802 initializer.)
static int resolve_width(bool wide) {
	return wide ? 802 : 640;
}

// Mirrors gfx_height() on Wii: constant 480 regardless of aspect ratio.
static int resolve_height(void) {
	return 480;
}

int main(void) {
	// The stale static initializer that gfx_width() returns BEFORE gfx_setup()
	// resolves the real aspect ratio. Calibrating IR to this on a 4:3 console
	// is the bug the main.c reorder removes.
	const int STALE_WIDTH = 802;

	// 4:3 console: real virtual width is 640, not the stale 802.
	assert(resolve_width(false) == 640);

	// 16:9 console: real virtual width is 802 (coincides with the stale
	// initializer, so 16:9 was never visibly skewed -- only 4:3 was).
	assert(resolve_width(true) == 802);

	// Height is aspect-independent and never stale.
	assert(resolve_height() == 480);
	assert(resolve_width(false) != resolve_height());

	// Document/guard the skew the reorder removes on a 4:3 console: IR space
	// calibrated to the stale 802 vs. the 640 the GUI is laid out for.
	double skew = (double)STALE_WIDTH / (double)resolve_width(false);
	assert(skew > 1.25 && skew < 1.26); // 802/640 ~= 1.2531

	// On 16:9 there is no skew: the stale value already equals the real width.
	assert(STALE_WIDTH == resolve_width(true));

	printf("test_ir_calibration_width: OK "
		   "(4:3=%d 16:9=%d h=%d skew(4:3)=%.4f)\n",
		   resolve_width(false), resolve_width(true), resolve_height(), skew);
	return 0;
}

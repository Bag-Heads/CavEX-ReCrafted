/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * A scriptable stand-in for source/platform/beta_import.c (ingame-import-1030)
 * for the headless front-end screen suites, which link the real screens and
 * the real Wii input but no SD card, worker thread or zip reader. It follows
 * beta_import.h's contract to the letter where the UI can observe it:
 *   - beta_import_poll is a snapshot copy and never blocks;
 *   - the registered selector is called ONCE, from poll (the main thread),
 *     the first time poll reports BI_DONE -- never before the swap;
 *   - beta_import_start returns false while a run is active.
 * A test drives the worker's side by writing fake_bi.progress between frames.
 * Default: no jar, so every suite that does not opt in sees exactly the
 * pre-import Texture Packs screen. */

#include "fake_beta_import.h"

#include <string.h>

struct fake_beta_import fake_bi;

void fake_beta_import_reset(void) {
	memset(&fake_bi, 0, sizeof(fake_bi));
}

bool beta_import_available(void) {
	fake_bi.available_calls++;
	return fake_bi.available;
}

bool beta_import_start(void) {
	fake_bi.starts++;
	bool active = fake_bi.progress.state >= BI_SCANNING
		&& fake_bi.progress.state <= BI_SWAPPING;
	if(active || fake_bi.start_refuse)
		return false;
	memset(&fake_bi.progress, 0, sizeof(fake_bi.progress));
	fake_bi.progress.state = BI_SCANNING;
	fake_bi.selector_called = false;
	return true;
}

void beta_import_poll(bi_progress* out) {
	fake_bi.polls++;
	if(fake_bi.progress.state == BI_DONE && !fake_bi.selector_called) {
		fake_bi.selector_called = true;
		if(fake_bi.selector)
			fake_bi.selector_result = fake_bi.selector("texturepacks/Beta-1.7.3");
	}
	*out = fake_bi.progress;
}

void beta_import_cancel(void) {
	fake_bi.cancels++;
}

void beta_import_set_selector(bi_select_fn fn) {
	fake_bi.selector = fn;
}

void beta_import_recover(void) {}

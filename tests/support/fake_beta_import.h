/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 * Scriptable beta_import.h backend for headless screen tests; see the .c. */
#ifndef TESTS_SUPPORT_FAKE_BETA_IMPORT_H
#define TESTS_SUPPORT_FAKE_BETA_IMPORT_H

#include <stdbool.h>

#include "platform/beta_import.h"

struct fake_beta_import {
	bool available;	   /* what beta_import_available answers */
	bool start_refuse; /* true = start refuses (beyond "already active") */
	bi_progress progress; /* the worker's state; the test writes it */
	bi_select_fn selector;
	bool selector_called, selector_result;
	int available_calls, starts, polls, cancels;
};
extern struct fake_beta_import fake_bi;

void fake_beta_import_reset(void);

#endif

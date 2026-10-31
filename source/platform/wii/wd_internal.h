/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).

	Internal seam between wii/watchdog.c (defines wd_emit_dump + the volatiles) and
	wii/panic.c (the fault handler calls wd_emit_dump). Not part of the public API.
*/

#ifndef WD_INTERNAL_H
#define WD_INTERNAL_H

#include <stdint.h>

// Emit the panic dump (banner + hang_report.txt). gpr non-NULL => FAULT path (real
// registers); NULL => HANG path (phase tags + counters only).
void wd_emit_dump(const char* cause, uint32_t srr0, uint32_t srr1, uint32_t lr,
				  const uint32_t* gpr);

#endif

/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).

	Fault handler. Compiled via the flat dispatcher source/platform/panic.c under
	PLATFORM_WII. Overrides libogc's (tuxedo) exception panic fn so a REAL CPU fault dumps
	registers via wd_emit_dump instead of libogc's default (which prints to a dead UART
	then spins forever -- the reason a fault has been indistinguishable from a hang).
*/

#include <gccore.h>
#include <tuxedo/ppc/exception.h> // PPCExcptCurPanicFn, PPC_EXCPT_*, PPCContext
#include <stdint.h>
#include <stdio.h>

#include "wd_internal.h" // wd_emit_dump

static void cavewii_fault_panic(unsigned exid, PPCContext* ctx) {
	static const char* NM[]
		= {"?", "RESET", "MCHK", "DSI", "ISI", "IRQ", "ALIGN", "PROGRAM", "FPU", "DECR"};
	char hdr[48];
	snprintf(hdr, sizeof(hdr), "FAULT %s exid=%u", (exid < 10) ? NM[exid] : "?", exid);
	// ctx->pc == faulting PC (SRR0), ctx->msr == SRR1, ctx->lr, ctx->gpr[1] == SP.
	wd_emit_dump(hdr, ctx->pc, ctx->msr, ctx->lr, ctx->gpr);
	for(;;) {
	} // world is corrupt -- hold the banner up, do not return into garbage
}

void watchdog_install_fault_handler(void) {
	PPCExcptCurPanicFn = cavewii_fault_panic; // replaces libogc's DEBUGPrint+spin default
}

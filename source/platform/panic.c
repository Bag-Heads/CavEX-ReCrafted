/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).

	Flat platform dispatcher for the fault handler (mirrors source/platform/gfx.c). The PC
	no-op of watchdog_install_fault_handler lives in watchdog.c's PLATFORM_PC branch, so on
	PC this TU is empty (the typedef avoids the ISO-C empty-translation-unit warning).
*/

#ifdef PLATFORM_WII
#include "wii/panic.c"
#else
typedef int wd_panic_pc_dummy;
#endif

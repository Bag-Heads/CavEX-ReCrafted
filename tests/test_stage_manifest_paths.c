#include "stage_manifest_paths.h"

#include <assert.h>
#include <string.h>

static void expect_path(size_t index, const char* expected) {
	assert(index < CW_STAGE_MANIFEST_PATH_COUNT);
	assert(strcmp(CW_STAGE_MANIFEST_PATHS[index], expected) == 0);
}

static int path_count(const char* needle) {
	int count = 0;
	for(size_t i = 0; i < CW_STAGE_MANIFEST_PATH_COUNT; ++i) {
		if(strcmp(CW_STAGE_MANIFEST_PATHS[i], needle) == 0)
			++count;
	}
	return count;
}

int main(void) {
	assert(CW_STAGE_MANIFEST_PATH_COUNT == 29); // +2: live sd:/config.json and its default (2026-09-17). // First-failure telemetry trace.
	expect_path(0, "sd:/CAVEWII_PROOF_LAUNCHER.json");
	expect_path(1, "sd:/apps/CaveWiiStage/proof_launcher_manifest.json");
	expect_path(2, "sd:/apps/CavEX/boot.dol");
	expect_path(3, "sd:/apps/CavEX/cw_run_args.txt");
	expect_path(4, "sd:/cw_boot_trace.txt");
	expect_path(5, "sd:/hang_report.txt");
	expect_path(6, "sd:/fbguard_report.txt");
	// The watchdog's app-dir FALLBACK reports (source/platform/hw_telemetry.h
	// HW_TELEMETRY_*_REPORT_FALLBACK_PATH). It writes these when the sd:/ root open
	// fails, i.e. on exactly the boot where entries 5 and 6 hold nothing -- so a manifest
	// that omits them makes the fallback a file no collector can ever fetch.
	expect_path(7, "sd:/apps/CavEX/hang_report.txt");
	expect_path(8, "sd:/apps/CavEX/fbguard_report.txt");
	// SEQ-SUFFIXED hang reports 2..WD_REPORT_SEQ_MAX, root then app-dir fallback
	// (source/platform/watchdog.h wd_report_path_for_seq). wd_write_report opens with
	// fopen("w"), which TRUNCATES, so before 2026-07-29 report N destroyed report N-1 --
	// the card recovered that day read seq=6, i.e. five stalls whose reports no longer
	// existed. A path the manifest does not name can never be fetched, so omitting these
	// would recreate the exact defect entries 7-8 were added to fix, one level along.
	expect_path(9, "sd:/hang_report_2.txt");
	expect_path(10, "sd:/hang_report_3.txt");
	expect_path(11, "sd:/hang_report_4.txt");
	expect_path(12, "sd:/hang_report_5.txt");
	expect_path(13, "sd:/hang_report_6.txt");
	expect_path(14, "sd:/hang_report_7.txt");
	expect_path(15, "sd:/hang_report_8.txt");
	expect_path(16, "sd:/apps/CavEX/hang_report_2.txt");
	expect_path(17, "sd:/apps/CavEX/hang_report_3.txt");
	expect_path(18, "sd:/apps/CavEX/hang_report_4.txt");
	expect_path(19, "sd:/apps/CavEX/hang_report_5.txt");
	expect_path(20, "sd:/apps/CavEX/hang_report_6.txt");
	expect_path(21, "sd:/apps/CavEX/hang_report_7.txt");
	expect_path(22, "sd:/apps/CavEX/hang_report_8.txt");
	expect_path(23, "sd:/lighting_bench.txt");
	// The stage server's OWN per-boot phase trace (main.c boot_trace()).
	// Added 2026-09-02: a boot that hung before listen() used to leave no
	// artifact at all, so the manifest entry is what makes the diagnosis
	// fetchable. KEEP IN STEP with CW_STAGE_BOOT_TRACE_PATH.
	expect_path(24, CW_STAGE_BOOT_TRACE_PATH);
	assert(strcmp(CW_STAGE_BOOT_TRACE_PATH, "sd:/cw_stage_boot.txt") == 0);
	// The rotated copy of the previous boot's trace (main.c
	// boot_trace_rotate(), called from boot_trace_begin() before the fresh
	// boot's fopen("w") truncates CW_STAGE_BOOT_TRACE_PATH). Added
	// 2026-09-02 so the prior instance's final "exit: ..." line -- written
	// after the listen socket is already closed -- survives to be fetched.
	// KEEP IN STEP with CW_STAGE_PREV_BOOT_TRACE_PATH.
	expect_path(25, CW_STAGE_PREV_BOOT_TRACE_PATH);
	assert(strcmp(CW_STAGE_PREV_BOOT_TRACE_PATH,
				  "sd:/previous_cw_stage_boot.txt") == 0);

	expect_path(26, "sd:/cw_tel_trace.txt");
	expect_path(27, "sd:/config.json");
	expect_path(28, "sd:/config_wii.json");

	for(size_t i = 0; i < CW_STAGE_MANIFEST_PATH_COUNT; ++i)
		assert(path_count(CW_STAGE_MANIFEST_PATHS[i]) == 1);
	return 0;
}

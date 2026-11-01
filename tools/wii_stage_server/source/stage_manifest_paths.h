#ifndef CAVEWII_STAGE_MANIFEST_PATHS_H
#define CAVEWII_STAGE_MANIFEST_PATHS_H

/* The stage server's own per-boot phase trace (main.c boot_trace()). Named
 * here rather than in main.c so the allow-list, the manifest and the writer
 * cannot drift apart. */
#define CW_STAGE_BOOT_TRACE_PATH "sd:/cw_stage_boot.txt"

/* Where boot_trace_begin() rotates the PREVIOUS boot's trace to before
 * truncating CW_STAGE_BOOT_TRACE_PATH for the new one (main.c
 * boot_trace_rotate()). Mirrors the "previous_" convention
 * tools/run_staged_wii_hardware.ps1's Get-StageMailboxOutName already uses
 * for the game's sd:/cw_boot_trace.txt -> previous_cw_boot_trace.txt, except
 * here the rotation happens ON-DEVICE at boot rather than at collector
 * fetch-time -- the exit-reason line (run_launch()/wait_exit(), written
 * AFTER the listen socket closes) would otherwise never survive to the next
 * boot's truncate. */
#define CW_STAGE_PREV_BOOT_TRACE_PATH "sd:/previous_cw_stage_boot.txt"

static const char* const CW_STAGE_MANIFEST_PATHS[] = {
	"sd:/CAVEWII_PROOF_LAUNCHER.json",
	"sd:/apps/CaveWiiStage/proof_launcher_manifest.json",
	"sd:/apps/CavEX/boot.dol",
	"sd:/apps/CavEX/cw_run_args.txt",
	"sd:/cw_boot_trace.txt",
	"sd:/hang_report.txt",
	"sd:/fbguard_report.txt",
	/* App-dir FALLBACK reports (source/platform/hw_telemetry.h,
	   HW_TELEMETRY_*_REPORT_FALLBACK_PATH). The watchdog writes these when the sd:/ root
	   open fails -- a full FAT root, or a root-only write error -- which is exactly the
	   boot on which the two paths above hold nothing. Absent from this manifest the
	   fallback was a file nobody ever fetched, i.e. not a fallback at all. */
	"sd:/apps/CavEX/hang_report.txt",
	"sd:/apps/CavEX/fbguard_report.txt",
	/* SEQ-SUFFIXED hang reports, root then app-dir fallback (source/platform/watchdog.h,
	   wd_report_path_for_seq + WD_REPORT_SEQ_MAX = 8). wd_write_report opens with
	   fopen("w"), which truncates, so before 2026-07-29 report N destroyed report N-1 and
	   only the LAST stall of a boot survived -- the 2026-07-29 card read seq=6, i.e. five
	   reports lost. Reports 2..8 now carry the suffix; report 1 keeps the canonical name
	   above. Listing them here is what makes them fetchable at all: a report the manifest
	   does not name is a file no collector can ever pull, the same defect the fallback
	   entries above exist to fix. KEEP IN STEP with WD_REPORT_SEQ_MAX. */
	"sd:/hang_report_2.txt",
	"sd:/hang_report_3.txt",
	"sd:/hang_report_4.txt",
	"sd:/hang_report_5.txt",
	"sd:/hang_report_6.txt",
	"sd:/hang_report_7.txt",
	"sd:/hang_report_8.txt",
	"sd:/apps/CavEX/hang_report_2.txt",
	"sd:/apps/CavEX/hang_report_3.txt",
	"sd:/apps/CavEX/hang_report_4.txt",
	"sd:/apps/CavEX/hang_report_5.txt",
	"sd:/apps/CavEX/hang_report_6.txt",
	"sd:/apps/CavEX/hang_report_7.txt",
	"sd:/apps/CavEX/hang_report_8.txt",
	"sd:/lighting_bench.txt",
	/* The STAGE SERVER's own boot trace (main.c boot_trace(), truncated once
	 * per boot). Added 2026-09-02 after a session in which some boots never
	 * opened :39019 and left NO artifact at all to explain why: every phase
	 * message went to the TV console, and the card was not even mounted
	 * while the network was being configured. Listing it here is what makes
	 * it fetchable -- a report the manifest does not name is a file no
	 * collector can ever pull, the same reasoning as the fallback and
	 * seq-suffixed entries above. */
	CW_STAGE_BOOT_TRACE_PATH,
	/* The rotated copy of the line above (main.c boot_trace_rotate()), holding
	 * the PRIOR boot's trace -- including its final "exit: ..." line, which
	 * is written after the listen socket closes and would otherwise be wiped
	 * by the very next boot's truncate before anyone could fetch it. Added
	 * 2026-09-02. KEEP IN STEP with CW_STAGE_PREV_BOOT_TRACE_PATH. */
	CW_STAGE_PREV_BOOT_TRACE_PATH,
	"sd:/cw_tel_trace.txt",
	/* The LIVE game config at the SD root and its default (2026-09-17, maintainer
	   order "I call bullshit on config"): listing them here makes them fetch/put-able
	   through the stage protocol AND reports their hash in the manifest, so a
	   controls refresh can be proven from the card instead of ejecting it. The old
	   denial protected nothing: a LAN peer can already replace boot.dol under
	   sd:/apps/ and CWRUN1-launch it. The game reads "config.json" CWD-relative;
	   which CWD the loader leaves (SD root vs the app dir) is UNPROVEN, so a
	   controls refresh writes BOTH sd:/config.json and the sd:/apps/CavEX
	   copy (provision_wii_sd.ps1 writes both too). */
	"sd:/config.json",
	"sd:/config_wii.json",
};

#define CW_STAGE_MANIFEST_PATH_COUNT                                         \
	(sizeof(CW_STAGE_MANIFEST_PATHS) / sizeof(CW_STAGE_MANIFEST_PATHS[0]))

#endif

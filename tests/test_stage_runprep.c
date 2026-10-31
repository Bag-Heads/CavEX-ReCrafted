#include "dol_launch.h"
#include "dol_runprep.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void put_be32(unsigned char* p, uint32_t v) {
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void base_dol(unsigned char h[CW_DOL_HEADER_SIZE]) {
	memset(h, 0, CW_DOL_HEADER_SIZE);
	put_be32(h + 0x00, 0x100);		   /* text offset 0 */
	put_be32(h + 0x48, 0x80004000);	   /* text address 0 */
	put_be32(h + 0x90, 0x1000);		   /* text length 0 */
	put_be32(h + 0xd8, 0x80006000);	   /* bss address */
	put_be32(h + 0xdc, 0x2000);		   /* bss length */
	put_be32(h + 0xe0, 0x80004020);	   /* entry point */
}

static void expect_reason(const unsigned char h[CW_DOL_HEADER_SIZE],
						  unsigned long file_size, const char* expected) {
	cw_dol_info info;
	char reason[64];
	assert(!cw_dol_validate_header(h, file_size, &info, reason, sizeof(reason)));
	assert(strcmp(reason, expected) == 0);
}

static void test_dol_accepts_basic_wii_dol(void) {
	unsigned char h[CW_DOL_HEADER_SIZE];
	cw_dol_info info;
	cw_dol_launch_plan plan;
	char reason[64];
	base_dol(h);
	assert(cw_dol_validate_header(h, 0x2000, &info, reason, sizeof(reason)));
	assert(info.entry_point == 0x80004020u);
	assert(info.min_address == 0x80004000u);
	assert(info.max_address == 0x80008000u);
	assert(info.text_count == 1);
	assert(info.data_count == 0);
	assert(info.section_count == 2);
	assert(cw_dol_build_launch_plan(
		h, 0x2000,
		"--cw-run-id r42 --cw-tel-host pull --cw-tel-mode pull\n",
		strlen("--cw-run-id r42 --cw-tel-host pull --cw-tel-mode pull\n"),
		&plan, reason, sizeof(reason)));
	assert(plan.load_section_count == 1);
	assert(plan.load_sections[0].file_offset == 0x100u);
	assert(plan.load_sections[0].address == 0x80004000u);
	assert(plan.argv_marker_address == 0x80004024u);
	assert(plan.argv_marker_file_offset == 0x124u);
	assert(plan.argv_struct_address == 0x80004028u);
	assert(plan.argv_struct_bytes == CW_DOL_ARGV_STRUCT_BYTES);
	/* past the image end (bss ends 0x80008000), NEVER inside it: crt0
	 * zeroes .bss before stock libogc reads argv (chip task_580cb885) */
	assert(plan.command_line_address == 0x80008000u);
	assert(plan.argv_argc == 7);
}

static void test_dol_rejects_bad_spans(void) {
	unsigned char h[CW_DOL_HEADER_SIZE];
	base_dol(h);
	expect_reason(h, 0xff, "dol_too_small");

	base_dol(h);
	put_be32(h + 0x00, 0x80);
	expect_reason(h, 0x2000, "section_offset_before_header");

	base_dol(h);
	put_be32(h + 0x90, 0x2000);
	expect_reason(h, 0x2000, "section_out_of_file");
}

static void test_dol_rejects_unsafe_memory(void) {
	unsigned char h[CW_DOL_HEADER_SIZE];
	base_dol(h);
	put_be32(h + 0x48, 0x7ffff000);
	expect_reason(h, 0x2000, "section_address_out_of_range");

	base_dol(h);
	put_be32(h + 0x48, 0x93ffff00);
	put_be32(h + 0x90, 0x200);
	expect_reason(h, 0x2000, "section_address_out_of_range");
}

static void test_dol_rejects_overlap_and_bad_entry(void) {
	unsigned char h[CW_DOL_HEADER_SIZE];
	base_dol(h);
	put_be32(h + 0x1c, 0x1200);		  /* data offset 0 */
	put_be32(h + 0x64, 0x80004800);	  /* data address 0 */
	put_be32(h + 0xac, 0x100);		  /* data length 0 */
	expect_reason(h, 0x2000, "overlapping_sections");

	base_dol(h);
	put_be32(h + 0xe0, 0x80006020);
	expect_reason(h, 0x2000, "entry_not_in_text");
}

static void expect_args_reason(const char* text, size_t size,
							   const char* expected) {
	cw_run_args_info info;
	char reason[64];
	assert(!cw_run_args_validate_text(text, size, &info, reason,
									 sizeof(reason)));
	assert(strcmp(reason, expected) == 0);
}

static void test_args_validation(void) {
	cw_run_args_info info;
	uint32_t argc = 0;
	char reason[64];
	char command_line[128];
	size_t command_line_size = 0;
	const char* ok =
		"--cw-run-id r42 --cw-tel-host pull --cw-tel-mode pull\n";
	assert(cw_run_args_validate_text(ok, strlen(ok), &info, reason,
									 sizeof(reason)));
	assert(info.argc == 7);
	assert(cw_run_args_build_command_line(ok, strlen(ok), command_line,
										  sizeof(command_line),
										  &command_line_size, &argc, reason,
										  sizeof(reason)));
	assert(argc == 7);
	const char expected[] =
		"boot.dol\0--cw-run-id\0r42\0--cw-tel-host\0pull\0"
		"--cw-tel-mode\0pull\0";
	assert(command_line_size == sizeof(expected));
	assert(memcmp(command_line, expected, sizeof(expected)) == 0);

	expect_args_reason("", 0, "args_empty");
	expect_args_reason("# comment only\n", strlen("# comment only\n"),
					   "args_no_tokens");
	const char nul_arg[] = {'-', '-', 'x', '\0', 'y'};
	expect_args_reason(nul_arg, sizeof(nul_arg), "args_contains_nul");

	char too_large[CW_RUN_ARGS_MAX_BYTES + 2];
	memset(too_large, 'a', sizeof(too_large));
	expect_args_reason(too_large, sizeof(too_large), "args_too_large");

	char many[256];
	size_t pos = 0;
	for(int i = 0; i < 32; ++i)
		pos += (size_t)snprintf(many + pos, sizeof(many) - pos, "a ");
	expect_args_reason(many, pos, "args_too_many_tokens");
}

static void test_address_mapping(void) {
	unsigned char h[CW_DOL_HEADER_SIZE];
	uint32_t off = 0;
	base_dol(h);
	assert(cw_dol_file_offset_for_address(h, 0x2000, 0x80004000u, 4u, &off));
	assert(off == 0x100u);
	assert(cw_dol_file_offset_for_address(h, 0x2000, 0x80004ffcu, 4u, &off));
	assert(off == 0x10fcu);
	assert(!cw_dol_file_offset_for_address(h, 0x2000, 0x80004ffcu, 8u, &off));
	assert(!cw_dol_file_offset_for_address(h, 0x2000, 0x80006000u, 4u, &off));
}

static void test_launch_plan_places_args_past_image_end(void) {
	unsigned char h[CW_DOL_HEADER_SIZE];
	cw_dol_launch_plan plan;
	char reason[64];
	const char* args = "--cw-run-id r42\n";

	/* no bss: args land 32-aligned past the last section end */
	base_dol(h);
	put_be32(h + 0xdc, 0);
	assert(cw_dol_build_launch_plan(h, 0x2000, args, strlen(args), &plan,
									reason, sizeof(reason)));
	assert(plan.command_line_address == 0x80005000u);

	/* tiny bss no longer bounds the args (they never lived there) */
	base_dol(h);
	put_be32(h + 0xdc, 4);
	assert(cw_dol_build_launch_plan(h, 0x2000, args, strlen(args), &plan,
									reason, sizeof(reason)));
	assert(plan.command_line_address == 0x80006020u);

	/* image ending flush against the top of Wii memory leaves no room */
	base_dol(h);
	put_be32(h + 0x1c, 0x1000);		  /* data offset 0 */
	put_be32(h + 0x64, 0x93fff000);	  /* data address 0 */
	put_be32(h + 0xac, 0xff0);		  /* data length 0 */
	assert(!cw_dol_build_launch_plan(h, 0x2000, args, strlen(args), &plan,
									 reason, sizeof(reason)));
	assert(strcmp(reason, "argv_command_line_no_space") == 0);
}

static void test_launch_context_builds_loader_contract(void) {
	unsigned char h[CW_DOL_HEADER_SIZE];
	char reason[64];
	cw_dol_launch_context ctx;
	const char* args =
		"--cw-run-id r42 --cw-tel-host pull --cw-tel-mode pull\n";
	base_dol(h);
	assert(cw_dol_prepare_launch_context(
		h, 0x2000, args, strlen(args), 0x91200000u, 0x91300000u, &ctx,
		reason, sizeof(reason)));
	assert(ctx.magic == CW_DOL_LAUNCH_CONTEXT_MAGIC);
	assert(ctx.section_count == 1);
	assert(ctx.entry_point == 0x80004020u);
	assert(ctx.min_address == 0x80004000u);
	/* max stretches past image end (0x80008000) to cover the staged
	 * command line so the stub's cache sweep flushes it too */
	assert(ctx.max_address == 0x80008000u + 64u);
	assert(ctx.bss_address == 0x80006000u);
	assert(ctx.bss_length == 0x2000u);
	assert(ctx.argv_struct_address == 0x80004028u);
	assert(ctx.command_line_address == 0x80008000u);
	assert(ctx.argv_argc == 7);
	assert(ctx.dol_image_address == 0x91200000u);
	assert(ctx.stack_top == 0x91300000u);
	assert(ctx.sections[0].file_offset == 0x100u);
	assert(ctx.sections[0].address == 0x80004000u);
	assert(ctx.sections[0].length == 0x1000u);
	assert(ctx.sections[0].is_text == 1u);
	const char expected[] =
		"boot.dol\0--cw-run-id\0r42\0--cw-tel-host\0pull\0"
		"--cw-tel-mode\0pull\0";
	assert(ctx.command_line_bytes == sizeof(expected));
	assert(memcmp(ctx.command_line, expected, sizeof(expected)) == 0);
	assert(offsetof(cw_dol_launch_context, sections) ==
		   CW_LAUNCH_CTX_OFF_SECTIONS);
	assert(offsetof(cw_dol_launch_context, command_line) ==
		   CW_LAUNCH_CTX_OFF_COMMAND_LINE);
	assert(sizeof(ctx.sections[0]) == CW_LAUNCH_SECTION_BYTES);
}

static void test_launch_context_rejects_storage_overlap(void) {
	unsigned char h[CW_DOL_HEADER_SIZE];
	char reason[64];
	cw_dol_launch_context ctx;
	const char* args = "--cw-run-id r42\n";
	base_dol(h);
	assert(cw_dol_prepare_launch_context(
		h, 0x2000, args, strlen(args), 0x91200000u, 0x91300000u, &ctx,
		reason, sizeof(reason)));
	assert(cw_dol_launch_reserved_range_is_safe(
		&ctx, 0x91200000u, 0x00100000u, reason, sizeof(reason)));
	assert(!cw_dol_launch_reserved_range_is_safe(
		&ctx, 0x80004800u, 0x1000u, reason, sizeof(reason)));
	assert(strcmp(reason, "launch_storage_overlaps_dol") == 0);
	assert(!cw_dol_launch_reserved_range_is_safe(
		&ctx, 0x80006000u, 0x1000u, reason, sizeof(reason)));
	assert(strcmp(reason, "launch_storage_overlaps_bss") == 0);
	assert(!cw_dol_launch_reserved_range_is_safe(
		&ctx, 0x80008000u, 0x20u, reason, sizeof(reason)));
	assert(strcmp(reason, "launch_storage_overlaps_argv") == 0);
}

int main(void) {
	test_dol_accepts_basic_wii_dol();
	test_dol_rejects_bad_spans();
	test_dol_rejects_unsafe_memory();
	test_dol_rejects_overlap_and_bad_entry();
	test_args_validation();
	test_address_mapping();
	test_launch_plan_places_args_past_image_end();
	test_launch_context_builds_loader_contract();
	test_launch_context_rejects_storage_overlap();
	return 0;
}

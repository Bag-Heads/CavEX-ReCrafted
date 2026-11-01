#include "dol_launch.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define STATIC_ASSERT(name, expr) typedef char name[(expr) ? 1 : -1]

STATIC_ASSERT(cw_ctx_magic_offset,
			  offsetof(cw_dol_launch_context, magic) ==
				  CW_LAUNCH_CTX_OFF_MAGIC);
STATIC_ASSERT(cw_ctx_sections_offset,
			  offsetof(cw_dol_launch_context, sections) ==
				  CW_LAUNCH_CTX_OFF_SECTIONS);
STATIC_ASSERT(cw_ctx_command_line_offset,
			  offsetof(cw_dol_launch_context, command_line) ==
				  CW_LAUNCH_CTX_OFF_COMMAND_LINE);
STATIC_ASSERT(cw_section_file_offset,
			  offsetof(cw_dol_load_section, file_offset) ==
				  CW_LAUNCH_SECTION_OFF_FILE_OFFSET);
STATIC_ASSERT(cw_section_address_offset,
			  offsetof(cw_dol_load_section, address) ==
				  CW_LAUNCH_SECTION_OFF_ADDRESS);
STATIC_ASSERT(cw_section_length_offset,
			  offsetof(cw_dol_load_section, length) ==
				  CW_LAUNCH_SECTION_OFF_LENGTH);
STATIC_ASSERT(cw_section_text_offset,
			  offsetof(cw_dol_load_section, is_text) ==
				  CW_LAUNCH_SECTION_OFF_IS_TEXT);

static void set_reason(char* reason, size_t reason_size, const char* fmt, ...) {
	if(!reason || reason_size == 0)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(reason, reason_size, fmt, ap);
	va_end(ap);
}

static int range_end(uint32_t start, uint32_t len, uint32_t* end) {
	if(len > 0xffffffffu - start)
		return 0;
	*end = start + len;
	return 1;
}

static int ranges_overlap(uint32_t a0, uint32_t a1, uint32_t b0,
						  uint32_t b1) {
	return a0 < b1 && b0 < a1;
}

int cw_dol_prepare_launch_context(
	const unsigned char header[CW_DOL_HEADER_SIZE], unsigned long file_size,
	const char* args_text, size_t args_size, uint32_t dol_image_address,
	uint32_t stack_top, cw_dol_launch_context* out, char* reason,
	size_t reason_size) {
	if(reason && reason_size)
		reason[0] = '\0';
	if(out)
		memset(out, 0, sizeof(*out));
	if(!out) {
		set_reason(reason, reason_size, "bad_argument");
		return 0;
	}
	if(dol_image_address == 0 || stack_top == 0) {
		set_reason(reason, reason_size, "bad_launch_storage");
		return 0;
	}

	cw_dol_launch_plan plan;
	if(!cw_dol_build_launch_plan(header, file_size, args_text, args_size,
								 &plan, reason, reason_size))
		return 0;

	size_t command_line_size = 0;
	uint32_t argc = 0;
	if(!cw_run_args_build_command_line(
		   args_text, args_size, out->command_line, sizeof(out->command_line),
		   &command_line_size, &argc, reason, reason_size))
		return 0;
	if(command_line_size != plan.command_line_bytes ||
	   argc != plan.argv_argc) {
		set_reason(reason, reason_size, "launch_context_drift");
		return 0;
	}

	uint32_t command_line_end = 0;
	if(!range_end(plan.command_line_address, plan.command_line_bytes,
				  &command_line_end)) {
		set_reason(reason, reason_size, "argv_address_overflow");
		return 0;
	}

	out->magic = CW_DOL_LAUNCH_CONTEXT_MAGIC;
	out->section_count = plan.load_section_count;
	out->entry_point = plan.dol.entry_point;
	out->min_address = plan.dol.min_address;
	/* min/max feed the loader stub's dcbst/icbi sweep; the staged command
	 * line sits just PAST the image end, so stretch the sweep over it. */
	out->max_address = plan.dol.max_address > command_line_end
						   ? plan.dol.max_address
						   : command_line_end;
	out->bss_address = plan.dol.bss_address;
	out->bss_length = plan.dol.bss_length;
	out->argv_struct_address = plan.argv_struct_address;
	out->command_line_address = plan.command_line_address;
	out->command_line_bytes = plan.command_line_bytes;
	out->argv_argc = plan.argv_argc;
	out->dol_image_address = dol_image_address;
	out->stack_top = stack_top;
	for(uint32_t i = 0; i < plan.load_section_count; ++i)
		out->sections[i] = plan.load_sections[i];
	return 1;
}

int cw_dol_launch_reserved_range_is_safe(
	const cw_dol_launch_context* ctx, uint32_t reserve_start,
	uint32_t reserve_bytes, char* reason, size_t reason_size) {
	if(reason && reason_size)
		reason[0] = '\0';
	if(!ctx) {
		set_reason(reason, reason_size, "bad_argument");
		return 0;
	}
	uint32_t reserve_end = 0;
	if(!range_end(reserve_start, reserve_bytes, &reserve_end)) {
		set_reason(reason, reason_size, "launch_storage_overflow");
		return 0;
	}
	for(uint32_t i = 0; i < ctx->section_count; ++i) {
		uint32_t section_end = 0;
		if(!range_end(ctx->sections[i].address, ctx->sections[i].length,
					  &section_end)) {
			set_reason(reason, reason_size, "section_address_overflow");
			return 0;
		}
		if(ranges_overlap(ctx->sections[i].address, section_end,
						  reserve_start, reserve_end)) {
			set_reason(reason, reason_size, "launch_storage_overlaps_dol");
			return 0;
		}
	}
	uint32_t bss_end = 0;
	if(ctx->bss_length &&
	   !range_end(ctx->bss_address, ctx->bss_length, &bss_end)) {
		set_reason(reason, reason_size, "bss_address_overflow");
		return 0;
	}
	if(ctx->bss_length &&
	   ranges_overlap(ctx->bss_address, bss_end, reserve_start, reserve_end)) {
		set_reason(reason, reason_size, "launch_storage_overlaps_bss");
		return 0;
	}
	uint32_t argv_end = 0;
	if(ctx->command_line_bytes &&
	   !range_end(ctx->command_line_address, ctx->command_line_bytes,
				  &argv_end)) {
		set_reason(reason, reason_size, "argv_address_overflow");
		return 0;
	}
	if(ctx->command_line_bytes &&
	   ranges_overlap(ctx->command_line_address, argv_end, reserve_start,
					  reserve_end)) {
		set_reason(reason, reason_size, "launch_storage_overlaps_argv");
		return 0;
	}
	return 1;
}

#ifndef CAVEWII_STAGE_DOL_LAUNCH_H
#define CAVEWII_STAGE_DOL_LAUNCH_H

#ifndef __ASSEMBLER__
#include "dol_runprep.h"

#include <stddef.h>
#include <stdint.h>

#define CW_DOL_COMMAND_LINE_MAX_BYTES \
	(CW_RUN_ARGS_MAX_BYTES + sizeof(CW_RUN_ARGV0) + 2u)
#else
#define CW_DOL_MAX_SECTIONS 19
#define CW_DOL_ARGV_MAGIC 0x5f617267
#endif

#define CW_DOL_LAUNCH_CONTEXT_MAGIC 0x43575231u
#define CW_DOL_LAUNCH_STACK_BYTES 32768u

#define CW_LAUNCH_CTX_OFF_MAGIC 0u
#define CW_LAUNCH_CTX_OFF_SECTION_COUNT 4u
#define CW_LAUNCH_CTX_OFF_ENTRY_POINT 8u
#define CW_LAUNCH_CTX_OFF_MIN_ADDRESS 12u
#define CW_LAUNCH_CTX_OFF_MAX_ADDRESS 16u
#define CW_LAUNCH_CTX_OFF_BSS_ADDRESS 20u
#define CW_LAUNCH_CTX_OFF_BSS_LENGTH 24u
#define CW_LAUNCH_CTX_OFF_ARGV_STRUCT_ADDRESS 28u
#define CW_LAUNCH_CTX_OFF_COMMAND_LINE_ADDRESS 32u
#define CW_LAUNCH_CTX_OFF_COMMAND_LINE_BYTES 36u
#define CW_LAUNCH_CTX_OFF_ARGV_ARGC 40u
#define CW_LAUNCH_CTX_OFF_DOL_IMAGE_ADDRESS 44u
#define CW_LAUNCH_CTX_OFF_STACK_TOP 48u
#define CW_LAUNCH_CTX_OFF_RESERVED 52u
#define CW_LAUNCH_CTX_OFF_SECTIONS 56u
#define CW_LAUNCH_SECTION_BYTES 16u
#define CW_LAUNCH_SECTION_OFF_FILE_OFFSET 0u
#define CW_LAUNCH_SECTION_OFF_ADDRESS 4u
#define CW_LAUNCH_SECTION_OFF_LENGTH 8u
#define CW_LAUNCH_SECTION_OFF_IS_TEXT 12u
#define CW_LAUNCH_CTX_OFF_COMMAND_LINE \
	(CW_LAUNCH_CTX_OFF_SECTIONS + CW_DOL_MAX_SECTIONS * CW_LAUNCH_SECTION_BYTES)

#ifndef __ASSEMBLER__
typedef struct cw_dol_launch_context {
	uint32_t magic;
	uint32_t section_count;
	uint32_t entry_point;
	uint32_t min_address;
	uint32_t max_address;
	uint32_t bss_address;
	uint32_t bss_length;
	uint32_t argv_struct_address;
	uint32_t command_line_address;
	uint32_t command_line_bytes;
	uint32_t argv_argc;
	uint32_t dol_image_address;
	uint32_t stack_top;
	uint32_t reserved;
	cw_dol_load_section sections[CW_DOL_MAX_SECTIONS];
	char command_line[CW_DOL_COMMAND_LINE_MAX_BYTES];
} cw_dol_launch_context;

int cw_dol_prepare_launch_context(
	const unsigned char header[CW_DOL_HEADER_SIZE], unsigned long file_size,
	const char* args_text, size_t args_size, uint32_t dol_image_address,
	uint32_t stack_top, cw_dol_launch_context* out, char* reason,
	size_t reason_size);

int cw_dol_launch_reserved_range_is_safe(
	const cw_dol_launch_context* ctx, uint32_t reserve_start,
	uint32_t reserve_bytes, char* reason, size_t reason_size);

#endif

#endif

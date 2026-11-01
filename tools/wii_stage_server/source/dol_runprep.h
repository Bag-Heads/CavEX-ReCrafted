#ifndef CAVEWII_STAGE_DOL_RUNPREP_H
#define CAVEWII_STAGE_DOL_RUNPREP_H

#include <stddef.h>
#include <stdint.h>

#define CW_DOL_HEADER_SIZE 256u
#define CW_DOL_MAX_SECTIONS 19u
#define CW_RUN_ARGS_MAX_BYTES 511u
#define CW_RUN_ARGS_MAX_ARGC 32u
#define CW_DOL_ARGV_MAGIC 0x5f617267u
#define CW_DOL_ARGV_MARKER_OFFSET 4u
#define CW_DOL_ARGV_STRUCT_OFFSET 8u
#define CW_DOL_ARGV_STRUCT_BYTES 24u
#define CW_RUN_ARGV0 "boot.dol"

typedef struct cw_dol_info {
	uint32_t entry_point;
	uint32_t min_address;
	uint32_t max_address;
	uint32_t text_count;
	uint32_t data_count;
	uint32_t section_count;
	uint32_t bss_address;
	uint32_t bss_length;
} cw_dol_info;

typedef struct cw_run_args_info {
	uint32_t argc;
} cw_run_args_info;

typedef struct cw_dol_load_section {
	uint32_t file_offset;
	uint32_t address;
	uint32_t length;
	uint32_t is_text;
} cw_dol_load_section;

typedef struct cw_dol_launch_plan {
	cw_dol_info dol;
	cw_dol_load_section load_sections[CW_DOL_MAX_SECTIONS];
	uint32_t load_section_count;
	uint32_t argv_marker_address;
	uint32_t argv_marker_file_offset;
	uint32_t argv_struct_address;
	uint32_t argv_struct_bytes;
	uint32_t command_line_address;
	uint32_t command_line_bytes;
	uint32_t argv_argc;
} cw_dol_launch_plan;

int cw_dol_validate_header(const unsigned char header[CW_DOL_HEADER_SIZE],
						   unsigned long file_size, cw_dol_info* out,
						   char* reason, size_t reason_size);

int cw_dol_file_offset_for_address(
	const unsigned char header[CW_DOL_HEADER_SIZE], unsigned long file_size,
	uint32_t address, uint32_t length, uint32_t* out_offset);

int cw_run_args_validate_text(const char* text, size_t size,
							 cw_run_args_info* out, char* reason,
							 size_t reason_size);

int cw_run_args_build_command_line(const char* text, size_t size,
								   char* out, size_t out_capacity,
								   size_t* out_size, uint32_t* out_argc,
								   char* reason, size_t reason_size);

int cw_dol_build_launch_plan(
	const unsigned char header[CW_DOL_HEADER_SIZE], unsigned long file_size,
	const char* args_text, size_t args_size, cw_dol_launch_plan* out,
	char* reason, size_t reason_size);

#endif

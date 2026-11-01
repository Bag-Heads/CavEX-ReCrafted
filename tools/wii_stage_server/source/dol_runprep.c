#include "dol_runprep.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DOL_TEXT_COUNT 7u
#define DOL_DATA_COUNT 11u
#define DOL_TEXT_OFFSET_BASE 0u
#define DOL_DATA_OFFSET_BASE (DOL_TEXT_OFFSET_BASE + DOL_TEXT_COUNT * 4u)
#define DOL_TEXT_ADDR_BASE (DOL_DATA_OFFSET_BASE + DOL_DATA_COUNT * 4u)
#define DOL_DATA_ADDR_BASE (DOL_TEXT_ADDR_BASE + DOL_TEXT_COUNT * 4u)
#define DOL_TEXT_LEN_BASE (DOL_DATA_ADDR_BASE + DOL_DATA_COUNT * 4u)
#define DOL_DATA_LEN_BASE (DOL_TEXT_LEN_BASE + DOL_TEXT_COUNT * 4u)
#define DOL_BSS_ADDR_OFFSET (DOL_DATA_LEN_BASE + DOL_DATA_COUNT * 4u)
#define DOL_BSS_LEN_OFFSET (DOL_BSS_ADDR_OFFSET + 4u)
#define DOL_ENTRY_OFFSET (DOL_BSS_LEN_OFFSET + 4u)

typedef struct cw_mem_range {
	uint32_t start;
	uint32_t end;
	int is_text;
} cw_mem_range;

typedef struct cw_raw_section {
	uint32_t file_offset;
	uint32_t address;
	uint32_t length;
	int is_text;
} cw_raw_section;

static void set_reason(char* reason, size_t reason_size, const char* fmt, ...) {
	if(!reason || reason_size == 0)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(reason, reason_size, fmt, ap);
	va_end(ap);
}

static uint32_t read_be32(const unsigned char* p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int range_end(uint32_t start, uint32_t len, uint32_t* end) {
	if(len > 0xffffffffu - start)
		return 0;
	*end = start + len;
	return 1;
}

static int range_is_wii_memory(uint32_t start, uint32_t end) {
	return start >= 0x80000000u && end > start && end <= 0x94000000u;
}

static int ranges_overlap(uint32_t a0, uint32_t a1, uint32_t b0,
						  uint32_t b1) {
	return a0 < b1 && b0 < a1;
}

static uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
	return (value + alignment - 1u) & ~(alignment - 1u);
}

static int add_range(cw_mem_range* ranges, uint32_t* count, uint32_t start,
					 uint32_t end, int is_text, char* reason,
					 size_t reason_size) {
	if(*count >= CW_DOL_MAX_SECTIONS) {
		set_reason(reason, reason_size, "too_many_sections");
		return 0;
	}
	for(uint32_t i = 0; i < *count; ++i) {
		if(ranges_overlap(start, end, ranges[i].start, ranges[i].end)) {
			set_reason(reason, reason_size, "overlapping_sections");
			return 0;
		}
	}
	ranges[*count].start = start;
	ranges[*count].end = end;
	ranges[*count].is_text = is_text;
	(*count)++;
	return 1;
}

static int validate_file_span(uint32_t offset, uint32_t len,
							  unsigned long file_size, char* reason,
							  size_t reason_size) {
	if(offset < CW_DOL_HEADER_SIZE) {
		set_reason(reason, reason_size, "section_offset_before_header");
		return 0;
	}
	if((unsigned long)offset > file_size ||
	   (unsigned long)len > file_size - (unsigned long)offset) {
		set_reason(reason, reason_size, "section_out_of_file");
		return 0;
	}
	return 1;
}

static int get_section(const unsigned char header[CW_DOL_HEADER_SIZE],
					   uint32_t linear_index, cw_raw_section* out) {
	if(linear_index < DOL_TEXT_COUNT) {
		uint32_t i = linear_index;
		out->file_offset = read_be32(header + DOL_TEXT_OFFSET_BASE + i * 4u);
		out->address = read_be32(header + DOL_TEXT_ADDR_BASE + i * 4u);
		out->length = read_be32(header + DOL_TEXT_LEN_BASE + i * 4u);
		out->is_text = 1;
		return 1;
	}
	linear_index -= DOL_TEXT_COUNT;
	if(linear_index < DOL_DATA_COUNT) {
		uint32_t i = linear_index;
		out->file_offset = read_be32(header + DOL_DATA_OFFSET_BASE + i * 4u);
		out->address = read_be32(header + DOL_DATA_ADDR_BASE + i * 4u);
		out->length = read_be32(header + DOL_DATA_LEN_BASE + i * 4u);
		out->is_text = 0;
		return 1;
	}
	return 0;
}

static int token_is_space(unsigned char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int cw_dol_validate_header(const unsigned char header[CW_DOL_HEADER_SIZE],
						   unsigned long file_size, cw_dol_info* out,
						   char* reason, size_t reason_size) {
	if(reason && reason_size)
		reason[0] = '\0';
	if(out)
		memset(out, 0, sizeof(*out));
	if(!header || !out) {
		set_reason(reason, reason_size, "bad_argument");
		return 0;
	}
	if(file_size < CW_DOL_HEADER_SIZE) {
		set_reason(reason, reason_size, "dol_too_small");
		return 0;
	}

	cw_mem_range ranges[CW_DOL_MAX_SECTIONS];
	uint32_t range_count = 0;
	uint32_t min_address = 0xffffffffu;
	uint32_t max_address = 0u;

	for(uint32_t kind = 0; kind < 2; ++kind) {
		uint32_t count = kind == 0 ? DOL_TEXT_COUNT : DOL_DATA_COUNT;
		uint32_t off_base = kind == 0 ? DOL_TEXT_OFFSET_BASE
									  : DOL_DATA_OFFSET_BASE;
		uint32_t addr_base = kind == 0 ? DOL_TEXT_ADDR_BASE
									   : DOL_DATA_ADDR_BASE;
		uint32_t len_base = kind == 0 ? DOL_TEXT_LEN_BASE : DOL_DATA_LEN_BASE;
		for(uint32_t i = 0; i < count; ++i) {
			uint32_t offset = read_be32(header + off_base + i * 4u);
			uint32_t addr = read_be32(header + addr_base + i * 4u);
			uint32_t len = read_be32(header + len_base + i * 4u);
			if(len == 0)
				continue;
			if(!validate_file_span(offset, len, file_size, reason, reason_size))
				return 0;
			uint32_t end = 0;
			if(!range_end(addr, len, &end)) {
				set_reason(reason, reason_size, "section_address_overflow");
				return 0;
			}
			if(!range_is_wii_memory(addr, end)) {
				set_reason(reason, reason_size, "section_address_out_of_range");
				return 0;
			}
			if(!add_range(ranges, &range_count, addr, end, kind == 0, reason,
						  reason_size))
				return 0;
			if(addr < min_address)
				min_address = addr;
			if(end > max_address)
				max_address = end;
			if(kind == 0)
				out->text_count++;
			else
				out->data_count++;
		}
	}

	if(range_count == 0) {
		set_reason(reason, reason_size, "no_load_sections");
		return 0;
	}

	uint32_t bss_addr = read_be32(header + DOL_BSS_ADDR_OFFSET);
	uint32_t bss_len = read_be32(header + DOL_BSS_LEN_OFFSET);
	if(bss_len) {
		uint32_t bss_end = 0;
		if(!range_end(bss_addr, bss_len, &bss_end)) {
			set_reason(reason, reason_size, "bss_address_overflow");
			return 0;
		}
		if(!range_is_wii_memory(bss_addr, bss_end)) {
			set_reason(reason, reason_size, "bss_address_out_of_range");
			return 0;
		}
		if(!add_range(ranges, &range_count, bss_addr, bss_end, 0, reason,
					  reason_size))
			return 0;
		if(bss_addr < min_address)
			min_address = bss_addr;
		if(bss_end > max_address)
			max_address = bss_end;
	}

	uint32_t entry = read_be32(header + DOL_ENTRY_OFFSET);
	int entry_ok = 0;
	for(uint32_t i = 0; i < range_count; ++i) {
		if(ranges[i].is_text && entry >= ranges[i].start &&
		   entry < ranges[i].end) {
			entry_ok = 1;
			break;
		}
	}
	if(!entry_ok) {
		set_reason(reason, reason_size, "entry_not_in_text");
		return 0;
	}

	out->entry_point = entry;
	out->min_address = min_address;
	out->max_address = max_address;
	out->section_count = range_count;
	out->bss_address = bss_addr;
	out->bss_length = bss_len;
	return 1;
}

int cw_dol_file_offset_for_address(
	const unsigned char header[CW_DOL_HEADER_SIZE], unsigned long file_size,
	uint32_t address, uint32_t length, uint32_t* out_offset) {
	if(out_offset)
		*out_offset = 0;
	if(!header || !out_offset)
		return 0;
	uint32_t end = 0;
	if(!range_end(address, length, &end))
		return 0;
	for(uint32_t i = 0; i < DOL_TEXT_COUNT + DOL_DATA_COUNT; ++i) {
		cw_raw_section sec;
		if(!get_section(header, i, &sec) || sec.length == 0)
			continue;
		uint32_t sec_end = 0;
		if(!range_end(sec.address, sec.length, &sec_end))
			continue;
		if(address >= sec.address && end <= sec_end) {
			uint32_t rel = address - sec.address;
			if(!validate_file_span(sec.file_offset, sec.length, file_size,
								   NULL, 0))
				return 0;
			if((unsigned long)rel > file_size - (unsigned long)sec.file_offset)
				return 0;
			*out_offset = sec.file_offset + rel;
			return 1;
		}
	}
	return 0;
}

int cw_run_args_validate_text(const char* text, size_t size,
							 cw_run_args_info* out, char* reason,
							 size_t reason_size) {
	if(reason && reason_size)
		reason[0] = '\0';
	if(out)
		memset(out, 0, sizeof(*out));
	if(!text || !out) {
		set_reason(reason, reason_size, "bad_argument");
		return 0;
	}
	if(size == 0) {
		set_reason(reason, reason_size, "args_empty");
		return 0;
	}
	if(size > CW_RUN_ARGS_MAX_BYTES) {
		set_reason(reason, reason_size, "args_too_large");
		return 0;
	}

	uint32_t argc = 1;
	int in_token = 0;
	for(size_t i = 0; i < size; ++i) {
		unsigned char c = (unsigned char)text[i];
		if(c == '\0') {
			set_reason(reason, reason_size, "args_contains_nul");
			return 0;
		}
		if(c < 0x20 && c != '\t' && c != '\r' && c != '\n') {
			set_reason(reason, reason_size, "args_control_char");
			return 0;
		}
		int is_ws = token_is_space(c);
		if(is_ws) {
			in_token = 0;
			continue;
		}
		if(!in_token) {
			if(c == '#')
				break;
			if(argc >= CW_RUN_ARGS_MAX_ARGC) {
				set_reason(reason, reason_size, "args_too_many_tokens");
				return 0;
			}
			argc++;
			in_token = 1;
		}
	}
	if(argc == 1) {
		set_reason(reason, reason_size, "args_no_tokens");
		return 0;
	}
	out->argc = argc;
	return 1;
}

int cw_run_args_build_command_line(const char* text, size_t size,
								   char* out, size_t out_capacity,
								   size_t* out_size, uint32_t* out_argc,
								   char* reason, size_t reason_size) {
	cw_run_args_info info;
	if(out_size)
		*out_size = 0;
	if(out_argc)
		*out_argc = 0;
	if(!cw_run_args_validate_text(text, size, &info, reason, reason_size))
		return 0;
	if(!out || !out_size || !out_argc) {
		set_reason(reason, reason_size, "bad_argument");
		return 0;
	}

	size_t pos = 0;
	size_t argv0_len = strlen(CW_RUN_ARGV0) + 1u;
	if(argv0_len + 1u > out_capacity) {
		set_reason(reason, reason_size, "args_output_too_small");
		return 0;
	}
	memcpy(out + pos, CW_RUN_ARGV0, argv0_len);
	pos += argv0_len;
	uint32_t argc = 1;

	size_t i = 0;
	while(i < size) {
		while(i < size && token_is_space((unsigned char)text[i]))
			++i;
		if(i >= size)
			break;
		if(text[i] == '#')
			break;
		size_t start = i;
		while(i < size && !token_is_space((unsigned char)text[i]))
			++i;
		size_t len = i - start;
		if(pos + len + 2u > out_capacity) {
			set_reason(reason, reason_size, "args_output_too_small");
			return 0;
		}
		memcpy(out + pos, text + start, len);
		pos += len;
		out[pos++] = '\0';
		argc++;
	}
	out[pos++] = '\0';
	*out_size = pos;
	*out_argc = argc;
	return 1;
}

static int entry_range_contains(const cw_dol_launch_plan* plan, uint32_t address,
								uint32_t length) {
	uint32_t end = 0;
	if(!range_end(address, length, &end))
		return 0;
	for(uint32_t i = 0; i < plan->load_section_count; ++i) {
		const cw_dol_load_section* sec = &plan->load_sections[i];
		if(sec->is_text && address >= sec->address &&
		   end <= sec->address + sec->length)
			return 1;
	}
	return 0;
}

int cw_dol_build_launch_plan(
	const unsigned char header[CW_DOL_HEADER_SIZE], unsigned long file_size,
	const char* args_text, size_t args_size, cw_dol_launch_plan* out,
	char* reason, size_t reason_size) {
	if(reason && reason_size)
		reason[0] = '\0';
	if(out)
		memset(out, 0, sizeof(*out));
	if(!out) {
		set_reason(reason, reason_size, "bad_argument");
		return 0;
	}
	if(!cw_dol_validate_header(header, file_size, &out->dol, reason,
							   reason_size))
		return 0;

	for(uint32_t i = 0; i < DOL_TEXT_COUNT + DOL_DATA_COUNT; ++i) {
		cw_raw_section sec;
		if(!get_section(header, i, &sec) || sec.length == 0)
			continue;
		cw_dol_load_section* dst =
			&out->load_sections[out->load_section_count++];
		dst->file_offset = sec.file_offset;
		dst->address = sec.address;
		dst->length = sec.length;
		dst->is_text = sec.is_text ? 1u : 0u;
	}

	out->argv_marker_address =
		out->dol.entry_point + CW_DOL_ARGV_MARKER_OFFSET;
	out->argv_struct_address =
		out->dol.entry_point + CW_DOL_ARGV_STRUCT_OFFSET;
	out->argv_struct_bytes = CW_DOL_ARGV_STRUCT_BYTES;
	if(!entry_range_contains(out, out->argv_marker_address, 4u) ||
	   !entry_range_contains(out, out->argv_struct_address,
							 CW_DOL_ARGV_STRUCT_BYTES)) {
		set_reason(reason, reason_size, "argv_slot_not_in_text");
		return 0;
	}
	if(!cw_dol_file_offset_for_address(header, file_size,
									   out->argv_marker_address, 4u,
									   &out->argv_marker_file_offset)) {
		set_reason(reason, reason_size, "argv_marker_unmapped");
		return 0;
	}

	char command_line[CW_RUN_ARGS_MAX_BYTES + sizeof(CW_RUN_ARGV0) + 2u];
	size_t command_line_bytes = 0;
	uint32_t argc = 0;
	if(!cw_run_args_build_command_line(args_text, args_size, command_line,
									   sizeof(command_line),
									   &command_line_bytes, &argc, reason,
									   reason_size))
		return 0;
	/* Loader-faithful argv placement (chip task_580cb885). The command
	 * line must still be intact when libogc's __CheckARGV shelters it to
	 * __Arena1Lo; stock libogc crt0 runs __CheckARGV only AFTER zeroing
	 * [.sbss,.bss], so anywhere inside the image is erased before delivery
	 * (the old start-of-.bss slot never delivered a byte -- the SD-file
	 * fallback did all the work). Place it just past the image end (max
	 * section/bss end), the same spot HBC/wiiload use, which both crt0
	 * generations preserve: v3.x shelters argv before its BSS clear, older
	 * crt0s zero only up to __bss_end. 32-byte aligned so the loader
	 * stub's cache-line flush owns whole lines. */
	uint32_t image_end = out->dol.max_address;
	uint32_t cmd_addr = align_up_u32(image_end, 32u);
	uint32_t cmd_end = 0;
	if(cmd_addr < image_end ||
	   !range_end(cmd_addr, (uint32_t)command_line_bytes, &cmd_end) ||
	   !range_is_wii_memory(cmd_addr, cmd_end)) {
		set_reason(reason, reason_size, "argv_command_line_no_space");
		return 0;
	}
	out->command_line_address = cmd_addr;
	out->command_line_bytes = (uint32_t)command_line_bytes;
	out->argv_argc = argc;
	return 1;
}

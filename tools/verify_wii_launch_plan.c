#include "dol_runprep.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int file_size_bytes(const char* path, unsigned long* out_size) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return 0;
	if(fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return 0;
	}
	long pos = ftell(f);
	fclose(f);
	if(pos < 0)
		return 0;
	*out_size = (unsigned long)pos;
	return 1;
}

static int read_exact_prefix(const char* path, unsigned char* dst,
							 size_t bytes) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return 0;
	size_t n = fread(dst, 1, bytes, f);
	fclose(f);
	return n == bytes;
}

static int read_small_text_file(const char* path, char* dst, size_t dst_size,
								size_t* out_size) {
	unsigned long size = 0;
	if(!file_size_bytes(path, &size))
		return 0;
	if(size + 1u > dst_size)
		return 0;
	FILE* f = fopen(path, "rb");
	if(!f)
		return 0;
	size_t n = fread(dst, 1, (size_t)size, f);
	int ok = n == (size_t)size && ferror(f) == 0;
	fclose(f);
	if(!ok)
		return 0;
	dst[n] = '\0';
	*out_size = n;
	return 1;
}

static int read_u32be_at(const char* path, uint32_t offset, uint32_t* out) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return 0;
	if(fseek(f, (long)offset, SEEK_SET) != 0) {
		fclose(f);
		return 0;
	}
	unsigned char b[4];
	size_t n = fread(b, 1, sizeof(b), f);
	fclose(f);
	if(n != sizeof(b))
		return 0;
	*out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
		   ((uint32_t)b[2] << 8) | (uint32_t)b[3];
	return 1;
}

static int hash_file_hex(const char* path,
						 char hex[CW_SHA256_HEX_LENGTH + 1]) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return 0;
	cw_sha256_ctx ctx;
	cw_sha256_init(&ctx);
	unsigned char buf[8192];
	for(;;) {
		size_t n = fread(buf, 1, sizeof(buf), f);
		if(n)
			cw_sha256_update(&ctx, buf, n);
		if(n < sizeof(buf)) {
			if(ferror(f)) {
				fclose(f);
				return 0;
			}
			break;
		}
	}
	fclose(f);
	cw_sha256_final_hex(&ctx, hex);
	return 1;
}

static int fail(const char* reason) {
	printf("{\"status\":\"FAIL\",\"reason\":\"%s\"}\n", reason);
	return 1;
}

int main(int argc, char** argv) {
	if(argc != 3) {
		fprintf(stderr, "usage: %s <boot.dol> <cw_run_args.txt>\n", argv[0]);
		return fail("usage");
	}
	const char* dol_path = argv[1];
	const char* args_path = argv[2];

	unsigned long dol_size = 0;
	if(!file_size_bytes(dol_path, &dol_size))
		return fail("dol_stat_failed");
	unsigned char header[CW_DOL_HEADER_SIZE];
	if(!read_exact_prefix(dol_path, header, sizeof(header)))
		return fail("dol_header_read_failed");

	char args[CW_RUN_ARGS_MAX_BYTES + 1u];
	size_t args_size = 0;
	if(!read_small_text_file(args_path, args, sizeof(args), &args_size))
		return fail("args_read_failed");

	cw_dol_launch_plan plan;
	char reason[96];
	if(!cw_dol_build_launch_plan(header, dol_size, args, args_size, &plan,
								 reason, sizeof(reason)))
		return fail(reason);

	uint32_t marker = 0;
	if(!read_u32be_at(dol_path, plan.argv_marker_file_offset, &marker))
		return fail("argv_marker_read_failed");
	if(marker != CW_DOL_ARGV_MAGIC)
		return fail("argv_marker_mismatch");

	char dol_sha[CW_SHA256_HEX_LENGTH + 1];
	char args_sha[CW_SHA256_HEX_LENGTH + 1];
	if(!hash_file_hex(dol_path, dol_sha))
		return fail("dol_hash_failed");
	if(!hash_file_hex(args_path, args_sha))
		return fail("args_hash_failed");

	printf("{\"status\":\"PASS\",\"reason\":\"launch_plan\","
		   "\"dol_bytes\":%lu,\"dol_sha256\":\"%s\","
		   "\"args_bytes\":%lu,\"args_sha256\":\"%s\","
		   "\"entry_point\":\"0x%08lx\","
		   "\"min_address\":\"0x%08lx\",\"max_address\":\"0x%08lx\","
		   "\"load_sections\":%lu,\"sections\":%lu,\"argv_count\":%lu,"
		   "\"argv_marker_address\":\"0x%08lx\","
		   "\"argv_marker_file_offset\":%lu,"
		   "\"argv_struct_address\":\"0x%08lx\","
		   "\"argv_struct_bytes\":%lu,"
		   "\"command_line_address\":\"0x%08lx\","
		   "\"command_line_bytes\":%lu}\n",
		   dol_size, dol_sha, (unsigned long)args_size, args_sha,
		   (unsigned long)plan.dol.entry_point,
		   (unsigned long)plan.dol.min_address,
		   (unsigned long)plan.dol.max_address,
		   (unsigned long)plan.load_section_count,
		   (unsigned long)plan.dol.section_count,
		   (unsigned long)plan.argv_argc,
		   (unsigned long)plan.argv_marker_address,
		   (unsigned long)plan.argv_marker_file_offset,
		   (unsigned long)plan.argv_struct_address,
		   (unsigned long)plan.argv_struct_bytes,
		   (unsigned long)plan.command_line_address,
		   (unsigned long)plan.command_line_bytes);
	return 0;
}

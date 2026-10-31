#include "sha256.h"

#include <assert.h>
#include <string.h>

static void check_one_shot(const char* msg, const char* expected) {
	char hex[CW_SHA256_HEX_LENGTH + 1];
	cw_sha256_ctx ctx;
	cw_sha256_init(&ctx);
	cw_sha256_update(&ctx, msg, strlen(msg));
	cw_sha256_final_hex(&ctx, hex);
	assert(strcmp(hex, expected) == 0);
}

static void check_incremental(void) {
	char hex[CW_SHA256_HEX_LENGTH + 1];
	cw_sha256_ctx ctx;
	cw_sha256_init(&ctx);
	cw_sha256_update(&ctx, "a", 1);
	cw_sha256_update(&ctx, "bc", 2);
	cw_sha256_final_hex(&ctx, hex);
	assert(strcmp(hex,
				  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9c"
				  "b410ff61f20015ad") == 0);
}

static void check_many_blocks(void) {
	char block[1000];
	char hex[CW_SHA256_HEX_LENGTH + 1];
	memset(block, 'a', sizeof(block));
	cw_sha256_ctx ctx;
	cw_sha256_init(&ctx);
	for(int i = 0; i < 1000; ++i)
		cw_sha256_update(&ctx, block, sizeof(block));
	cw_sha256_final_hex(&ctx, hex);
	assert(strcmp(hex,
				  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e"
				  "046d39ccc7112cd0") == 0);
}

int main(void) {
	check_one_shot("",
				   "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934c"
				   "a495991b7852b855");
	check_one_shot("abc",
				   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9c"
				   "b410ff61f20015ad");
	check_incremental();
	check_many_blocks();
	return 0;
}

#ifndef CAVEWII_STAGE_SHA256_H
#define CAVEWII_STAGE_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define CW_SHA256_DIGEST_LENGTH 32
#define CW_SHA256_HEX_LENGTH 64

typedef struct cw_sha256_ctx {
	uint32_t state[8];
	uint64_t bit_len;
	uint8_t buffer[64];
	size_t buffer_len;
} cw_sha256_ctx;

void cw_sha256_init(cw_sha256_ctx* ctx);
void cw_sha256_update(cw_sha256_ctx* ctx, const void* data, size_t len);
void cw_sha256_final(cw_sha256_ctx* ctx,
					 uint8_t digest[CW_SHA256_DIGEST_LENGTH]);
void cw_sha256_final_hex(cw_sha256_ctx* ctx,
						 char hex[CW_SHA256_HEX_LENGTH + 1]);

#endif

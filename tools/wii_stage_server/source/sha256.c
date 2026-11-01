#include "sha256.h"

#include <string.h>

static const uint32_t k_sha256[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
	0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
	0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
	0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr32(uint32_t x, unsigned int n) {
	return (x >> n) | (x << (32u - n));
}

static uint32_t load_be32(const uint8_t* p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void transform(cw_sha256_ctx* ctx, const uint8_t block[64]) {
	uint32_t w[64];
	for(size_t i = 0; i < 16; ++i)
		w[i] = load_be32(block + i * 4);
	for(size_t i = 16; i < 64; ++i) {
		uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
					  (w[i - 15] >> 3);
		uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
					  (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32_t a = ctx->state[0];
	uint32_t b = ctx->state[1];
	uint32_t c = ctx->state[2];
	uint32_t d = ctx->state[3];
	uint32_t e = ctx->state[4];
	uint32_t f = ctx->state[5];
	uint32_t g = ctx->state[6];
	uint32_t h = ctx->state[7];

	for(size_t i = 0; i < 64; ++i) {
		uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t temp1 = h + s1 + ch + k_sha256[i] + w[i];
		uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = s0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

void cw_sha256_init(cw_sha256_ctx* ctx) {
	ctx->state[0] = 0x6a09e667u;
	ctx->state[1] = 0xbb67ae85u;
	ctx->state[2] = 0x3c6ef372u;
	ctx->state[3] = 0xa54ff53au;
	ctx->state[4] = 0x510e527fu;
	ctx->state[5] = 0x9b05688cu;
	ctx->state[6] = 0x1f83d9abu;
	ctx->state[7] = 0x5be0cd19u;
	ctx->bit_len = 0;
	ctx->buffer_len = 0;
}

void cw_sha256_update(cw_sha256_ctx* ctx, const void* data, size_t len) {
	const uint8_t* p = (const uint8_t*)data;
	if(!len)
		return;
	ctx->bit_len += (uint64_t)len * 8u;
	while(len > 0) {
		size_t room = sizeof(ctx->buffer) - ctx->buffer_len;
		size_t n = len < room ? len : room;
		memcpy(ctx->buffer + ctx->buffer_len, p, n);
		ctx->buffer_len += n;
		p += n;
		len -= n;
		if(ctx->buffer_len == sizeof(ctx->buffer)) {
			transform(ctx, ctx->buffer);
			ctx->buffer_len = 0;
		}
	}
}

void cw_sha256_final(cw_sha256_ctx* ctx,
					 uint8_t digest[CW_SHA256_DIGEST_LENGTH]) {
	uint64_t bit_len = ctx->bit_len;

	ctx->buffer[ctx->buffer_len++] = 0x80;
	if(ctx->buffer_len > 56) {
		memset(ctx->buffer + ctx->buffer_len, 0,
			   sizeof(ctx->buffer) - ctx->buffer_len);
		transform(ctx, ctx->buffer);
		ctx->buffer_len = 0;
	}
	memset(ctx->buffer + ctx->buffer_len, 0, 56 - ctx->buffer_len);
	for(size_t i = 0; i < 8; ++i)
		ctx->buffer[56 + i] = (uint8_t)(bit_len >> (56 - i * 8));
	transform(ctx, ctx->buffer);

	for(size_t i = 0; i < 8; ++i)
		store_be32(digest + i * 4, ctx->state[i]);
}

void cw_sha256_final_hex(cw_sha256_ctx* ctx,
						 char hex[CW_SHA256_HEX_LENGTH + 1]) {
	static const char digits[] = "0123456789abcdef";
	uint8_t digest[CW_SHA256_DIGEST_LENGTH];
	cw_sha256_final(ctx, digest);
	for(size_t i = 0; i < CW_SHA256_DIGEST_LENGTH; ++i) {
		hex[i * 2] = digits[digest[i] >> 4];
		hex[i * 2 + 1] = digits[digest[i] & 0x0f];
	}
	hex[CW_SHA256_HEX_LENGTH] = '\0';
}

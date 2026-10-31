#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lodepng/lodepng.h"
#include "platform/texture_decode.h"

static void write_huge_ihdr(const char* path) {
	/* Complete PNG signature + IHDR, deliberately no IDAT/IEND. A decoder would
	 * report truncation; the bounded wrapper must reject the 1025-wide IHDR
	 * first, proving dimensions are inspected before image decode/allocation. */
	uint8_t header[33] = {
		137, 80, 78, 71, 13, 10, 26, 10,
		0, 0, 0, 13, 'I', 'H', 'D', 'R',
		0, 0, 4, 1, /* width = 1025 */
		0, 0, 0, 1, /* height = 1 */
		8, 6, 0, 0, 0,
		0, 0, 0, 0,
	};
	lodepng_chunk_generate_crc(&header[8]);
	FILE* file = fopen(path, "wb");
	assert(file);
	assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
	assert(fclose(file) == 0);
}

static void test_valid_decode(void) {
	const char* path = "test_texture_decode_valid.png";
	uint8_t source[4 * 4 * 4];
	for(size_t k = 0; k < sizeof(source); k++)
		source[k] = (uint8_t)(k * 17u);
	assert(lodepng_encode32_file(path, source, 4, 4) == 0);

	size_t width = 99, height = 99;
	enum tex_png_decode_status status = TEX_PNG_DECODE_ERROR;
	uint8_t* decoded = tex_png_decode32_bounded(
		path, 1024, 1024, &width, &height, &status);
	assert(decoded);
	assert(status == TEX_PNG_DECODE_OK);
	assert(width == 4 && height == 4);
	assert(memcmp(decoded, source, sizeof(source)) == 0);
	free(decoded);
	assert(remove(path) == 0);
}

static void test_dimension_preflight(void) {
	const char* path = "test_texture_decode_huge_ihdr.png";
	write_huge_ihdr(path);
	size_t width = 99, height = 99;
	enum tex_png_decode_status status = TEX_PNG_DECODE_OK;
	assert(!tex_png_decode32_bounded(
		path, 1024, 1024, &width, &height, &status));
	assert(status == TEX_PNG_DECODE_INVALID_DIMENSIONS);
	assert(width == 0 && height == 0);
	assert(remove(path) == 0);
}

static void test_encoded_file_preflight(void) {
	const char* path = "test_texture_decode_oversize.png";
	FILE* file = fopen(path, "wb");
	assert(file);
	assert(fseek(file, (long)TEX_PNG_MAX_FILE_BYTES, SEEK_SET) == 0);
	assert(fputc(0, file) == 0);
	assert(fclose(file) == 0);

	size_t width = 99, height = 99;
	enum tex_png_decode_status status = TEX_PNG_DECODE_OK;
	assert(!tex_png_decode32_bounded(
		path, 1024, 1024, &width, &height, &status));
	assert(status == TEX_PNG_DECODE_FILE_TOO_LARGE);
	assert(width == 0 && height == 0);
	assert(remove(path) == 0);
}

int main(void) {
	test_valid_decode();
	test_dimension_preflight();
	test_encoded_file_preflight();
	return 0;
}

/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "texture_decode.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../lodepng/lodepng.h"

static uint8_t* read_png_file_bounded(const char* filename, size_t* size,
									  enum tex_png_decode_status* status) {
	FILE* file = fopen(filename, "rb");
	if(!file) {
		*status = TEX_PNG_DECODE_IO_ERROR;
		return NULL;
	}
	if(fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		*status = TEX_PNG_DECODE_IO_ERROR;
		return NULL;
	}
	long end = ftell(file);
	if(end < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		*status = TEX_PNG_DECODE_IO_ERROR;
		return NULL;
	}
	if((unsigned long)end > TEX_PNG_MAX_FILE_BYTES) {
		fclose(file);
		*status = TEX_PNG_DECODE_FILE_TOO_LARGE;
		return NULL;
	}
	if(end == 0) {
		fclose(file);
		*status = TEX_PNG_DECODE_INVALID_HEADER;
		return NULL;
	}

	*size = (size_t)end;
	uint8_t* bytes = malloc(*size);
	if(!bytes) {
		fclose(file);
		*status = TEX_PNG_DECODE_IO_ERROR;
		return NULL;
	}
	if(fread(bytes, 1, *size, file) != *size || ferror(file)) {
		free(bytes);
		fclose(file);
		*status = TEX_PNG_DECODE_IO_ERROR;
		return NULL;
	}
	if(fclose(file) != 0) {
		free(bytes);
		*status = TEX_PNG_DECODE_IO_ERROR;
		return NULL;
	}
	return bytes;
}

uint8_t* tex_png_decode32_bounded(const char* filename, size_t max_width,
								  size_t max_height, size_t* width,
								  size_t* height,
								  enum tex_png_decode_status* status) {
	assert(filename && max_width && max_height && width && height && status);
	if(!filename || !max_width || !max_height || !width || !height || !status)
		return NULL;
	*width = 0;
	*height = 0;
	*status = TEX_PNG_DECODE_IO_ERROR;

	size_t encoded_size = 0;
	uint8_t* encoded = read_png_file_bounded(filename, &encoded_size, status);
	if(!encoded)
		return NULL;

	LodePNGState state;
	lodepng_state_init(&state);
	unsigned png_width = 0, png_height = 0;
	unsigned error = lodepng_inspect(
		&png_width, &png_height, &state, encoded, encoded_size);
	if(error) {
		*status = TEX_PNG_DECODE_INVALID_HEADER;
		goto fail;
	}
	if(png_width == 0 || png_height == 0 || png_width > max_width
	   || png_height > max_height) {
		*status = TEX_PNG_DECODE_INVALID_DIMENSIONS;
		goto fail;
	}

	/* Texture sampling never consumes PNG text or color profiles. Avoid their
	 * independent decompression/allocation paths and cap the actual image stream
	 * to the maximum 16-bit RGBA scanline payload plus filter/interlace slack. */
#ifdef LODEPNG_COMPILE_ANCILLARY_CHUNKS
	state.decoder.read_text_chunks = 0;
	state.decoder.max_icc_size = 1024u * 1024u;
#endif
	size_t width_size = png_width;
	size_t height_size = png_height;
	if(width_size > SIZE_MAX / height_size) {
		*status = TEX_PNG_DECODE_INVALID_DIMENSIONS;
		goto fail;
	}
	size_t pixels = width_size * height_size;
	if(pixels > (SIZE_MAX - 64u) / 8u
	   || width_size > SIZE_MAX - height_size) {
		*status = TEX_PNG_DECODE_INVALID_DIMENSIONS;
		goto fail;
	}
	size_t span = width_size + height_size;
	size_t image_bytes = pixels * 8u;
	if(span > (SIZE_MAX - image_bytes - 64u) / 8u) {
		*status = TEX_PNG_DECODE_INVALID_DIMENSIONS;
		goto fail;
	}
	state.decoder.zlibsettings.max_output_size
		= image_bytes + span * 8u + 64u;
	state.info_raw.colortype = LCT_RGBA;
	state.info_raw.bitdepth = 8;

	uint8_t* decoded = NULL;
	unsigned decoded_width = 0, decoded_height = 0;
	error = lodepng_decode(
		&decoded, &decoded_width, &decoded_height, &state, encoded, encoded_size);
	if(error || decoded_width != png_width || decoded_height != png_height) {
		free(decoded);
		*status = TEX_PNG_DECODE_ERROR;
		goto fail;
	}

	*width = decoded_width;
	*height = decoded_height;
	*status = TEX_PNG_DECODE_OK;
	lodepng_state_cleanup(&state);
	free(encoded);
	return decoded;

fail:
	lodepng_state_cleanup(&state);
	free(encoded);
	return NULL;
}

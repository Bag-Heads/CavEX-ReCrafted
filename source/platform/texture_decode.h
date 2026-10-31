/*
	Copyright (c) 2026 CaveWii_v2 contributors

	Bounded PNG decoding for texture-pack inputs. Texture packs are user-provided
	files, so dimensions and encoded size must be checked before LodePNG allocates
	the decoded image or its IDAT scratch buffer.
*/

#ifndef TEXTURE_DECODE_H
#define TEXTURE_DECODE_H

#include <stddef.h>
#include <stdint.h>

/* A 1024x1024 PNG can contain 8 MiB of uncompressed 16-bit RGBA samples.
 * Ten MiB leaves deflate/chunk overhead without allowing an arbitrarily large
 * input buffer to be duplicated by LodePNG on the Wii. This is an explicit
 * texture-pack format limit, not a silent allocation heuristic. */
#define TEX_PNG_MAX_FILE_BYTES (10u * 1024u * 1024u)

enum tex_png_decode_status {
	TEX_PNG_DECODE_OK,
	TEX_PNG_DECODE_IO_ERROR,
	TEX_PNG_DECODE_FILE_TOO_LARGE,
	TEX_PNG_DECODE_INVALID_HEADER,
	TEX_PNG_DECODE_INVALID_DIMENSIONS,
	TEX_PNG_DECODE_ERROR,
};

/* Reads a regular texture-pack PNG once, checks its encoded size and IHDR
 * dimensions before decoding, and returns RGBA8 pixels owned by the caller.
 * On failure, returns NULL and always zeros width/height. */
uint8_t* tex_png_decode32_bounded(const char* filename, size_t max_width,
								  size_t max_height, size_t* width,
								  size_t* height,
								  enum tex_png_decode_status* status);

#endif

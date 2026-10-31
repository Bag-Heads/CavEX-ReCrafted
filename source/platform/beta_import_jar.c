/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* In-game Beta importer, part A2: a streaming zip reader on the game's zlib
 * and the texture half of tools/pack_helper core._prepare for a b1.7.3
 * client jar.
 *
 * What the helper writes for a real (1x) b1.7.3.jar, and what this writes:
 *   - each of the 55 client-jar textures at its canonical size: the helper's
 *     convert_image passes those bytes through untouched after a decode
 *     check, so these are copied byte for byte (gui/container.png lands as
 *     gui/chest.png, helper source_names);
 *   - gui_2.png  = compose_gui2(bundled gui_2.png, gui/gui.png, gui/icons.png)
 *   - anim.png   = compose_anim(beta-base anim.png, terrain.png)
 *     both via tex_beta_compose.c (pixel-identical to the helper, proven by
 *     test_tex_beta_compose), encoded with lodepng: decoded RGBA equals the
 *     helper's; the PNG bytes differ only by encoder (Pillow vs lodepng);
 *   - pointer.png = the bundled Beta pointer (assets/pointer.png is a byte
 *     copy of packs/beta-base/pointer.png).
 *
 * Deliberate scope limit: a texture that is not its canonical size (an HD or
 * modded jar) is refused with a message pointing at the PC Pack Helper,
 * which does the 2x/4x/8x reductions. An unmodified b1.7.3.jar is all 1x.
 *
 * Memory: the central directory (bounded, ~70 KB for the real jar), one
 * 16 KB input + 16 KB output buffer and zlib's ~44 KB inflate state while an
 * entry streams; then one PNG decode (<= 256 KiB) at a time to validate. */

#include "beta_import_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "../lodepng/lodepng.h"
#include "tex_beta_compose.h"
#include "texture_decode.h"

/* ---- the 55 client-jar textures (conversion_map.json, map 1.4.0) -------- */

const struct bi_texture bi_textures[BI_TEXTURE_COUNT] = {
	{"armor/chain_1.png", {"armor/chain_1.png", NULL}, 64, 32},
	{"armor/chain_2.png", {"armor/chain_2.png", NULL}, 64, 32},
	{"armor/cloth_1.png", {"armor/cloth_1.png", NULL}, 64, 32},
	{"armor/cloth_2.png", {"armor/cloth_2.png", NULL}, 64, 32},
	{"armor/diamond_1.png", {"armor/diamond_1.png", NULL}, 64, 32},
	{"armor/diamond_2.png", {"armor/diamond_2.png", NULL}, 64, 32},
	{"armor/gold_1.png", {"armor/gold_1.png", NULL}, 64, 32},
	{"armor/gold_2.png", {"armor/gold_2.png", NULL}, 64, 32},
	{"armor/iron_1.png", {"armor/iron_1.png", NULL}, 64, 32},
	{"armor/iron_2.png", {"armor/iron_2.png", NULL}, 64, 32},
	{"art/kz.png", {"art/kz.png", NULL}, 256, 256},
	{"environment/clouds.png", {"environment/clouds.png", NULL}, 256, 256},
	{"environment/rain.png", {"environment/rain.png", NULL}, 64, 256},
	{"environment/snow.png", {"environment/snow.png", NULL}, 64, 256},
	{"font/default.png", {"font/default.png", NULL}, 128, 128},
	{"gui/chest.png", {"gui/container.png", "gui/chest.png"}, 256, 256},
	{"gui/crafting.png", {"gui/crafting.png", NULL}, 256, 256},
	{"gui/furnace.png", {"gui/furnace.png", NULL}, 256, 256},
	{"gui/gui.png", {"gui/gui.png", NULL}, 256, 256},
	{"gui/icons.png", {"gui/icons.png", NULL}, 256, 256},
	{"gui/inventory.png", {"gui/inventory.png", NULL}, 256, 256},
	{"gui/items.png", {"gui/items.png", NULL}, 256, 256},
	{"gui/trap.png", {"gui/trap.png", NULL}, 256, 256},
	{"item/arrows.png", {"item/arrows.png", NULL}, 32, 32},
	{"item/cart.png", {"item/cart.png", NULL}, 64, 32},
	{"misc/dial.png", {"misc/dial.png", NULL}, 16, 16},
	{"misc/foliagecolor.png", {"misc/foliagecolor.png", NULL}, 256, 256},
	{"misc/grasscolor.png", {"misc/grasscolor.png", NULL}, 256, 256},
	{"misc/mapbg.png", {"misc/mapbg.png", NULL}, 64, 64},
	{"misc/mapicons.png", {"misc/mapicons.png", NULL}, 32, 32},
	{"misc/shadow.png", {"misc/shadow.png", NULL}, 64, 64},
	{"misc/water.png", {"misc/water.png", NULL}, 16, 16},
	{"mob/char.png", {"mob/char.png", NULL}, 64, 32},
	{"mob/chicken.png", {"mob/chicken.png", NULL}, 64, 32},
	{"mob/cow.png", {"mob/cow.png", NULL}, 64, 32},
	{"mob/creeper.png", {"mob/creeper.png", NULL}, 64, 32},
	{"mob/ghast.png", {"mob/ghast.png", NULL}, 64, 32},
	{"mob/ghast_fire.png", {"mob/ghast_fire.png", NULL}, 64, 32},
	{"mob/pig.png", {"mob/pig.png", NULL}, 64, 32},
	{"mob/pigzombie.png", {"mob/pigzombie.png", NULL}, 64, 32},
	{"mob/sheep.png", {"mob/sheep.png", NULL}, 64, 32},
	{"mob/sheep_fur.png", {"mob/sheep_fur.png", NULL}, 64, 32},
	{"mob/skeleton.png", {"mob/skeleton.png", NULL}, 64, 32},
	{"mob/slime.png", {"mob/slime.png", NULL}, 64, 32},
	{"mob/spider.png", {"mob/spider.png", NULL}, 64, 32},
	{"mob/spider_eyes.png", {"mob/spider_eyes.png", NULL}, 64, 32},
	{"mob/squid.png", {"mob/squid.png", NULL}, 64, 32},
	{"mob/wolf.png", {"mob/wolf.png", NULL}, 64, 32},
	{"mob/wolf_angry.png", {"mob/wolf_angry.png", NULL}, 64, 32},
	{"mob/wolf_tame.png", {"mob/wolf_tame.png", NULL}, 64, 32},
	{"mob/zombie.png", {"mob/zombie.png", NULL}, 64, 32},
	{"particles.png", {"particles.png", NULL}, 128, 128},
	{"terrain.png", {"terrain.png", NULL}, 256, 256},
	{"terrain/moon.png", {"terrain/moon.png", NULL}, 32, 32},
	{"terrain/sun.png", {"terrain/sun.png", NULL}, 32, 32},
};

/* ---- zip reader ---------------------------------------------------------- */

#define ZIP_EOCD_SIG 0x06054b50u
#define ZIP_CDH_SIG 0x02014b50u
#define ZIP_LFH_SIG 0x04034b50u
#define ZIP_MAX_CD (4u << 20)	   /* central directory bytes */
#define ZIP_MAX_ENTRY (32u << 20)  /* helper limits.file_bytes */
#define ZIP_MAX_RATIO 200u		   /* helper limits.ratio */
#define ZIP_CHUNK 16384u

struct bi_zip {
	FILE* f;
	long file_size;
	uint8_t* cd;	  /* central directory bytes */
	uint32_t* offs;	  /* entry i starts at cd + offs[i] */
	size_t count;
};

static uint16_t rd16(const uint8_t* p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
		| ((uint32_t)p[3] << 24);
}

const char* bi_zip_err_str(enum bi_zip_err e) {
	switch(e) {
		case BI_ZIP_OK: return "ok";
		case BI_ZIP_IO: return "the file could not be read";
		case BI_ZIP_NOT_ZIP: return "it is not a jar/zip file";
		case BI_ZIP_ZIP64: return "it uses a zip format Beta jars never use";
		case BI_ZIP_BAD_CD: return "its file list is damaged";
		case BI_ZIP_TOO_BIG: return "an entry is too large";
		case BI_ZIP_ENCRYPTED: return "it is encrypted";
		case BI_ZIP_METHOD: return "it uses an unsupported compression";
		case BI_ZIP_BAD_LOCAL: return "an entry header is damaged";
		case BI_ZIP_CORRUPT: return "it is damaged (checksum mismatch)";
		case BI_ZIP_SINK: return "the SD card write failed";
		case BI_ZIP_CANCELLED: return "cancelled";
	}
	return "unknown error";
}

void bi_zip_close(bi_zip* z) {
	if(!z)
		return;
	if(z->f)
		fclose(z->f);
	free(z->cd);
	free(z->offs);
	free(z);
}

enum bi_zip_err bi_zip_open(const char* path, bi_zip** out) {
	*out = NULL;
	bi_zip* z = calloc(1, sizeof(*z));
	if(!z)
		return BI_ZIP_IO;
	enum bi_zip_err err = BI_ZIP_IO;
	z->f = fopen(path, "rb");
	if(!z->f || fseek(z->f, 0, SEEK_END) != 0)
		goto fail;
	z->file_size = ftell(z->f);
	if(z->file_size < 22) {
		err = BI_ZIP_NOT_ZIP;
		goto fail;
	}
	/* EOCD: last 22 bytes + up to 64 KB comment. */
	long tail = z->file_size < 22 + 65535 ? z->file_size : 22 + 65535;
	uint8_t* buf = malloc((size_t)tail);
	if(!buf)
		goto fail;
	if(fseek(z->f, z->file_size - tail, SEEK_SET) != 0
	   || fread(buf, 1, (size_t)tail, z->f) != (size_t)tail) {
		free(buf);
		goto fail;
	}
	long eocd = -1;
	for(long i = tail - 22; i >= 0; i--) {
		if(rd32(buf + i) == ZIP_EOCD_SIG
		   && i + 22 + rd16(buf + i + 20) == tail) {
			eocd = i;
			break;
		}
	}
	if(eocd < 0) {
		free(buf);
		err = BI_ZIP_NOT_ZIP;
		goto fail;
	}
	const uint8_t* e = buf + eocd;
	uint16_t disk = rd16(e + 4), cd_disk = rd16(e + 6);
	uint16_t n_disk = rd16(e + 8), n_total = rd16(e + 10);
	uint32_t cd_size = rd32(e + 12), cd_off = rd32(e + 16);
	free(buf);
	if(disk || cd_disk || n_disk != n_total || n_total == 0xFFFF
	   || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
		err = BI_ZIP_ZIP64;
		goto fail;
	}
	if(cd_size > ZIP_MAX_CD) {
		err = BI_ZIP_TOO_BIG;
		goto fail;
	}
	if((long)cd_off + (long)cd_size > z->file_size - 22) {
		err = BI_ZIP_BAD_CD;
		goto fail;
	}
	z->cd = malloc(cd_size ? cd_size : 1);
	z->offs = malloc(sizeof(uint32_t) * (n_total ? n_total : 1));
	if(!z->cd || !z->offs)
		goto fail;
	if(fseek(z->f, (long)cd_off, SEEK_SET) != 0
	   || fread(z->cd, 1, cd_size, z->f) != cd_size)
		goto fail;
	uint32_t pos = 0;
	for(size_t i = 0; i < n_total; i++) {
		if(pos + 46 > cd_size || rd32(z->cd + pos) != ZIP_CDH_SIG) {
			err = BI_ZIP_BAD_CD;
			goto fail;
		}
		uint32_t len = 46u + rd16(z->cd + pos + 28) + rd16(z->cd + pos + 30)
			+ rd16(z->cd + pos + 32);
		if(pos + len > cd_size) {
			err = BI_ZIP_BAD_CD;
			goto fail;
		}
		z->offs[i] = pos;
		pos += len;
	}
	z->count = n_total;
	*out = z;
	return BI_ZIP_OK;
fail:
	bi_zip_close(z);
	return err;
}

size_t bi_zip_count(const bi_zip* z) {
	return z ? z->count : 0;
}

bool bi_zip_entry_at(const bi_zip* z, size_t i, bi_zip_entry* out) {
	if(!z || i >= z->count)
		return false;
	const uint8_t* h = z->cd + z->offs[i];
	uint16_t name_len = rd16(h + 28);
	memset(out, 0, sizeof(*out));
	out->flags = rd16(h + 8);
	out->method = rd16(h + 10);
	out->crc = rd32(h + 16);
	out->csize = rd32(h + 20);
	out->usize = rd32(h + 24);
	out->local_offset = rd32(h + 42);
	size_t n = name_len < sizeof(out->name) - 1 ? name_len : sizeof(out->name) - 1;
	memcpy(out->name, h + 46, n);
	out->name[n] = '\0';
	/* A name that does not fit or embeds NUL can never match a texture. */
	if(name_len >= sizeof(out->name) || strlen(out->name) != name_len)
		out->name[0] = '\0';
	return true;
}

enum bi_zip_err bi_zip_extract(bi_zip* z, const bi_zip_entry* e,
							   bi_zip_sink sink, void* user,
							   volatile bool* cancel) {
	if(e->flags & 1u)
		return BI_ZIP_ENCRYPTED;
	if(e->method != 0 && e->method != 8)
		return BI_ZIP_METHOD;
	if(e->usize > ZIP_MAX_ENTRY)
		return BI_ZIP_TOO_BIG;
	if(e->csize == 0 ? e->usize != 0
					 : e->usize / ZIP_MAX_RATIO > e->csize)
		return BI_ZIP_TOO_BIG; /* helper "compression_bomb" */
	if(e->method == 0 && e->csize != e->usize)
		return BI_ZIP_CORRUPT;

	uint8_t lfh[30];
	if(fseek(z->f, (long)e->local_offset, SEEK_SET) != 0
	   || fread(lfh, 1, 30, z->f) != 30)
		return BI_ZIP_IO;
	if(rd32(lfh) != ZIP_LFH_SIG || rd16(lfh + 8) != e->method)
		return BI_ZIP_BAD_LOCAL;
	long data = (long)e->local_offset + 30 + rd16(lfh + 26) + rd16(lfh + 28);
	if(data + (long)e->csize > z->file_size)
		return BI_ZIP_BAD_LOCAL;
	if(fseek(z->f, data, SEEK_SET) != 0)
		return BI_ZIP_IO;

	enum bi_zip_err err = BI_ZIP_OK;
	uint8_t* in = malloc(ZIP_CHUNK);
	uint8_t* outb = malloc(ZIP_CHUNK);
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	bool inflating = false;
	if(!in || !outb) {
		err = BI_ZIP_IO;
		goto done;
	}
	if(e->method == 8) {
		if(inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
			err = BI_ZIP_IO;
			goto done;
		}
		inflating = true;
	}
	uint32_t remaining = e->csize, produced = 0;
	uLong crc = crc32(0L, Z_NULL, 0);
	bool stream_end = e->method == 0;
	while(remaining > 0 || (e->method == 8 && !stream_end)) {
		if(cancel && *cancel) {
			err = BI_ZIP_CANCELLED;
			goto done;
		}
		uint32_t want = remaining < ZIP_CHUNK ? remaining : ZIP_CHUNK;
		if(want && fread(in, 1, want, z->f) != want) {
			err = BI_ZIP_IO;
			goto done;
		}
		remaining -= want;
		if(e->method == 0) {
			produced += want;
			crc = crc32(crc, in, want);
			if(!sink(user, in, want)) {
				err = BI_ZIP_SINK;
				goto done;
			}
			continue;
		}
		zs.next_in = in;
		zs.avail_in = want;
		do {
			zs.next_out = outb;
			zs.avail_out = ZIP_CHUNK;
			int r = inflate(&zs, Z_NO_FLUSH);
			if(r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) {
				err = BI_ZIP_CORRUPT;
				goto done;
			}
			size_t got = ZIP_CHUNK - zs.avail_out;
			if(got) {
				if(produced + got > e->usize) {
					err = BI_ZIP_CORRUPT;
					goto done;
				}
				produced += (uint32_t)got;
				crc = crc32(crc, outb, (uInt)got);
				if(!sink(user, outb, got)) {
					err = BI_ZIP_SINK;
					goto done;
				}
			}
			if(r == Z_STREAM_END) {
				stream_end = true;
				break;
			}
			if(r == Z_BUF_ERROR && got == 0)
				break; /* needs more input */
		} while(zs.avail_in > 0 || zs.avail_out == 0);
		if(!stream_end && remaining == 0) {
			err = BI_ZIP_CORRUPT; /* compressed data ran out early */
			goto done;
		}
	}
	if(produced != e->usize || (uint32_t)crc != e->crc)
		err = BI_ZIP_CORRUPT;
done:
	if(inflating)
		inflateEnd(&zs);
	free(in);
	free(outb);
	return err;
}

/* ---- texture extraction --------------------------------------------------- */

static void ctx_fail(bi_ctx* ctx, const char* fmt, const char* a,
					 const char* b) {
	char msg[400]; /* bi_progress_set keeps the first 127 chars */
	snprintf(msg, sizeof(msg), fmt, a ? a : "", b ? b : "");
	bi_progress_set(ctx, BI_FAILED, ctx->p.done, ctx->p.total, msg);
}

static bool name_ieq(const char* a, const char* b) {
	for(; *a && *b; a++, b++)
		if(tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	return *a == *b;
}

static bool name_has_prefix_i(const char* s, const char* prefix) {
	for(; *prefix; s++, prefix++)
		if(!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
			return false;
	return true;
}

static bool name_ends_i(const char* s, const char* suffix) {
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && name_ieq(s + n - m, suffix);
}

struct file_sink {
	FILE* f;
};

static bool file_sink_write(void* user, const uint8_t* data, size_t n) {
	struct file_sink* s = user;
	return fwrite(data, 1, n, s->f) == n;
}

/* Create dir/rel's parent folders, then open it for writing. */
static FILE* open_out(const char* dir, const char* rel, char* path,
					  size_t path_len) {
	int n = snprintf(path, path_len, "%s/%s", dir, rel);
	if(n < 0 || (size_t)n >= path_len)
		return NULL;
	char parent[BI_PATH_MAX];
	snprintf(parent, sizeof(parent), "%s", path);
	char* slash = strrchr(parent, '/');
	if(slash) {
		*slash = '\0';
		if(!bi_mkdirs(parent))
			return NULL;
	}
	return fopen(path, "wb");
}

/* conversion._png_dimensions + _decode for a canonical-size pass-through:
 * PNG signature, IHDR first, exact canonical size, no APNG animation
 * (Pillow n_frames != 1 is refused by the helper), and a full decode. */
static bool png_validate(const char* path, unsigned w, unsigned h,
						 char* why, size_t why_len) {
	FILE* f = fopen(path, "rb");
	if(!f) {
		snprintf(why, why_len, "cannot be read back");
		return false;
	}
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	uint8_t hdr[24];
	bool ok = fread(hdr, 1, 24, f) == 24 && memcmp(hdr, sig, 8) == 0
		&& rd32(hdr + 8) == 0x0d000000u && memcmp(hdr + 12, "IHDR", 4) == 0;
	if(!ok) {
		fclose(f);
		snprintf(why, why_len, "is not a real PNG image");
		return false;
	}
	unsigned pw = ((unsigned)hdr[16] << 24) | ((unsigned)hdr[17] << 16)
		| ((unsigned)hdr[18] << 8) | hdr[19];
	unsigned ph = ((unsigned)hdr[20] << 24) | ((unsigned)hdr[21] << 16)
		| ((unsigned)hdr[22] << 8) | hdr[23];
	if(pw != w || ph != h) {
		fclose(f);
		snprintf(why, why_len, "is %ux%u, not %ux%u", pw, ph, w, h);
		return false;
	}
	/* Walk chunk headers: refuse acTL (animated PNG). */
	long pos = 8;
	for(;;) {
		uint8_t ch[8];
		if(fseek(f, pos, SEEK_SET) != 0 || fread(ch, 1, 8, f) != 8)
			break;
		uint32_t len = ((uint32_t)ch[0] << 24) | ((uint32_t)ch[1] << 16)
			| ((uint32_t)ch[2] << 8) | ch[3];
		if(memcmp(ch + 4, "acTL", 4) == 0) {
			fclose(f);
			snprintf(why, why_len, "is an animated PNG");
			return false;
		}
		if(memcmp(ch + 4, "IEND", 4) == 0 || len > (1u << 30))
			break;
		pos += 12 + (long)len;
	}
	fclose(f);
	size_t dw = 0, dh = 0;
	enum tex_png_decode_status st;
	uint8_t* px = tex_png_decode32_bounded(path, w, h, &dw, &dh, &st);
	if(!px) {
		snprintf(why, why_len, "is damaged (PNG decode failed)");
		return false;
	}
	free(px);
	return true;
}

static bool write_png_rgba(const char* dir, const char* rel,
						   const uint8_t* rgba) {
	unsigned char* png = NULL;
	size_t len = 0;
	if(lodepng_encode32(&png, &len, rgba, TEX_BETA_CANON, TEX_BETA_CANON) != 0
	   || !png) {
		free(png);
		return false;
	}
	char path[BI_PATH_MAX];
	FILE* f = open_out(dir, rel, path, sizeof(path));
	bool ok = f && fwrite(png, 1, len, f) == len;
	if(f && fclose(f) != 0)
		ok = false;
	free(png);
	return ok;
}

static bool copy_file(const char* src, const char* dir, const char* rel) {
	FILE* in = fopen(src, "rb");
	if(!in)
		return false;
	char path[BI_PATH_MAX];
	FILE* out = open_out(dir, rel, path, sizeof(path));
	bool ok = out != NULL;
	uint8_t buf[4096];
	size_t n;
	while(ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
		ok = fwrite(buf, 1, n, out) == n;
	if(ferror(in))
		ok = false;
	fclose(in);
	if(out && fclose(out) != 0)
		ok = false;
	return ok;
}

bool bi_jar_extract_textures(const char* jar, const char* staging_dir,
							 bi_ctx* ctx) {
	bi_zip* z = NULL;
	enum bi_zip_err err = bi_zip_open(jar, &z);
	if(err != BI_ZIP_OK) {
		ctx_fail(ctx, "Can't read the jar: %s.", bi_zip_err_str(err), NULL);
		return false;
	}
	const size_t total = BI_TEXTURE_COUNT + 3; /* + gui_2, anim, pointer */
	bi_progress_set(ctx, BI_TEXTURES, 0, (int)total, "Reading textures");

	/* Pass 1: classify. Modern jar refusal comes first (helper order), then
	 * "is this a client jar", then which entry supplies each texture -- last
	 * one in archive order wins, like the helper's sequential writes. */
	long pick[BI_TEXTURE_COUNT];
	for(size_t t = 0; t < BI_TEXTURE_COUNT; t++)
		pick[t] = -1;
	bool has_class = false;
	static const char* const modern[] = {"assets/minecraft/", "textures/block/",
										 "textures/item/", "textures/blocks/",
										 "textures/items/"};
	bi_zip_entry e;
	for(size_t i = 0; i < bi_zip_count(z); i++) {
		bi_zip_entry_at(z, i, &e);
		for(size_t m = 0; m < sizeof(modern) / sizeof(*modern); m++) {
			if(name_has_prefix_i(e.name, modern[m])) {
				bi_zip_close(z);
				ctx_fail(ctx, "This jar isn't Beta 1.7.3 (it's a newer "
						 "version). Use b1.7.3.jar.", NULL, NULL);
				return false;
			}
		}
		if(name_ends_i(e.name, ".class"))
			has_class = true;
		for(size_t t = 0; t < BI_TEXTURE_COUNT; t++)
			for(int s = 0; s < 2; s++)
				if(bi_textures[t].source[s]
				   && name_ieq(e.name, bi_textures[t].source[s]))
					pick[t] = (long)i;
	}
	if(!has_class) {
		bi_zip_close(z);
		ctx_fail(ctx, "This is not the Beta 1.7.3 jar. Copy b1.7.3.jar to "
				 "sd:/beta/.", NULL, NULL);
		return false;
	}
	size_t missing = 0;
	const char* first_missing = NULL;
	for(size_t t = 0; t < BI_TEXTURE_COUNT; t++) {
		if(pick[t] < 0) {
			if(!first_missing)
				first_missing = bi_textures[t].dest;
			missing++;
		}
	}
	if(missing) {
		char n[24];
		snprintf(n, sizeof(n), "%zu", missing);
		bi_zip_close(z);
		ctx_fail(ctx, "Jar is missing %s textures (%s). Damaged or modded.",
				 n, first_missing);
		return false;
	}

	/* Pass 2: stream each texture to staging and validate it. */
	for(size_t t = 0; t < BI_TEXTURE_COUNT; t++) {
		if(bi_cancel_requested(ctx)) {
			bi_zip_close(z);
			return false;
		}
		bi_zip_entry_at(z, (size_t)pick[t], &e);
		char path[BI_PATH_MAX];
		FILE* f = open_out(staging_dir, bi_textures[t].dest, path, sizeof(path));
		if(!f) {
			bi_zip_close(z);
			ctx_fail(ctx, "Can't write %s to the SD card.",
					 bi_textures[t].dest, NULL);
			return false;
		}
		setvbuf(f, NULL, _IOFBF, 16384);
		struct file_sink sink = {f};
		err = bi_zip_extract(z, &e, file_sink_write, &sink, &ctx->cancel);
		if(fclose(f) != 0 && err == BI_ZIP_OK)
			err = BI_ZIP_SINK;
		if(err == BI_ZIP_CANCELLED) {
			bi_zip_close(z);
			return false;
		}
		if(err != BI_ZIP_OK) {
			bi_zip_close(z);
			ctx_fail(ctx, "%s: %s.", e.name, bi_zip_err_str(err));
			return false;
		}
		char why[64];
		if(!png_validate(path, bi_textures[t].w, bi_textures[t].h, why,
						 sizeof(why))) {
			bi_zip_close(z);
			if(strstr(why, " not "))
				ctx_fail(ctx, "%s %s. Use the PC Pack Helper for HD packs.",
						 e.name, why);
			else
				ctx_fail(ctx, "%s %s.", e.name, why);
			return false;
		}
		bi_progress_step(ctx, (int)t + 1);
	}
	bi_zip_close(z);

	/* Compositions, exactly the helper's (tex_beta_compose.c). */
	const char* assets = ctx->assets_dir;
	uint8_t* gui2 = tex_beta_compose_gui2(staging_dir, assets, NULL);
	bool ok = gui2 && write_png_rgba(staging_dir, "gui_2.png", gui2);
	free(gui2);
	if(!ok) {
		ctx_fail(ctx, "Can't build the HUD sheet (gui_2.png). Reinstall "
				 "CavEX if this repeats.", NULL, NULL);
		return false;
	}
	bi_progress_step(ctx, BI_TEXTURE_COUNT + 1);
	if(bi_cancel_requested(ctx))
		return false;

	char anim_base[BI_PATH_MAX];
	snprintf(anim_base, sizeof(anim_base), "%s/%s", assets, TEX_BETA_BASE_ANIM);
	uint8_t* anim = tex_beta_compose_anim(staging_dir, anim_base, NULL);
	ok = anim && write_png_rgba(staging_dir, "anim.png", anim);
	free(anim);
	if(!ok) {
		ctx_fail(ctx, "Can't build the animation sheet (anim.png). Reinstall "
				 "CavEX if this repeats.", NULL, NULL);
		return false;
	}
	bi_progress_step(ctx, BI_TEXTURE_COUNT + 2);

	char pointer[BI_PATH_MAX];
	snprintf(pointer, sizeof(pointer), "%s/pointer.png", assets);
	if(!copy_file(pointer, staging_dir, "pointer.png")) {
		ctx_fail(ctx, "Can't copy the pointer. Reinstall CavEX if this "
				 "repeats.", NULL, NULL);
		return false;
	}
	bi_progress_step(ctx, (int)total);
	return true;
}

/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
	In-game "Import Beta 1.7.3", sound phase (coder B; contract
	tmp/jobs/ingame-import-1030/CONTRACT.md). bi_sounds_import() turns the
	player's own .minecraft "resources" folder (sd:/beta/resources/) into the
	one-shot pool files of the pack, byte-identical to what the PC Pack
	Helper writes (tools/pack_helper/owned_importer.py process_sounds with
	strict_transcode=True, each file through tools/sound_transcode).

	The helper's rules, reproduced here one for one:
	  - keys: the engine's AUDIO_SOUND_TABLE X-macro (audio_sound_table.h),
	    every row, in table order. "step.grass" -> dir "step", base "grass".
	  - sources: <resources>/sound/<dir>/ then <resources>/newsound/<dir>/
	    (Beta's one SoundPool: a plain union, no precedence), each directory
	    listed in sorted order, files matching ^<base>\d*\.(wav|ogg)$ (case-
	    insensitive).
	  - dedup: a candidate whose BYTES equal an earlier candidate's is dropped
	    (sound == newsound overlap). The helper hashes with sha256; here it is
	    size + FNV-1a 64 and, on a hash match, a full byte compare, which is
	    the same relation.
	  - names: the n-th surviving candidate (1-based) is "<base><n>.wav" for
	    n <= AUDIO_MAX_VARIANTS-1, "<base>.wav" for n == AUDIO_MAX_VARIANTS,
	    and dropped beyond that. A candidate that fails to decode keeps its n
	    and is skipped (strict mode: never raw-copied), so the gap stays.
	  - bytes: decode -> PCM16 mono (stereo averaged, (l+r)/2 truncating)
	    -> the 32 kHz rate cap (sound_transcode_rate_cap, Lanczos-3) ->
	    the canonical 44-byte-header WAV (audio_wav.h wavh_write layout).

	Music and records (helper process_music / process_records) ride along:
	music/ + newmusic/*.ogg -> music/<name> verbatim, first name wins;
	streaming/{13,cat}.ogg -> records/<stem>.ogg verbatim, first wins. The
	return value counts every file written (sounds + music + records).

	Reuse instead of a copy: tools/sound_transcode.c is textually included
	below (no main, header-only stb_vorbis), so the cap and the key table are
	THE tool's code, not a mirror. stb_vorbis itself is the engine's one
	implementation TU (audio_registry.c); this file only declares it.

	Wii budget (the maintainer's "one-time ogg decode at import only" rule):
	  - the encoded file is never loaded whole: stb_vorbis pull-decodes from
	    the FILE in 4096-frame blocks and the blocks are folded to mono as
	    they come, so the peak is one decoded MONO PCM buffer (frames*2 bytes;
	    ~5 MB for Beta's longest 192 kHz cave sound) plus the capped copy
	    (~1/6 of that) plus stb_vorbis's own setup memory. The pull path
	    yields the same samples as stb_vorbis_decode_memory (which is that
	    loop internally; test_audio_deferred pins the equivalence).
	  - WAV sources are probed with wavh_probe_file and only the sample
	    words are read.
	  - SD writes go out in 64 KiB chunks with a cancel check between chunks.
	  - Allocation failure is a reported failure for the import (ctx msg
	    names the file); it is never a silent skip.
	Nothing here runs during play: the orchestrator's worker thread calls it
	from the Texture Packs screen only.
*/

#include "beta_import.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pack_cache.h" /* pack_cache_ensure_dir, pack_cache_fnv */

/* audio_wav.h is first pulled in by sound_transcode.c below; ask it for
   the FILE-walking probe too (WAV sources are not loaded whole). */
#define WAVH_WITH_FILE_PROBE
#include "audio_wav.h"

/* ---- tools/sound_transcode.c, textually, as THE transcode core ---------
   NO_MAIN: no main(). EXTERN_VORBIS: stb_vorbis declarations only, the
   implementation is audio_registry.c's. The tool's four public symbols are
   renamed so this TU can sit in the same link as the tool's own TU
   (test_audio_env compiles tools/sound_transcode.c next to the engine
   pool): same code, bi_-prefixed names. */
#define SOUND_TRANSCODE_NO_MAIN
#define SOUND_TRANSCODE_EXTERN_VORBIS
#define sound_transcode_buffer bi_st_sound_transcode_buffer
#define sound_transcode_wav_emit bi_st_sound_transcode_wav_emit
#define sound_transcode_rate_cap bi_st_sound_transcode_rate_cap
#define sound_transcode_manifest_line bi_st_sound_transcode_manifest_line
#include "../../tools/sound_transcode.c"
#undef sound_transcode_buffer
#undef sound_transcode_wav_emit
#undef sound_transcode_rate_cap
#undef sound_transcode_manifest_line

#define BI_PATH_MAX 512
#define BI_WRITE_CHUNK (64u * 1024u)
#define BI_DECODE_FRAMES 4096 /* frames per pull; 16 KiB stereo shorts */

/* One source file found for a key. */
typedef struct {
	char path[BI_PATH_MAX];
	char name[96]; /* file name only, for sorting and messages */
	uint64_t size;
	uint64_t hash;
	bool dup; /* byte-identical to an earlier candidate: dropped */
} bi_cand;

typedef struct {
	bi_cand* v;
	int n, cap;
} bi_cand_list;

static bool cand_push(bi_cand_list* l, const bi_cand* c) {
	if(l->n == l->cap) {
		int ncap = l->cap ? l->cap * 2 : 16;
		bi_cand* nv = realloc(l->v, (size_t)ncap * sizeof(*nv));
		if(!nv)
			return false;
		l->v = nv;
		l->cap = ncap;
	}
	l->v[l->n++] = *c;
	return true;
}

static void cand_free(bi_cand_list* l) {
	free(l->v);
	l->v = NULL;
	l->n = l->cap = 0;
}

/* ^<base>\d*\.(wav|ogg)$, case-insensitive (owned_importer._variant_pattern). */
static bool name_matches(const char* name, const char* base) {
	size_t bl = strlen(base);
	for(size_t k = 0; k < bl; k++)
		if(tolower((unsigned char)name[k]) != tolower((unsigned char)base[k]))
			return false;
	const char* p = name + bl;
	while(*p >= '0' && *p <= '9')
		p++;
	if(*p != '.')
		return false;
	p++;
	char ext[4];
	for(int k = 0; k < 3; k++) {
		if(!p[k])
			return false;
		ext[k] = (char)tolower((unsigned char)p[k]);
	}
	ext[3] = 0;
	if(p[3])
		return false;
	return strcmp(ext, "wav") == 0 || strcmp(ext, "ogg") == 0;
}

/* The helper lists with Python's sorted(): plain code-point order of the
   name. (On Windows pathlib compares case-folded; Beta's resources are all
   lower-case, so the two agree. Lower-case compare here keeps the Windows
   helper's order for odd-cased user files too.) */
static int cand_cmp(const void* a, const void* b) {
	const char* x = ((const bi_cand*)a)->name;
	const char* y = ((const bi_cand*)b)->name;
	for(;; x++, y++) {
		int cx = tolower((unsigned char)*x), cy = tolower((unsigned char)*y);
		if(cx != cy)
			return cx - cy;
		if(!cx)
			return strcmp(((const bi_cand*)a)->name, ((const bi_cand*)b)->name);
	}
}

/* size + FNV-1a 64 over the file, streamed. */
static bool file_hash(const char* path, uint64_t* size, uint64_t* hash,
					  uint8_t* buf, size_t buf_sz) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	uint64_t h = 0xcbf29ce484222325ull, total = 0;
	size_t got;
	while((got = fread(buf, 1, buf_sz, f)) > 0) {
		h = pack_cache_fnv(h, buf, got);
		total += got;
	}
	bool ok = !ferror(f);
	fclose(f);
	*size = total;
	*hash = h;
	return ok;
}

/* Byte compare of two files (same size already known). */
static bool files_equal(const char* a, const char* b, uint8_t* buf,
						size_t buf_sz) {
	FILE* fa = fopen(a, "rb");
	FILE* fb = fopen(b, "rb");
	bool eq = fa && fb;
	size_t half = buf_sz / 2;
	while(eq) {
		size_t ga = fread(buf, 1, half, fa);
		size_t gb = fread(buf + half, 1, half, fb);
		if(ga != gb || memcmp(buf, buf + half, ga) != 0)
			eq = false;
		if(ga == 0)
			break;
	}
	if(fa)
		fclose(fa);
	if(fb)
		fclose(fb);
	return eq;
}

/* "*.ogg" (the helper's Path.glob; case-insensitive like its Windows run). */
static bool is_ogg_name(const char* name) {
	size_t n = strlen(name);
	if(n < 5)
		return false;
	const char* e = name + n - 4;
	return e[0] == '.' && tolower((unsigned char)e[1]) == 'o'
		&& tolower((unsigned char)e[2]) == 'g'
		&& tolower((unsigned char)e[3]) == 'g';
}

/* Collect <root>/<subdir>'s matching regular files into `out` (base NULL:
   every *.ogg). A missing directory is simply empty; a listing error is
   reported. */
static bool scan_dir(const char* dir, const char* base, bi_cand_list* out,
					 int* first, bi_ctx* ctx) {
	*first = out->n;
	DIR* d = opendir(dir);
	if(!d)
		return true; /* helper: `if d.is_dir()` -- absent means none */
	struct dirent* e;
	bool ok = true;
	while(ok) {
		errno = 0;
		e = readdir(d);
		if(!e) {
			if(errno != 0) {
				char m[128];
				snprintf(m, sizeof(m), "Could not read sounds folder %.80s",
						 dir);
				bi_progress_set(ctx, BI_FAILED, 0, 0, m);
				ok = false;
			}
			break;
		}
		if(base ? !name_matches(e->d_name, base) : !is_ogg_name(e->d_name))
			continue;
		bi_cand c;
		memset(&c, 0, sizeof(c));
		if(snprintf(c.path, sizeof(c.path), "%s/%s", dir, e->d_name)
			   >= (int)sizeof(c.path)
		   || snprintf(c.name, sizeof(c.name), "%s", e->d_name)
			   >= (int)sizeof(c.name))
			continue; /* cannot be a Beta resource name */
		struct stat st;
		if(stat(c.path, &st) != 0 || !S_ISREG(st.st_mode))
			continue; /* helper: f.is_file() */
		if(!cand_push(out, &c)) {
			bi_progress_set(ctx, BI_FAILED, 0, 0,
							"Out of memory listing sounds");
			ok = false;
		}
	}
	closedir(d);
	/* sorted(d.iterdir()) -- per directory, before the next root. */
	if(ok && out->n - *first > 1)
		qsort(out->v + *first, (size_t)(out->n - *first), sizeof(bi_cand),
			  cand_cmp);
	return ok;
}

/* ---- decode one source to PCM16 mono at its native rate --------------- */

typedef enum { DEC_OK, DEC_BAD_FILE, DEC_OOM } dec_result;

static bool grow_pcm(int16_t** pcm, uint32_t* cap, uint32_t need) {
	if(need <= *cap)
		return true;
	uint32_t ncap = *cap ? *cap : 65536;
	while(ncap < need) {
		if(ncap > UINT32_MAX / 2)
			return false;
		ncap *= 2;
	}
	int16_t* np = realloc(*pcm, (size_t)ncap * 2);
	if(!np)
		return false;
	*pcm = np;
	*cap = ncap;
	return true;
}

static dec_result decode_ogg(const char* path, int16_t** out_pcm,
							 uint32_t* out_frames, uint32_t* out_rate) {
	int err = 0;
	stb_vorbis* v = stb_vorbis_open_filename(path, &err, NULL);
	if(!v)
		return DEC_BAD_FILE; /* stb_vorbis_decode_memory would fail too */
	stb_vorbis_info info = stb_vorbis_get_info(v);
	int channels = info.channels;
	if((channels != 1 && channels != 2) || info.sample_rate == 0) {
		stb_vorbis_close(v);
		return DEC_BAD_FILE;
	}
	/* Length hint only; the loop grows the buffer if the stream runs long. */
	unsigned hint = stb_vorbis_stream_length_in_samples(v);
	int16_t* pcm = NULL;
	uint32_t cap = 0, frames = 0;
	if(hint && !grow_pcm(&pcm, &cap, hint)) {
		stb_vorbis_close(v);
		return DEC_OOM;
	}
	/* Heap, not the worker's stack: 16 KiB of interleaved shorts. */
	short* block = malloc(sizeof(short) * BI_DECODE_FRAMES * 2);
	if(!block) {
		free(pcm);
		stb_vorbis_close(v);
		return DEC_OOM;
	}
	for(;;) {
		int n = stb_vorbis_get_samples_short_interleaved(
			v, channels, block, BI_DECODE_FRAMES * channels);
		if(n <= 0)
			break;
		if(!grow_pcm(&pcm, &cap, frames + (uint32_t)n)) {
			free(block);
			free(pcm);
			stb_vorbis_close(v);
			return DEC_OOM;
		}
		if(channels == 1) {
			memcpy(pcm + frames, block, (size_t)n * 2);
		} else {
			/* The tool's / engine's average downmix, verbatim. */
			for(int k = 0; k < n; k++)
				pcm[frames + (uint32_t)k] = (int16_t)(
					((int)block[2 * k] + (int)block[2 * k + 1]) / 2);
		}
		frames += (uint32_t)n;
	}
	free(block);
	stb_vorbis_close(v);
	if(frames == 0) {
		free(pcm);
		return DEC_BAD_FILE; /* the tool rejects frames <= 0 */
	}
	*out_pcm = pcm;
	*out_frames = frames;
	*out_rate = info.sample_rate;
	return DEC_OK;
}

static dec_result decode_wav(const char* path, int16_t** out_pcm,
							 uint32_t* out_frames, uint32_t* out_rate) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return DEC_BAD_FILE;
	uint32_t off = 0, frames = 0, rate = 0;
	if(!wavh_probe_file(f, &off, &frames, &rate)) {
		fclose(f);
		return DEC_BAD_FILE; /* exactly what wavh_parse rejects */
	}
	int16_t* pcm = malloc((size_t)frames * 2);
	if(!pcm) {
		fclose(f);
		return DEC_OOM;
	}
	if(fseek(f, (long)off, SEEK_SET) != 0
	   || fread(pcm, 2, frames, f) != frames) {
		free(pcm);
		fclose(f);
		return DEC_BAD_FILE;
	}
	fclose(f);
	for(uint32_t k = 0; k < frames; k++)
		pcm[k] = wavh_sample_from_le(pcm, k); /* LE words -> host order */
	*out_pcm = pcm;
	*out_frames = frames;
	*out_rate = rate;
	return DEC_OK;
}

static dec_result decode_source(const char* path, int16_t** pcm,
								uint32_t* frames, uint32_t* rate) {
	/* The tool detects by magic, not by extension. */
	FILE* f = fopen(path, "rb");
	if(!f)
		return DEC_BAD_FILE;
	uint8_t magic[4] = {0};
	size_t got = fread(magic, 1, 4, f);
	fclose(f);
	if(got < 4)
		return DEC_BAD_FILE;
	if(memcmp(magic, "OggS", 4) == 0)
		return decode_ogg(path, pcm, frames, rate);
	return decode_wav(path, pcm, frames, rate);
}

/* ---- write the canonical WAV in chunks --------------------------------- */

typedef enum { WR_OK, WR_IO, WR_CANCEL } wr_result;

static void put_u32le(uint8_t* p, uint32_t v) {
	for(int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static wr_result write_wav_chunked(const char* path, uint32_t rate,
								   const int16_t* pcm, uint32_t frames,
								   uint8_t* buf, size_t buf_sz, bi_ctx* ctx) {
	FILE* f = fopen(path, "wb");
	if(!f)
		return WR_IO;
	/* The tool's header via wavh_write (0 frames), then the two size
	   fields patched to the real length: byte-for-byte the 44-byte layout
	   wavh_write emits for `frames`. */
	uint8_t hdr[44];
	size_t hl = wavh_write(hdr, rate, NULL, 0);
	put_u32le(hdr + 4, 36 + frames * 2);
	put_u32le(hdr + 40, frames * 2);
	if(hl != 44 || fwrite(hdr, 1, 44, f) != 44) {
		fclose(f);
		return WR_IO;
	}
	uint32_t done = 0;
	uint32_t per = (uint32_t)(buf_sz / 2);
	while(done < frames) {
		if(bi_cancel_requested(ctx)) {
			fclose(f);
			return WR_CANCEL;
		}
		uint32_t n = frames - done;
		if(n > per)
			n = per;
		for(uint32_t k = 0; k < n; k++) {
			uint16_t v = (uint16_t)pcm[done + k];
			buf[2 * k] = (uint8_t)v;
			buf[2 * k + 1] = (uint8_t)(v >> 8);
		}
		if(fwrite(buf, 1, (size_t)n * 2, f) != (size_t)n * 2) {
			fclose(f);
			return WR_IO;
		}
		done += n;
	}
	if(fclose(f) != 0)
		return WR_IO;
	return WR_OK;
}

/* ---- the phase --------------------------------------------------------- */

static void key_split(const char* key, char* subdir, size_t subdir_sz,
					  const char** base) {
	/* "mob.zombiepig.zpig" -> subdir "mob/zombiepig", base "zpig";
	   "random.pop" -> "random", "pop"; a dot-free key -> "", key. */
	const char* last = strrchr(key, '.');
	if(!last) {
		subdir[0] = 0;
		*base = key;
		return;
	}
	size_t n = (size_t)(last - key);
	if(n >= subdir_sz)
		n = subdir_sz - 1;
	memcpy(subdir, key, n);
	subdir[n] = 0;
	for(size_t k = 0; k < n; k++)
		if(subdir[k] == '.')
			subdir[k] = '/';
	*base = last + 1;
}

static bool is_dir(const char* path) {
	struct stat st;
	return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Gather, dedup and mark the candidates of one key. Returns false on a
   reported failure (ctx msg set). */
static bool gather_key(const char* resources_dir, const char* key,
					   bi_cand_list* list, uint8_t* buf, size_t buf_sz,
					   bi_ctx* ctx) {
	char subdir[128];
	const char* base;
	key_split(key, subdir, sizeof(subdir), &base);
	static const char* const roots[2] = {"sound", "newsound"};
	for(int r = 0; r < 2; r++) {
		char dir[BI_PATH_MAX];
		int n = subdir[0] ? snprintf(dir, sizeof(dir), "%s/%s/%s",
									 resources_dir, roots[r], subdir)
						  : snprintf(dir, sizeof(dir), "%s/%s", resources_dir,
									 roots[r]);
		if(n <= 0 || n >= (int)sizeof(dir))
			continue;
		int first;
		if(!scan_dir(dir, base, list, &first, ctx))
			return false;
	}
	/* Dedup by bytes, keeping the first occurrence (set semantics). */
	for(int i = 0; i < list->n; i++) {
		bi_cand* c = &list->v[i];
		if(!file_hash(c->path, &c->size, &c->hash, buf, buf_sz)) {
			char m[128];
			snprintf(m, sizeof(m), "Could not read %.100s", c->name);
			bi_progress_set(ctx, BI_FAILED, 0, 0, m);
			return false;
		}
		for(int j = 0; j < i; j++) {
			const bi_cand* p = &list->v[j];
			if(p->dup || p->size != c->size || p->hash != c->hash)
				continue;
			if(files_equal(p->path, c->path, buf, buf_sz)) {
				c->dup = true;
				break;
			}
		}
	}
	return true;
}

/* ---- music/ + newmusic/ -> music/<name>, streaming/ -> records/ ----------
   Verbatim copies (audio_music.c pull-decodes the compressed file at play
   time; the helper never transcodes these), by the helper's process_music /
   process_records rules: music and newmusic both feed the one music pool,
   each listed sorted, the FIRST file of a given NAME wins (a repeated name
   is skipped, bytes not compared); records are streaming/*.ogg whose
   lower-cased stem is one of Beta 1.7.3's two records
   [owned_importer.RECORD_STEMS = ("13", "cat")], written as
   records/<stem_lower>.ogg, first wins. Candidates to skip are marked `dup`
   so the copy pass and the progress total share one list. */
static const char* const s_record_stems[] = {"13", "cat"};

static void lower_stem(const char* name, char* out, size_t out_sz) {
	size_t n = strlen(name);
	const char* dot = strrchr(name, '.');
	if(dot)
		n = (size_t)(dot - name);
	if(n >= out_sz)
		n = out_sz - 1;
	for(size_t k = 0; k < n; k++)
		out[k] = (char)tolower((unsigned char)name[k]);
	out[n] = 0;
}

static bool gather_music(const char* resources_dir, bi_cand_list* music,
						 bi_cand_list* records, bi_ctx* ctx) {
	static const char* const roots[2] = {"music", "newmusic"};
	char dir[BI_PATH_MAX];
	int first;
	for(int r = 0; r < 2; r++) {
		snprintf(dir, sizeof(dir), "%s/%s", resources_dir, roots[r]);
		if(!scan_dir(dir, NULL, music, &first, ctx))
			return false;
	}
	for(int i = 0; i < music->n; i++)
		for(int j = 0; j < i; j++)
			if(!music->v[j].dup
			   && strcmp(music->v[j].name, music->v[i].name) == 0) {
				music->v[i].dup = true; /* skipped_duplicate_name */
				break;
			}

	snprintf(dir, sizeof(dir), "%s/streaming", resources_dir);
	if(!scan_dir(dir, NULL, records, &first, ctx))
		return false;
	for(int i = 0; i < records->n; i++) {
		char stem[96], other[96];
		lower_stem(records->v[i].name, stem, sizeof(stem));
		bool known = false;
		for(size_t s = 0; s < sizeof(s_record_stems) / sizeof(*s_record_stems);
			s++)
			if(strcmp(stem, s_record_stems[s]) == 0)
				known = true;
		if(!known) {
			records->v[i].dup = true; /* not a Beta record: ignored */
			continue;
		}
		for(int j = 0; j < i; j++) {
			if(records->v[j].dup)
				continue;
			lower_stem(records->v[j].name, other, sizeof(other));
			if(strcmp(stem, other) == 0) {
				records->v[i].dup = true; /* a later same-stem file: skipped */
				break;
			}
		}
	}
	return true;
}

static int count_kept(const bi_cand_list* l) {
	int n = 0;
	for(int i = 0; i < l->n; i++)
		if(!l->v[i].dup)
			n++;
	return n;
}

/* Verbatim copy in BI_WRITE_CHUNK pieces, cancel checked between pieces. */
static wr_result copy_file_chunked(const char* src, const char* dst,
								   uint8_t* buf, size_t buf_sz, bi_ctx* ctx) {
	FILE* in = fopen(src, "rb");
	if(!in)
		return WR_IO;
	FILE* out = fopen(dst, "wb");
	if(!out) {
		fclose(in);
		return WR_IO;
	}
	wr_result r = WR_OK;
	for(;;) {
		if(bi_cancel_requested(ctx)) {
			r = WR_CANCEL;
			break;
		}
		size_t got = fread(buf, 1, buf_sz, in);
		if(got == 0) {
			if(ferror(in))
				r = WR_IO;
			break;
		}
		if(fwrite(buf, 1, got, out) != got) {
			r = WR_IO;
			break;
		}
	}
	fclose(in);
	if(fclose(out) != 0 && r == WR_OK)
		r = WR_IO;
	return r;
}

/* One copy pass over a gathered list into staging/<out_sub>/, naming each
   kept file by `record_names` (records/<stem>.ogg) or verbatim. */
static bool copy_pass(const bi_cand_list* l, const char* staging_dir,
					  const char* out_sub, bool record_names, uint8_t* buf,
					  int* done, int total, int* written, bi_ctx* ctx) {
	char out_dir[BI_PATH_MAX];
	if(snprintf(out_dir, sizeof(out_dir), "%s/%s", staging_dir, out_sub)
	   >= (int)sizeof(out_dir)) {
		bi_progress_set(ctx, BI_FAILED, *done, total, "Staging path too long");
		return false;
	}
	bool dir_ready = false;
	for(int i = 0; i < l->n; i++) {
		const bi_cand* c = &l->v[i];
		if(c->dup)
			continue;
		if(bi_cancel_requested(ctx)) {
			bi_progress_set(ctx, BI_CANCELLED, *done, total, "Import cancelled");
			return false;
		}
		char m[128];
		snprintf(m, sizeof(m), "Copying %.20s/%.90s", out_sub, c->name);
		bi_progress_set(ctx, BI_SOUNDS, *done, total, m);
		if(!dir_ready) {
			if(!pack_cache_ensure_dir(out_dir)) {
				snprintf(m, sizeof(m), "Could not create %.100s", out_dir);
				bi_progress_set(ctx, BI_FAILED, *done, total, m);
				return false;
			}
			dir_ready = true;
		}
		char out_name[96], out_path[BI_PATH_MAX];
		if(record_names) {
			char stem[90];
			lower_stem(c->name, stem, sizeof(stem));
			snprintf(out_name, sizeof(out_name), "%s.ogg", stem);
		} else {
			snprintf(out_name, sizeof(out_name), "%s", c->name);
		}
		if(snprintf(out_path, sizeof(out_path), "%s/%s", out_dir, out_name)
		   >= (int)sizeof(out_path)) {
			bi_progress_set(ctx, BI_FAILED, *done, total, "Staging path too long");
			return false;
		}
		wr_result wr = copy_file_chunked(c->path, out_path, buf, BI_WRITE_CHUNK,
										 ctx);
		if(wr != WR_OK) {
			remove(out_path);
			if(wr == WR_CANCEL) {
				bi_progress_set(ctx, BI_CANCELLED, *done, total,
								"Import cancelled");
			} else {
				snprintf(m, sizeof(m),
						 "Could not copy %.12s/%.48s (SD card full or "
						 "write-protected?)",
						 out_sub, out_name);
				bi_progress_set(ctx, BI_FAILED, *done, total, m);
			}
			return false;
		}
		(*written)++;
		(*done)++;
		bi_progress_step(ctx, *done);
	}
	return true;
}

int bi_sounds_import(const char* resources_dir, const char* staging_dir,
					 bi_ctx* ctx) {
	if(!staging_dir || !ctx)
		return -1;
	if(!is_dir(resources_dir)) {
		bi_progress_set(ctx, BI_SOUNDS, 0, 0,
						"No sounds folder: textures only. To add sounds, copy "
						"your .minecraft/resources folder to "
						"sd:/beta/resources and import again.");
		return 0;
	}

	uint8_t* buf = malloc(BI_WRITE_CHUNK);
	if(!buf) {
		bi_progress_set(ctx, BI_FAILED, 0, 0, "Out of memory importing sounds");
		return -1;
	}

	/* Pass 1: list + dedup every key, so the progress total is honest. */
	bi_cand_list lists[AUDIO_SOUND_COUNT], music, records;
	memset(lists, 0, sizeof(lists));
	memset(&music, 0, sizeof(music));
	memset(&records, 0, sizeof(records));
	int total = 0, written = 0, ret = -1;
	bi_progress_set(ctx, BI_SOUNDS, 0, 0, "Looking for Beta sounds...");
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(bi_cancel_requested(ctx)) {
			bi_progress_set(ctx, BI_CANCELLED, 0, 0, "Import cancelled");
			goto out;
		}
		if(!gather_key(resources_dir, s_keys[id], &lists[id], buf,
					   BI_WRITE_CHUNK, ctx))
			goto out;
		int n = 0;
		for(int i = 0; i < lists[id].n; i++)
			if(!lists[id].v[i].dup)
				n++;
		if(n > AUDIO_MAX_VARIANTS)
			n = AUDIO_MAX_VARIANTS; /* the overflow is dropped, not work */
		total += n;
	}
	int sound_total = total;
	if(!gather_music(resources_dir, &music, &records, ctx))
		goto out;
	total += count_kept(&music) + count_kept(&records);
	if(total == 0) {
		bi_progress_set(ctx, BI_SOUNDS, 0, 0,
						"No Beta sounds or music found in sd:/beta/resources: "
						"textures only.");
		ret = 0;
		goto out;
	}

	/* Pass 2: decode + cap + write, in the helper's order. */
	int done = 0;
	bi_progress_set(ctx, BI_SOUNDS, 0, total,
					sound_total ? "Converting sounds..." : "Copying music...");
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++) {
		bi_cand_list* l = &lists[id];
		if(l->n == 0)
			continue;
		char subdir[128];
		const char* base;
		key_split(s_keys[id], subdir, sizeof(subdir), &base);
		char out_dir[BI_PATH_MAX];
		int dl = subdir[0] ? snprintf(out_dir, sizeof(out_dir), "%s/sound/%s",
									  staging_dir, subdir)
						   : snprintf(out_dir, sizeof(out_dir), "%s/sound",
									  staging_dir);
		if(dl <= 0 || dl >= (int)sizeof(out_dir)) {
			bi_progress_set(ctx, BI_FAILED, 0, 0, "Staging path too long");
			goto out;
		}
		bool dir_ready = false;
		int n = 0;
		for(int i = 0; i < l->n; i++) {
			const bi_cand* c = &l->v[i];
			if(c->dup)
				continue;
			n++;
			if(n > AUDIO_MAX_VARIANTS)
				break; /* skipped_variant_overflow */
			if(bi_cancel_requested(ctx)) {
				bi_progress_set(ctx, BI_CANCELLED, done, total,
								"Import cancelled");
				goto out;
			}
			char m[128];
			snprintf(m, sizeof(m), "Converting %.60s/%.40s",
					 subdir[0] ? subdir : "sound", c->name);
			bi_progress_set(ctx, BI_SOUNDS, done, total, m);

			int16_t* pcm = NULL;
			uint32_t frames = 0, rate = 0;
			dec_result dr = decode_source(c->path, &pcm, &frames, &rate);
			if(dr == DEC_OOM) {
				snprintf(m, sizeof(m), "Out of memory decoding %.90s",
						 c->name);
				bi_progress_set(ctx, BI_FAILED, done, total, m);
				goto out;
			}
			if(dr == DEC_BAD_FILE) {
				/* strict_transcode: skipped_transcode_failed, n consumed */
				fprintf(stderr,
						"[beta_import] %s: not a decodable ogg or PCM16 mono "
						"WAV, skipped\n",
						c->path);
				done++;
				bi_progress_step(ctx, done);
				continue;
			}
			if(!bi_st_sound_transcode_rate_cap(&pcm, &frames, &rate)) {
				free(pcm);
				snprintf(m, sizeof(m), "Out of memory converting %.90s",
						 c->name);
				bi_progress_set(ctx, BI_FAILED, done, total, m);
				goto out;
			}
			if(!dir_ready) {
				if(!pack_cache_ensure_dir(out_dir)) {
					free(pcm);
					snprintf(m, sizeof(m), "Could not create %.100s", out_dir);
					bi_progress_set(ctx, BI_FAILED, done, total, m);
					goto out;
				}
				dir_ready = true;
			}
			char out_path[BI_PATH_MAX];
			int pl = n < AUDIO_MAX_VARIANTS
				? snprintf(out_path, sizeof(out_path), "%s/%s%d.wav", out_dir,
						   base, n)
				: snprintf(out_path, sizeof(out_path), "%s/%s.wav", out_dir,
						   base);
			if(pl <= 0 || pl >= (int)sizeof(out_path)) {
				free(pcm);
				bi_progress_set(ctx, BI_FAILED, done, total,
								"Staging path too long");
				goto out;
			}
			wr_result wr = write_wav_chunked(out_path, rate, pcm, frames, buf,
											 BI_WRITE_CHUNK, ctx);
			free(pcm);
			if(wr != WR_OK) {
				remove(out_path); /* never leave a torn file in staging */
				if(wr == WR_CANCEL) {
					bi_progress_set(ctx, BI_CANCELLED, done, total,
									"Import cancelled");
				} else {
					snprintf(m, sizeof(m),
							 "Could not write %.60s (SD card full or "
							 "write-protected?)",
							 out_path + strlen(staging_dir) + 1);
					bi_progress_set(ctx, BI_FAILED, done, total, m);
				}
				goto out;
			}
			written++;
			done++;
			bi_progress_step(ctx, done);
		}
	}
	{
		char m[128];
		snprintf(m, sizeof(m), "Converted %d sound file%s", written,
				 written == 1 ? "" : "s");
		bi_progress_set(ctx, BI_SOUNDS, done, total, m);
	}

	/* Pass 3: music/ + newmusic/ -> music/, streaming/ -> records/. */
	int sounds_written = written;
	if(!copy_pass(&music, staging_dir, "music", false, buf, &done, total,
				  &written, ctx)
	   || !copy_pass(&records, staging_dir, "records", true, buf, &done, total,
					 &written, ctx))
		goto out;
	{
		char m[128];
		snprintf(m, sizeof(m), "Converted %d sound file%s, copied %d music file%s",
				 sounds_written, sounds_written == 1 ? "" : "s",
				 written - sounds_written, written - sounds_written == 1 ? "" : "s");
		bi_progress_set(ctx, BI_SOUNDS, total, total, m);
	}
	ret = written;

out:
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++)
		cand_free(&lists[id]);
	cand_free(&music);
	cand_free(&records);
	free(buf);
	return ret;
}

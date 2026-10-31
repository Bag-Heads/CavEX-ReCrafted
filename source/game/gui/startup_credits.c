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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "startup_credits.h"

/* ---- manifest ---------------------------------------------------------- */

static int sc_sprite_index(const struct startup_credits_data* d,
						   const char* name) {
	for(int k = 0; k < d->nsprites; k++)
		if(strcmp(d->sprite_names[k], name) == 0)
			return k;
	return -1;
}

static int sc_texture_index(const struct startup_credits_data* d,
							const char* name) {
	for(int k = 0; k < d->ntextures; k++)
		if(strcmp(d->tex[k].name, name) == 0)
			return k;
	return -1;
}

static bool sc_hex_rgb(const char* hex, uint8_t* r, uint8_t* g, uint8_t* b) {
	unsigned v;
	char tail;
	if(strlen(hex) != 6 || sscanf(hex, "%6x%c", &v, &tail) != 1)
		return false;
	*r = (uint8_t)(v >> 16);
	*g = (uint8_t)(v >> 8);
	*b = (uint8_t)v;
	return true;
}

static bool sc_on_screen(int x, int y, int w, int h) {
	return x >= 0 && y >= 0 && x + w <= 640 && y + h <= 480;
}

static int sc_parse_item(struct startup_credits_data* d, const char* key,
						 char* rest) {
	if(!d->ncards)
		return SC_LOAD_ERR_PARSE;
	struct sc_card* c = &d->cards[d->ncards - 1];
	if(c->nitems == SC_MAX_ITEMS)
		return SC_LOAD_ERR_PARSE;
	struct sc_item* it = &c->items[c->nitems];
	memset(it, 0, sizeof(*it));
	int x, y, a = 0, n = 0;
	bool seq = key[0] == 's';
	if((seq ? sscanf(rest, "%d %d %n", &x, &y, &n) != 2
			: sscanf(rest, "%d %d %d %n", &x, &y, &a, &n) != 3)
	   || x < 0 || y < 0 || x > 640 || y > 480)
		return SC_LOAD_ERR_PARSE;
	it->x = (int16_t)x;
	it->y = (int16_t)y;
	char* tail = rest + n;
	if(key[0] != 't') {
		/* anim <x> <y> <ms> <sprite>...   (every step the same ms)
		 * seq  <x> <y> <sprite>:<ms>...   (per-step table, sprites reused) */
		if(a < 0 || a > 60000)
			return SC_LOAD_ERR_PARSE;
		it->kind = SC_ITEM_ANIM;
		for(char* tok = strtok(tail, " \t"); tok; tok = strtok(NULL, " \t")) {
			unsigned ms = (unsigned)a;
			char* colon = strchr(tok, ':');
			if(seq) {
				char tail2;
				if(!colon || sscanf(colon + 1, "%u%c", &ms, &tail2) != 1 || !ms
				   || ms > 60000)
					return SC_LOAD_ERR_PARSE;
				*colon = '\0';
			} else if(colon) {
				return SC_LOAD_ERR_PARSE;
			}
			int k = sc_sprite_index(d, tok);
			if(k < 0 || it->nsteps == SC_MAX_STEPS
			   || !sc_on_screen(x, y, d->sprites[k].w, d->sprites[k].h))
				return SC_LOAD_ERR_PARSE;
			it->step_sprite[it->nsteps] = (uint8_t)k;
			it->step_ms[it->nsteps++] = (uint16_t)ms;
			it->loop_ms += ms;
		}
		if(!it->nsteps)
			return SC_LOAD_ERR_PARSE;
	} else {
		char hex[16];
		int m = 0;
		if(a < 8 || a > 64 || a % 8 || sscanf(tail, "%15s %n", hex, &m) != 1
		   || !sc_hex_rgb(hex, &it->r, &it->g, &it->b) || !tail[m]
		   || strlen(tail + m) >= SC_TEXT_MAX)
			return SC_LOAD_ERR_PARSE;
		it->kind = SC_ITEM_TEXT;
		it->size = (uint8_t)a;
		snprintf(it->text, sizeof(it->text), "%s", tail + m);
		for(const char* q = it->text; *q; q++)
			if((unsigned char)*q < 32 || (unsigned char)*q > 126)
				return SC_LOAD_ERR_PARSE;
	}
	c->nitems++;
	return SC_LOAD_OK;
}

int startup_credits_parse(struct startup_credits_data* d, const char* text) {
	memset(d, 0, sizeof(*d));
	if(!text)
		return SC_LOAD_ERR_PARSE;
	char line[512]; /* a seq table is one long line */
	const char* p = text;
	bool header = false, ended = false, timing = false, footer = false;
	size_t tex_bytes = 0;
	while(*p && !ended) {
		size_t n = strcspn(p, "\n");
		if(n >= sizeof(line))
			return SC_LOAD_ERR_PARSE;
		memcpy(line, p, n);
		line[n] = '\0';
		if(n && line[n - 1] == '\r')
			line[n - 1] = '\0';
		p += n + (p[n] == '\n');
		char key[16];
		int off = 0;
		if(sscanf(line, "%15s %n", key, &off) != 1)
			continue; /* blank */
		char* rest = line + off;
		if(!header) {
			if(strcmp(key, "startup-credits") != 0 || strcmp(rest, "2") != 0)
				return SC_LOAD_ERR_PARSE;
			header = true;
		} else if(!strcmp(key, "texture")) {
			if(d->ntextures == SC_MAX_TEXTURES)
				return SC_LOAD_ERR_PARSE;
			struct sc_texture* t = &d->tex[d->ntextures];
			unsigned w, h;
			if(sscanf(rest, "%15s %63s %u %u", t->name, t->file, &w, &h) != 4
			   || !w || !h || w > 1024 || h > 1024 || w % 4 || h % 4
			   || strchr(t->file, '/') || strchr(t->file, '\\')
			   || sc_texture_index(d, t->name) >= 0)
				return SC_LOAD_ERR_PARSE;
			t->w = (uint16_t)w;
			t->h = (uint16_t)h;
			tex_bytes += (size_t)w * h * 4;
			if(tex_bytes > STARTUP_CREDITS_TEXTURE_BUDGET)
				return SC_LOAD_ERR_PARSE;
			d->ntextures++;
		} else if(!strcmp(key, "timing")) {
			unsigned fi, dw, fo, st;
			if(sscanf(rest, "%u %u %u %u", &fi, &dw, &fo, &st) != 4 || !dw
			   || dw > 30000 || fi > 5000 || fo > 5000 || st > 600000)
				return SC_LOAD_ERR_PARSE;
			d->fade_in_ms = fi;
			d->dwell_ms = dw;
			d->fade_out_ms = fo;
			d->stall_ms = st;
			timing = true;
		} else if(!strcmp(key, "sprite")) {
			char name[16], tex[16];
			unsigned x, y, w, h;
			int t;
			if(d->nsprites == SC_MAX_SPRITES
			   || sscanf(rest, "%15s %15s %u %u %u %u", name, tex, &x, &y, &w,
						 &h) != 6
			   || (t = sc_texture_index(d, tex)) < 0 || !w || !h
			   /* each value on its own, then subtraction: no wrap */
			   || x >= d->tex[t].w || y >= d->tex[t].h
			   || w > (unsigned)d->tex[t].w - x
			   || h > (unsigned)d->tex[t].h - y
			   || sc_sprite_index(d, name) >= 0)
				return SC_LOAD_ERR_PARSE;
			snprintf(d->sprite_names[d->nsprites], 16, "%s", name);
			d->sprites[d->nsprites++] = (struct sc_sprite) {(uint8_t)t,
				(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h};
		} else if(!strcmp(key, "card")) {
			if(d->ncards == SC_MAX_CARDS)
				return SC_LOAD_ERR_PARSE;
			d->ncards++;
		} else if(!strcmp(key, "anim") || !strcmp(key, "seq")
				  || !strcmp(key, "text")) {
			int r = sc_parse_item(d, key, rest);
			if(r != SC_LOAD_OK)
				return r;
		} else if(!strcmp(key, "footer")) {
			int y, sz;
			char hex[16];
			if(footer || sscanf(rest, "%d %d %15s", &y, &sz, hex) != 3 || y < 32
			   || y > 448 || sz < 8 || sz > 32 || sz % 8
			   || !sc_hex_rgb(hex, &d->footer_r, &d->footer_g, &d->footer_b))
				return SC_LOAD_ERR_PARSE;
			d->footer_y = (int16_t)y;
			d->footer_size = (uint8_t)sz;
			footer = true;
		} else if(!strcmp(key, "end")) {
			ended = true;
		} else {
			return SC_LOAD_ERR_PARSE;
		}
	}
	/* The font is texture 1: 16x16 cells of 8x8 texels. */
	if(!header || !ended || !timing || !footer || !d->ncards
	   || d->ntextures != 2 || strcmp(d->tex[SC_TEX_FONT].name, "font") != 0
	   || d->tex[SC_TEX_FONT].w != 128 || d->tex[SC_TEX_FONT].h != 128)
		return SC_LOAD_ERR_PARSE;
	for(int k = 0; k < d->ncards; k++)
		if(!d->cards[k].nitems)
			return SC_LOAD_ERR_PARSE;
	return SC_LOAD_OK;
}

static char* sc_read_text(const char* path, size_t max) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return NULL;
	char* buf = malloc(max + 1);
	size_t n = buf ? fread(buf, 1, max + 1, f) : 0;
	bool bad = !buf || ferror(f) || n > max;
	fclose(f);
	if(bad) {
		free(buf);
		return NULL;
	}
	buf[n] = '\0';
	return buf;
}

static uint32_t sc_be32(const uint8_t* b) {
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8
		| b[3];
}

static int sc_load_texture(struct sc_texture* t, const char* dir) {
	char path[512];
	if(snprintf(path, sizeof(path), "%s/%s", dir, t->file)
	   >= (int)sizeof(path))
		return SC_LOAD_ERR_TEXTURE;
	FILE* f = fopen(path, "rb");
	if(!f)
		return SC_LOAD_ERR_TEXTURE;
	uint8_t hdr[12];
	size_t bytes = (size_t)t->w * t->h * 4;
	bool ok = fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "CWSA", 4)
		&& sc_be32(hdr + 4) == t->w && sc_be32(hdr + 8) == t->h;
	if(!ok) {
		fclose(f);
		return SC_LOAD_ERR_TEXTURE;
	}
	t->rgba = malloc(bytes);
	if(!t->rgba) {
		fclose(f);
		return SC_LOAD_ERR_ALLOC;
	}
	uint8_t extra;
	ok = fread(t->rgba, 1, bytes, f) == bytes && fread(&extra, 1, 1, f) == 0;
	fclose(f);
	if(!ok) {
		free(t->rgba);
		t->rgba = NULL;
		return SC_LOAD_ERR_TEXTURE;
	}
	return SC_LOAD_OK;
}

/* Beta FontRenderer widths: the rightmost lit column of each 8x8 cell + 1
 * (space = 3), from the font texture's alpha -- the rule the offline layout
 * used, so run-time text lands exactly where the design put it. */
static void sc_glyph_widths(struct startup_credits_data* d) {
	const struct sc_texture* f = &d->tex[SC_TEX_FONT];
	for(int c = 0; c < 256; c++) {
		int w = 0;
		for(int x = 0; x < 8; x++)
			for(int y = 0; y < 8; y++)
				if(f->rgba[(((c / 16) * 8 + y) * 128 + (c % 16) * 8 + x) * 4 + 3])
					w = x + 1;
		d->glyph_w[c] = (uint8_t)(c == 32 ? 3 : w);
	}
	d->glyphs_ready = true;
}

int startup_credits_load(struct startup_credits_data* d, const char* dir) {
	char path[512];
	snprintf(path, sizeof(path), "%s/credits.txt", dir);
	char* text = sc_read_text(path, 8192);
	if(!text) {
		memset(d, 0, sizeof(*d));
		return SC_LOAD_ERR_MANIFEST;
	}
	int r = startup_credits_parse(d, text);
	free(text);
	if(r != SC_LOAD_OK)
		return r;
	for(int k = 0; k < d->ntextures; k++) {
		r = sc_load_texture(&d->tex[k], dir);
		if(r != SC_LOAD_OK) {
			startup_credits_data_free(d);
			return r;
		}
	}
	sc_glyph_widths(d);
	return SC_LOAD_OK;
}

uint8_t* startup_credits_take_texture(struct startup_credits_data* d, int k) {
	if(!d || k < 0 || k >= d->ntextures)
		return NULL;
	uint8_t* p = d->tex[k].rgba;
	d->tex[k].rgba = NULL;
	return p;
}

void startup_credits_data_free(struct startup_credits_data* d) {
	if(!d)
		return;
	for(int k = 0; k < SC_MAX_TEXTURES; k++) {
		free(d->tex[k].rgba);
		d->tex[k].rgba = NULL;
	}
}

static uint32_t sc_period(const struct startup_credits_data* d) {
	return d->fade_in_ms + d->dwell_ms + d->fade_out_ms;
}

uint32_t startup_credits_nominal_ms(const struct startup_credits_data* d) {
	return d ? d->ncards * sc_period(d) : 0;
}

int startup_credits_anim_step(const struct sc_item* it, uint32_t since_ms) {
	if(!it->loop_ms)
		return 0;
	uint32_t t = since_ms % it->loop_ms;
	for(int k = 0; k < it->nsteps; k++) {
		if(t < it->step_ms[k])
			return k;
		t -= it->step_ms[k];
	}
	return it->nsteps - 1; /* unreachable: the steps sum to loop_ms */
}

int startup_credits_text_width(const struct startup_credits_data* d,
	const char* s, int size) {
	int w = 0;
	for(; s && *s; s++)
		w += (d->glyph_w[(unsigned char)*s] + 1) * size / 8;
	return w;
}

/* ---- sequence ---------------------------------------------------------- */

void startup_credits_begin(struct startup_credits* s,
	const struct startup_credits_data* d, bool first_run, uint32_t now_ms) {
	memset(s, 0, sizeof(*s));
	s->d = d;
	s->first_run = first_run;
	s->start_ms = now_ms;
	s->now_ms = now_ms;
	s->phase = SC_SHOWING;
}

static uint32_t sc_elapsed(const struct startup_credits* s) {
	return s->clock_ms;
}

/* SHOWING: card k occupies [k*period, (k+1)*period): fade in, readable,
 * fade out. The last card never fades out here; it holds until the exit. */
static struct sc_visible sc_showing(const struct startup_credits* s) {
	const struct startup_credits_data* d = s->d;
	uint32_t t = sc_elapsed(s), period = sc_period(d);
	uint32_t k = period ? t / period : 0;
	if(k > (uint32_t)d->ncards - 1u)
		k = d->ncards - 1u;
	uint32_t in = t - k * period;
	unsigned a = 255;
	if(in < d->fade_in_ms)
		a = 255 * in / d->fade_in_ms;
	else if(k + 1 < d->ncards && in >= d->fade_in_ms + d->dwell_ms)
		a = 255 - 255 * (in - d->fade_in_ms - d->dwell_ms)
			/ (d->fade_out_ms ? d->fade_out_ms : 1);
	return (struct sc_visible) {(uint8_t)k, (uint8_t)a};
}

/* First-run dwells are complete once the last card was readable for its
 * full dwell; its fade-out is then the exit to the title. */
static uint32_t sc_dwells_done_ms(const struct startup_credits_data* d) {
	return startup_credits_nominal_ms(d) - d->fade_out_ms;
}

static void sc_finish(struct startup_credits* s, bool a_held) {
	s->phase = SC_DONE;
	if(a_held)
		s->block_title_a = true;
}

void startup_credits_update(struct startup_credits* s, uint32_t now_ms,
	bool ready, bool a_edge, bool a_held) {
	if(!s->d || s->phase == SC_DONE)
		return;
	uint32_t dt = now_ms - s->now_ms;
	s->clock_ms += dt < SC_MAX_FRAME_STEP_MS ? dt : SC_MAX_FRAME_STEP_MS;
	s->now_ms = now_ms;
	if(s->phase == SC_SHOWING) {
		bool dwells = sc_elapsed(s) >= sc_dwells_done_ms(s->d);
		if(s->first_run && dwells)
			s->full_play = true;
		if(ready && !s->error && (!s->first_run || dwells)) {
			struct sc_visible v = sc_showing(s);
			s->phase = SC_EXITING;
			s->exit_start_ms = s->clock_ms;
			s->exit_card = v.card;
			s->exit_alpha = v.alpha;
		}
	}
	if(s->phase == SC_EXITING && (!ready || s->error)) {
		/* Readiness revoked or an error raised mid-fade: the cards come
		 * back and hold (never DONE, never a marker) until ready again. */
		s->phase = SC_SHOWING;
	}
	if(s->phase == SC_EXITING) {
		/* Later boots: a FRESH A after readiness cuts the fade-out. A pre-
		 * ready press is not buffered: the edge must come on a frame after
		 * one that already saw ready. */
		if(!s->first_run && a_edge && s->ready_seen)
			sc_finish(s, true);
		else if(s->clock_ms - s->exit_start_ms >= s->d->fade_out_ms)
			sc_finish(s, a_held);
	}
	s->ready_seen = ready;
}

void startup_credits_set_phase(struct startup_credits* s, const char* word) {
	snprintf(s->phase_word, sizeof(s->phase_word), "%s", word ? word : "");
}

void startup_credits_set_error(struct startup_credits* s, const char* text) {
	s->error = text != NULL;
	snprintf(s->error_text, sizeof(s->error_text), "%s", text ? text : "");
}

bool startup_credits_done(const struct startup_credits* s) {
	return s->phase == SC_DONE;
}

bool startup_credits_title_input_blocked(struct startup_credits* s,
	bool a_held) {
	if(s->block_title_a && !a_held)
		s->block_title_a = false;
	return s->block_title_a;
}

enum startup_credits_footer startup_credits_footer_state(
	const struct startup_credits* s) {
	if(s->error)
		return SC_FOOTER_ERROR;
	/* Only while NOT ready (a ready first boot still owes its dwells), and
	 * on REAL elapsed time: the 100 ms-capped presented clock would make the
	 * warning ~10x late at one serviced frame/s. */
	if(s->phase == SC_SHOWING && !s->ready_seen && s->now_ms - s->start_ms
		   > startup_credits_nominal_ms(s->d) + s->d->stall_ms)
		return SC_FOOTER_STALL;
	return SC_FOOTER_NONE;
}

void startup_credits_footer_text(const struct startup_credits* s, char* out,
	size_t size) {
	if(!size)
		return;
	switch(startup_credits_footer_state(s)) {
		case SC_FOOTER_ERROR: snprintf(out, size, "%s", s->error_text); break;
		case SC_FOOTER_STALL:
			if(s->phase_word[0])
				snprintf(out, size, "Still loading %s...", s->phase_word);
			else
				snprintf(out, size, "Still loading...");
			break;
		default: out[0] = '\0'; break;
	}
}

int startup_credits_visible(const struct startup_credits* s,
	struct sc_visible out[1]) {
	if(!s->d || s->phase == SC_DONE)
		return 0;
	if(s->phase == SC_EXITING) {
		uint32_t f = s->clock_ms - s->exit_start_ms, fade = s->d->fade_out_ms;
		uint32_t a = fade ? s->exit_alpha * (fade - (f < fade ? f : fade)) / fade
						  : 0;
		out[0] = (struct sc_visible) {s->exit_card, (uint8_t)a};
		return 1;
	}
	out[0] = sc_showing(s);
	return 1;
}

static void sc_text(const struct startup_credits_data* d, int x, int y,
	int size, const char* str, uint8_t r, uint8_t g, uint8_t b, uint8_t a,
	startup_credits_quad_fn quad, void* user) {
	/* Beta shadow (colour/4 at +size/8) first, then the glyphs. */
	for(int pass = 0; pass < 2; pass++) {
		int cx = x + (pass ? 0 : size / 8), cy = y + (pass ? 0 : size / 8);
		uint8_t rr = pass ? r : r / 4, gg = pass ? g : g / 4;
		uint8_t bb = pass ? b : b / 4;
		for(const char* q = str; *q; q++) {
			unsigned c = (unsigned char)*q;
			if(c != 32)
				quad(user, SC_TEX_FONT, (c % 16) * 8, (c / 16) * 8, 8, 8, cx,
					 cy, size, size, rr, gg, bb, a);
			cx += (d->glyph_w[c] + 1) * size / 8;
		}
	}
}

void startup_credits_draw(const struct startup_credits* s,
	startup_credits_quad_fn quad, void* user) {
	const struct startup_credits_data* d = s->d;
	struct sc_visible v[1];
	if(startup_credits_visible(s, v) && v[0].alpha) {
		const struct sc_card* c = &d->cards[v[0].card];
		/* Animation clock: from the card's first presented moment, running
		 * through its fades. */
		uint32_t card_start = v[0].card * sc_period(d);
		uint32_t since = sc_elapsed(s) > card_start ? sc_elapsed(s) - card_start
													: 0;
		for(int i = 0; i < c->nitems; i++) {
			const struct sc_item* it = &c->items[i];
			if(it->kind == SC_ITEM_ANIM) {
				const struct sc_sprite* sp = &d->sprites[
					it->step_sprite[startup_credits_anim_step(it, since)]];
				quad(user, sp->tex, sp->x, sp->y, sp->w, sp->h, it->x, it->y,
					 sp->w, sp->h, 255, 255, 255, v[0].alpha);
			} else {
				sc_text(d, it->x, it->y, it->size, it->text, it->r, it->g,
						it->b, v[0].alpha, quad, user);
			}
		}
	}
	char foot[128];
	startup_credits_footer_text(s, foot, sizeof(foot));
	if(foot[0]) {
		int w = startup_credits_text_width(d, foot, d->footer_size);
		sc_text(d, (640 - w) / 2, d->footer_y, d->footer_size, foot,
				d->footer_r, d->footer_g, d->footer_b, 255, quad, user);
	}
}

/* ---- marker ------------------------------------------------------------ */

#define SC_MARKER_CONTENT "startup-credits-v1\n"

bool startup_credits_marker_due(const struct startup_credits* s) {
	return s->first_run && s->full_play && s->phase == SC_DONE
		&& !s->marker_written;
}

void startup_credits_marker_noted(struct startup_credits* s) {
	s->marker_written = true;
}

bool startup_credits_marker_path(const char* config_path, char* out,
	size_t size) {
	if(!config_path || !out || !size)
		return false;
	const char* slash = strrchr(config_path, '/');
	const char* bslash = strrchr(config_path, '\\');
	if(bslash && (!slash || bslash > slash))
		slash = bslash;
	int n = slash ? snprintf(out, size, "%.*s/%s",
							 (int)(slash - config_path), config_path,
							 STARTUP_CREDITS_MARKER)
				  : snprintf(out, size, "%s", STARTUP_CREDITS_MARKER);
	return n > 0 && (size_t)n < size;
}

bool startup_credits_marker_seen(const char* path) {
	/* Exact byte count and bytes: an embedded NUL or any trailing byte is
	 * corrupt (first-run behaviour). */
	FILE* f = path ? fopen(path, "rb") : NULL;
	if(!f)
		return false;
	char buf[64];
	size_t n = fread(buf, 1, sizeof(buf), f);
	bool ok = !ferror(f) && n == strlen(SC_MARKER_CONTENT)
		&& memcmp(buf, SC_MARKER_CONTENT, n) == 0;
	fclose(f);
	return ok;
}

static unsigned s_fail_at[3];

void startup_credits_test_fail(int op, unsigned nth) {
	if(op >= 0 && op < 3)
		s_fail_at[op] = nth;
}

static bool sc_op_ok(int op) {
	if(!s_fail_at[op])
		return true;
	return --s_fail_at[op] != 0;
}

int startup_credits_marker_write(const char* path) {
	char tmp[600];
	if(!path || snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return SC_MARKER_ERR_OPEN;
	/* A valid marker is already the result: nothing to replace or lose. */
	if(startup_credits_marker_seen(path))
		return SC_MARKER_OK;
	FILE* f = sc_op_ok(0) ? fopen(tmp, "wb") : NULL;
	if(!f)
		return SC_MARKER_ERR_OPEN;
	size_t len = strlen(SC_MARKER_CONTENT);
	bool ok = sc_op_ok(1) && fwrite(SC_MARKER_CONTENT, 1, len, f) == len
		&& fflush(f) == 0;
	ok = (fclose(f) == 0) && ok;
	if(!ok) {
		remove(tmp);
		return SC_MARKER_ERR_WRITE;
	}
	/* Temp first, then rename. Only where rename cannot replace (MSVCRT)
	 * is the old file removed -- and it is never a valid marker here (that
	 * returned above), so a failure can only leave first-run behaviour. */
	if(!sc_op_ok(2)) {
		remove(tmp);
		return SC_MARKER_ERR_RENAME;
	}
	if(rename(tmp, path) != 0) {
		remove(path);
		if(rename(tmp, path) != 0) {
			remove(tmp);
			return SC_MARKER_ERR_RENAME;
		}
	}
	return SC_MARKER_OK;
}

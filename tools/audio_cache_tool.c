/*
	Copyright (c) 2026 CaveWii_v2 contributors

	audio_cache_tool -- the PC producer of the opt-in audio pack cache
	(pack-cache-0927; Astra astra-packcacheB-0927 item 3: "a small C host tool
	sharing the production conversion code, called by the Python helper").

	It runs the PRODUCTION sound registry scan (source/platform/
	audio_registry.c, linked from the production object pool -- the same WAV
	parse, ogg decode/downmix, pass order, per-key fallback and arena policy
	the console runs) over one pack, then publishes the completed registry as
	a cache generation for each requested target through the same
	audio_cache.c writer the console uses. The Wii target gets big-endian
	sample words and the Wii's key (target id is part of both the key and the
	header), so a console with the full 12 MB audio region and the same files
	HITS it without decoding anything; a clipped region or any changed file
	misses and the console decodes fresh and republishes, exactly as without
	the tool.

	TEXTURES (pack-cache-0927 texture slice): the same tool runs the
	PRODUCTION tex_init pack load (a hidden GL context, PC backend) and
	publishes texture generations through texture_cache.c: the PC target
	from the live state, the Wii target by encoding the same RGBA images
	with the shared GX encoders (tex_gx_conv.c, byte-identical to the Wii
	backend's own conversion) -- including the 4-LOD atlas mip chains.

	MANIFEST (astra-packkey-0927): --manifest first writes the pack's helper
	digest manifest (<pack>/.cavewii/manifest.cwm) from the FINAL installed
	bytes; that is what makes the pack "helper-managed". Without a manifest
	both caches key the pack by a full hash of the files the load reads. The
	audio cache serves only roots that ship OGG sounds (measured: a fresh WAV
	load beats it); a WAV-only root prints "bypassed" and is not an error.
	The Pack Helper calls this
	tool with --manifest as the last step of an import, after the pack's
	files are in place, and treats pack + manifest + caches as one unit.

	Usage (run from the directory the game resolves the bundled assets/
	root against -- the SD card root on a standard card, the PC build's
	working directory on PC; a wrong directory only costs the console one
	rebuild, since it re-hashes the bundled files a generation depends on):
	    audio_cache_tool <pack-root> [--target wii|pc|both] [--manifest]
	                     [--only audio|textures] [--bench N]
	<pack-root> is the texture/audio pack directory ("assets" = the bundled
	root, cached under .cavewii/bundled-audio and .cavewii/bundled-textures;
	it never takes a manifest). --bench N additionally times N fresh loads vs
	N cache hits of that pack on this PC and prints the numbers (host
	measurement only; the console numbers need a real Wii run).

	Exit codes: 0 published (or audio bypassed by the measured policy), 1
	usage, 2 no registry (no arena / nothing loaded / no GL context), 3
	publication refused (unwritable destination, incomplete texture load), 4
	manifest failed.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "config.h"
#include "game/game_state.h"
#include "platform/audio.h"
#include "platform/audio_cache.h"
#include "platform/pack_manifest.h"
#include "platform/texture.h"
#include "platform/texture_cache.h"
#include "platform/time.h"

static void set(const char* key, const char* value) {
	if(!config_set_string(&gstate.config_user, key, value)) {
		fprintf(stderr, "audio_cache_tool: cannot set %s\n", key);
		exit(2);
	}
}

static double reload_ms(void) {
	ptime_t t0 = time_get();
	audio_registry_reload();
	return (double)time_diff_s(t0, time_get()) * 1000.0;
}

static double tex_ms(bool* ok) {
	ptime_t t0 = time_get();
	*ok = tex_init();
	return (double)time_diff_s(t0, time_get()) * 1000.0;
}

/* Texture half: returns the exit code contribution (0 or 2/3). */
static int textures(const char* pack, bool wii, bool pc, int bench) {
	if(!glfwInit())
		return 2;
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* gl = glfwCreateWindow(32, 32, "audio_cache_tool", NULL, NULL);
	if(!gl) {
		glfwTerminate();
		return 2;
	}
	glfwMakeContextCurrent(gl);
	if(glewInit() != GLEW_OK) {
		glfwDestroyWindow(gl);
		glfwTerminate();
		return 2;
	}
	int rc = 0;
	set("pack.cache", "true");
	tex_cache_tool_targets((pc ? 1u << TEX_CACHE_TARGET_PC : 0u)
						   | (wii ? 1u << TEX_CACHE_TARGET_WII : 0u));
	bool ok;
	double ms = tex_ms(&ok);
	char dir[400];
	tex_cache_dir(dir, sizeof(dir));
	unsigned key_us, read_us, write_us;
	tex_cache_timings(&key_us, &read_us, &write_us);
	printf("textures of \"%s\": fresh load + publish %.1f ms (write %.1f "
		   "ms), load %s, cache -> %s: %s\n",
		   pack, ms, write_us / 1000.0, ok ? "complete" : "INCOMPLETE", dir,
		   tex_cache_status());
	if(strcmp(tex_cache_status(), "written") != 0)
		rc = 3;

	if(bench > 0 && rc == 0 && pc) {
		double fresh = 0, hit = 0, key = 0;
		set("pack.cache", "false");
		for(int i = 0; i < bench; i++)
			fresh += tex_ms(&ok);
		set("pack.cache", "true");
		bool all_hit = true;
		for(int i = 0; i < bench; i++) {
			hit += tex_ms(&ok);
			all_hit = all_hit && strcmp(tex_cache_status(), "hit") == 0;
			tex_cache_timings(&key_us, &read_us, &write_us);
			key += key_us / 1000.0;
		}
		printf("textures bench x%d: fresh load %.2f ms/load, cache %s %.2f "
			   "ms/load (of which source re-hash %.2f ms)\n",
			   bench, fresh / bench, all_hit ? "hit" : "NOT ALL HITS",
			   hit / bench, key / bench);
	}
	glfwDestroyWindow(gl);
	glfwTerminate();
	return rc;
}

int main(int argc, char** argv) {
	if(argc < 2) {
		fprintf(stderr, "usage: %s <pack-root> [--target wii|pc|both] "
						"[--manifest] [--only audio|textures] [--bench N]\n",
				argv[0]);
		return 1;
	}
	const char* pack = argv[1];
	bool wii = true, pc = true, manifest = false;
	bool do_audio = true, do_tex = true;
	int bench = 0;
	for(int i = 2; i < argc; i++) {
		if(strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
			const char* t = argv[++i];
			wii = strcmp(t, "wii") == 0 || strcmp(t, "both") == 0;
			pc = strcmp(t, "pc") == 0 || strcmp(t, "both") == 0;
			if(!wii && !pc) {
				fprintf(stderr, "audio_cache_tool: unknown target '%s'\n", t);
				return 1;
			}
		} else if(strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
			bench = atoi(argv[++i]);
		} else if(strcmp(argv[i], "--manifest") == 0) {
			manifest = true;
		} else if(strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
			const char* o = argv[++i];
			do_audio = strcmp(o, "audio") == 0;
			do_tex = strcmp(o, "textures") == 0;
			if(!do_audio && !do_tex) {
				fprintf(stderr, "audio_cache_tool: unknown --only '%s'\n", o);
				return 1;
			}
		} else {
			fprintf(stderr, "audio_cache_tool: unknown argument '%s'\n", argv[i]);
			return 1;
		}
	}

	if(manifest) {
		if(strcmp(pack, "assets") == 0) {
			fprintf(stderr, "audio_cache_tool: the bundled root never takes a "
							"manifest\n");
			return 1;
		}
		struct pack_manifest m;
		if(!pack_manifest_write(pack, &m)) {
			fprintf(stderr, "audio_cache_tool: could not write the manifest "
							"of \"%s\"\n",
					pack);
			return 4;
		}
		printf("manifest of \"%s\": %u files, digest %016llx\n", pack,
			   (unsigned)m.files, (unsigned long long)m.digest);
	}

	gstate.config_user.root = json_value_init_object();
	set("paths.texturepack", pack);
	int rc = 0;
	if(do_tex)
		rc = textures(pack, wii, pc, bench);
	if(!do_audio)
		return rc;
	set("pack.cache", "false"); /* the build scan is a plain fresh decode */
	ptime_t t0 = time_get();
	audio_init(false, 0); /* headless: registry + arena, no device */
	double scan_ms = (double)time_diff_s(t0, time_get()) * 1000.0;
	size_t used = audio_arena_used();
	if(!audio_arena_total()) {
		fprintf(stderr, "audio_cache_tool: no PCM arena\n");
		return 2;
	}
	printf("fresh decode of \"%s\": %.1f ms, arena %zu/%zu bytes\n", pack,
		   scan_ms, used, audio_arena_total());

	/* Fable LOW-4: stb_vorbis is float IMDCT -- a PC decode is not proven
	   bit-identical to Broadway's, so the host tool never publishes the Wii
	   AUDIO target (the console publishes its own on its first miss). The
	   Wii TEXTURE target stays: integer mips + the verbatim GX encoders. */
	if(wii)
		printf("audio cache: wii target not published from the host tool "
			   "(float decode not proven identical; the console builds its "
			   "own)\n");
	const struct {
		bool want;
		uint32_t target;
		const char* name;
	} targets[] = {{pc, AUDIO_CACHE_TARGET_PC_LE, "pc"}};
	const char* bypass = audio_registry_cache_bypass_reason();
	if(bypass) {
		/* Not an error: the measured policy does not cache this root. */
		printf("audio cache: bypassed -- %s\n", bypass);
		return rc;
	}
	for(size_t k = 0; k < sizeof(targets) / sizeof(targets[0]); k++) {
		if(!targets[k].want)
			continue;
		char dir[320] = "";
		ptime_t tw = time_get();
		bool ok = audio_registry_cache_publish_target(targets[k].target, dir,
													  sizeof(dir));
		printf("%s cache -> %s: %s (%.1f ms)\n", targets[k].name,
			   dir[0] ? dir : "?", ok ? "published" : "REFUSED",
			   (double)time_diff_s(tw, time_get()) * 1000.0);
		if(!ok)
			rc = 3;
	}

	if(bench > 0 && rc == 0) {
		/* PC host bench: fresh decodes vs cache hits of the same tree. */
		double fresh = 0, hit = 0;
		set("pack.cache", "false");
		for(int i = 0; i < bench; i++)
			fresh += reload_ms();
		set("pack.cache", "true");
		(void)reload_ms(); /* the PC generation exists (--target pc/both) */
		bool all_hit = true;
		for(int i = 0; i < bench; i++) {
			hit += reload_ms();
			all_hit = all_hit
				&& strcmp(audio_registry_cache_status(), "hit") == 0;
		}
		printf("bench x%d: fresh decode %.2f ms/load, cache %s %.2f ms/load\n",
			   bench, fresh / bench, all_hit ? "hit" : "NOT ALL HITS",
			   hit / bench);
	}
	return rc;
}

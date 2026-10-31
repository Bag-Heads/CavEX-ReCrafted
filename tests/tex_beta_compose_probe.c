/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Driver for tests/test_tex_beta_compose.py: runs the production raw-jar
 * compositions (source/platform/tex_beta_compose.c) on a fixture folder and
 * writes the raw 256x256 RGBA result for a byte compare against the Pack
 * Helper's output.
 *
 *   probe gui2 <pack> <base_root> <out.rgba>
 *   probe anim <pack> <base_anim.png> <out.rgba>
 *   probe rawjar <pack>      prints 1/0 (loader rule == pack-list rule)
 *
 * Exit 0 ok, 2 composition refused (NULL), 1 usage/IO/disagreement. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/tex_beta_compose.h"
#include "texturepacks.h"

static int write_out(const char* path, uint8_t* img) {
	if(!img)
		return 2;
	FILE* f = fopen(path, "wb");
	size_t bytes = (size_t)TEX_BETA_CANON * TEX_BETA_CANON * 4;
	int rc = f && fwrite(img, 1, bytes, f) == bytes ? 0 : 1;
	if(f && fclose(f) != 0)
		rc = 1;
	free(img);
	return rc;
}

int main(int argc, char** argv) {
	if(argc == 3 && strcmp(argv[1], "rawjar") == 0) {
		bool loader = tex_beta_raw_jar_layout(argv[2]);
		bool list = texturepacks_is_raw_jar(argv[2]);
		if(loader != list) {
			fprintf(stderr, "loader rule %d != pack-list rule %d\n", loader,
					list);
			return 1;
		}
		printf("%d\n", loader ? 1 : 0);
		return 0;
	}
	if(argc != 5)
		return 1;
	if(strcmp(argv[1], "gui2") == 0)
		return write_out(argv[4], tex_beta_compose_gui2(argv[2], argv[3], NULL));
	if(strcmp(argv[1], "anim") == 0)
		return write_out(argv[4], tex_beta_compose_anim(argv[2], argv[3], NULL));
	return 1;
}

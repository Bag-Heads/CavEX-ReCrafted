/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* The ONE raw-jar layout rule, header-only so the texture loader
 * (tex_beta_compose.c) and the pack list (texturepacks.c) share it without
 * adding a link dependency to either's test targets. See tex_beta_compose.h. */

#ifndef TEX_BETA_LAYOUT_H
#define TEX_BETA_LAYOUT_H

#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

/* Does root/name exist as a regular file? */
static inline bool tex_beta_file_exists(const char* root, const char* name) {
	if(!root || !name)
		return false;
	char path[512];
	int n = snprintf(path, sizeof(path), "%s/%s", root, name);
	if(n < 0 || (size_t)n >= sizeof(path))
		return false;
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Is `root` (an already-resolved pack directory) an unconverted Beta jar
 * layout? The signature is the jar's own chest name: gui/container.png present
 * and gui/chest.png absent. Every helper-built pack carries gui/chest.png and
 * no bundled or optional pack (Isabella II) ships container.png, so those
 * packs never take the raw-jar path. */
static inline bool tex_beta_raw_jar_layout(const char* root) {
	return tex_beta_file_exists(root, "gui/container.png")
		&& !tex_beta_file_exists(root, "gui/chest.png");
}

#endif

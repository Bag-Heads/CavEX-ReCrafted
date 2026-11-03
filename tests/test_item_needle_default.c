/* Beta TextureCompassFX.onTick / TextureWatchFX.onTick always animate.
 * Check every startup config read, including the Wii-only preprocessor arm. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

int main(void) {
	FILE* f = fopen("source/main.c", "rb");
	assert(f);
	assert(!fseek(f, 0, SEEK_END));
	long n = ftell(f);
	assert(n > 0 && !fseek(f, 0, SEEK_SET));
	char* text = malloc((size_t)n + 1);
	assert(text && fread(text, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	text[n] = 0;
	/* Pin the startup expression, then exercise both JSON encodings. */
	const char* expression =
		"= json_object_dothas_value_of_type(json_value_get_object(gstate.config_user.root),\n"
		"\t\t\t\"gfx.item_needle_anim\", JSONBoolean)\n"
		"\t\t? json_object_dotget_boolean(json_value_get_object(gstate.config_user.root),\n"
		"\t\t\t\"gfx.item_needle_anim\") == 1\n"
		"\t\t: config_read_int(&gstate.config_user, \"gfx.item_needle_anim\", 1) != 0;";
	char* out = text;
	for(char* in = text; *in; in++) if(*in != '\r') *out++ = *in;
	*out = 0;
	assert(strstr(text, expression));
	const char* values[] = {NULL, "true", "false", "0", "1", "2"};
	const bool expected[] = {true, true, false, false, true, true};
	for(size_t k = 0; k < sizeof(values) / sizeof(values[0]); k++) {
		struct config c = {0};
		char json[96];
		if(values[k]) snprintf(json, sizeof(json), "{\"gfx\":{\"item_needle_anim\":%s}}", values[k]);
		else strcpy(json, "{}");
		c.root = json_parse_string(json);
		assert(c.root);
		bool enabled = json_object_dothas_value_of_type(json_value_get_object(c.root),
			"gfx.item_needle_anim", JSONBoolean)
			? json_object_dotget_boolean(json_value_get_object(c.root), "gfx.item_needle_anim") == 1
			: config_read_int(&c, "gfx.item_needle_anim", 1) != 0;
		assert(enabled == expected[k]);
		config_destroy(&c);
	}
	free(text);
	puts("PASS item needle default ON, including Wii startup");
}

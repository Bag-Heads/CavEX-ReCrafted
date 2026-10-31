#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "support/test_fixture_dir.h"

#include "network/complex_block_archive.h"
#include "network/server_local_state.h" // full server state (split from server_local.h) -- MAX_SIGNS, struct complex_block_pos by value

int main(void) {
	char dir_buf[128];
	const char* dir = test_fixture_dir("test_sign_archive_tmp", dir_buf, sizeof dir_buf);
#ifdef _WIN32
	(void)mkdir(dir);
#else
	(void)mkdir(dir, 0777);
#endif

	struct complex_block_pos pos[MAX_SIGNS];
	struct complex_block_pos expected_pos[MAX_SIGNS];
	char texts[MAX_SIGNS * SIGN_SIZE];
	char expected_texts[MAX_SIGNS * SIGN_SIZE];

	for(int i = 0; i < MAX_SIGNS; i++) {
		pos[i] = (struct complex_block_pos) {
			.x = i * 3 + 1,
			.y = i % WORLD_HEIGHT,
			.z = -i * 5 - 2,
		};

		for(int j = 0; j < SIGN_SIZE; j++)
			texts[i * SIGN_SIZE + j] = (char)('A' + ((i + j) % 26));
	}

	memcpy(expected_pos, pos, sizeof(pos));
	memcpy(expected_texts, texts, sizeof(texts));

	string_t path;
	string_init_set_str(path, dir);
	assert(sign_archive_write(pos, texts, path, WORLD_DIM_OVERWORLD));

	memset(pos, 0, sizeof(pos));
	memset(texts, 0, sizeof(texts));

	sign_archive_read(pos, texts, path, WORLD_DIM_OVERWORLD);
	string_clear(path);

	assert(memcmp(pos, expected_pos, sizeof(pos)) == 0);
	assert(memcmp(texts, expected_texts, sizeof(texts)) == 0);

	(void)remove(test_fixture_join(dir, "/signs.dat"));
	test_fixture_dir_cleanup(dir);
	return 0;
}

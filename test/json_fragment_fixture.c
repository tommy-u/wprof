// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
#define __unused __attribute__((unused))
#define BUG(...) do { fprintf(stderr, __VA_ARGS__); abort(); } while (0)
#include "../src/json.h"

int main(int argc, char **argv)
{
	bool serial = argc == 2 && !strcmp(argv[1], "--serial");
	struct json_state js = JSON_STATE_INIT(stdout);
	struct json_state encoded;

	if (!serial)
		json_init(&js);
	json_frag_start(&encoded);
	json_obj_start(&encoded);
	json_kv_str(&encoded, "comm", "é € 😀");
	json_kv_int(&encoded, "pid", 7);
	json_obj_end(&encoded);
	struct sbuf fragment = json_frag_end(&encoded);

	json_obj_start(&js);
	json_key(&js, "task");
	json_raw(&js, &fragment);
	memset(fragment.buf, 'x', fragment.len);
	sbuf_free(&fragment);
	char literal[] = "\"é € 😀\"";
	json_key(&js, "literal");
	json_fragment(&js, literal, sizeof(literal) - 1);
	memset(literal, 'x', sizeof(literal) - 1);
	json_obj_end(&js);

	size_t n = 1024 * 1024 + 4;
	char *large = malloc(n + 2);
	if (!large)
		return 1;
	large[0] = '"';
	for (size_t i = 0; i < n; i += 4)
		memcpy(large + 1 + i, "😀", 4);
	large[n + 1] = '"';
	json_obj_start(&js);
	json_key(&js, "large");
	json_fragment(&js, large, n + 2);
	memset(large, 'x', n + 2);
	free(large);
	json_obj_end(&js);
	json_finish(&js);
	return fflush(stdout) || ferror(stdout) ? 1 : 0;
}

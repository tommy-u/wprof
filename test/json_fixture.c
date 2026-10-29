// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/* Copyright (c) 2026 Meta Platforms, Inc. */
#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
#define __unused __attribute__((unused))
#define BUG(...) abort()

#ifdef JSON_REFERENCE
#include "json_reference.h"
#else
#include "../src/json.h"
#endif

int main(int argc, char **argv)
{
	const char *features[] = { "json-no-batching", "json-no-int-format", "json-no-int-ts", "json-no-parallel" };
	const char *optimizations[] = { "buffering", "integers", "timestamps", "parallel", "key-tokens", "literal-lengths", "task-tokens", "reuse", "worker-emitters" };
	const size_t debug_optimization[] = { 0, 1, 2, 3 };
	bool disabled[9] = { false };
	bool empty = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--empty") == 0) {
			empty = true;
		} else if ((strcmp(argv[i], "--json-opt") == 0 || strcmp(argv[i], "--no-json-opt") == 0) && i + 1 < argc) {
			bool disable = strcmp(argv[i], "--no-json-opt") == 0;
			const char *name = argv[++i];
			size_t optimization = 0;

			if (strcmp(name, "all") == 0) {
				for (size_t j = 0; j < 9; j++)
					disabled[j] = disable;
				continue;
			}
			while (optimization < 9 && strcmp(name, optimizations[optimization]) != 0)
				optimization++;
			if (optimization == 9)
				return 2;
			disabled[optimization] = disable;
		} else if (strcmp(argv[i], "--debug") == 0 && i + 1 < argc) {
			const char *name = argv[++i];
			size_t feature = 0;

			while (feature < 4 && strcmp(name, features[feature]) != 0)
				feature++;
			if (feature == 4)
				return 2;
			disabled[debug_optimization[feature]] = true;
		} else {
			return 2;
		}
	}
	struct json_state js = JSON_STATE_INIT(stdout);
#ifndef JSON_REFERENCE
	js.out.options = (struct json_options) {
		.no_batching = disabled[0],
		.no_int_format = disabled[1],
		.no_int_ts = disabled[2],
		.no_key_tokens = disabled[4],
		.no_literal_lengths = disabled[5],
		.no_task_tokens = disabled[6],
	};
	js.no_parallel = disabled[3];
	js.no_reuse = disabled[7];
	js.no_worker_emitters = disabled[8];
	json_init(&js);
#else
	(void)disabled;
#endif

	if (!empty) {
		char reused[32] = "original";
		char reused_key[32] = "original-key";
		char controls[32];

		for (int i = 1; i < 32; i++)
			controls[i - 1] = i;
		controls[31] = 0;
		json_obj_start(&js);
		json_kv_str(&js, "text", "quote\" slash\\\n\t\r\b\f\001\037 é € 😀 \200\300\257\355\240\200\364\220\200\200\342\202");
		json_kv_str(&js, "controls", controls);
		json_kv_str(&js, "utf8_boundaries", "\302\200\337\277\340\240\200\355\237\277\357\277\277\360\220\200\200\364\217\277\277");
		json_kv_str(&js, "malformed_utf8", "\301\277\340\237\277\360\217\277\277\365\200\200\200\377");
		json_kv_str(&js, "empty", "");
		json_kv_str(&js, "", "empty key");
		json_kv_str(&js, "key\"\\\n€", "escaped key");
		json_kv_str(&js, "reused", reused);
		json_kv_str(&js, reused_key, "stack key");
		json_kv_fmt(&js, "formatted", "%s/%d/%c", reused, 7, '\n');
		json_kv_fmt(&js, "long_format", "%0300d", 7);
		strcpy(reused, "overwritten");
		strcpy(reused_key, "overwritten-key");
		json_kv_int(&js, "signed", INT64_MIN);
		json_kv_int(&js, "signed_max", INT64_MAX);
		json_kv_uint(&js, "unsigned", UINT64_MAX);
		json_kv_uint(&js, "unsigned_zero", 0);
		json_kv_int(&js, "signed_min_next", INT64_MIN + 1);
		json_kv_int(&js, "signed_max_prev", INT64_MAX - 1);
		json_kv_uint(&js, "unsigned_max_prev", UINT64_MAX - 1);
		json_subarr_start(&js, "decimal_boundaries");
		for (unsigned long long power = 1; ; power *= 10) {
			for (int offset = 0; offset < 3; offset++) {
				unsigned long long value = power - 1 + offset;

				json_arr_elem(&js);
				json_obj_start(&js);
				json_kv_uint(&js, "unsigned", value);
				if (value <= INT64_MAX) {
					json_kv_int(&js, "positive", value);
					json_kv_int(&js, "negative", -(long long)value);
					json_subarr_start(&js, "signed_array");
					json_arr_int(&js, value);
					json_arr_int(&js, -(long long)value);
					json_arr_end(&js);
				}
				json_obj_end(&js);
			}
			if (power == 10000000000000000000ULL)
				break;
		}
		json_arr_end(&js);
		json_subobj_start(&js, "timestamps");
		json_kv_ts(&js, "zero", 0);
		json_kv_ts(&js, "one_ns", 1);
		json_kv_ts(&js, "second_before", 999999999ULL);
		json_kv_ts(&js, "second", 1000000000ULL);
		json_kv_ts(&js, "second_after", 1000000001ULL);
		json_kv_ts(&js, "guard_before", (1ULL << 22) * 1000000000ULL - 1);
		json_kv_ts(&js, "guard", (1ULL << 22) * 1000000000ULL);
		json_kv_ts(&js, "guard_after", (1ULL << 22) * 1000000000ULL + 1);
		json_kv_ts(&js, "seconds_2p23_before", (1ULL << 23) * 1000000000ULL - 1);
		json_kv_ts(&js, "seconds_2p23", (1ULL << 23) * 1000000000ULL);
		json_kv_ts(&js, "seconds_2p23_after", (1ULL << 23) * 1000000000ULL + 1);
		json_kv_ts(&js, "ns_2p53_before", (1ULL << 53) - 1);
		json_kv_ts(&js, "ns_2p53", 1ULL << 53);
		json_kv_ts(&js, "ns_2p53_after", (1ULL << 53) + 1);
		json_kv_ts(&js, "maximum_before", UINT64_MAX - 1);
		json_kv_ts(&js, "maximum", UINT64_MAX);
		json_kv_ts(&js, "wrapped_minus_one", (u64)-1);
		json_kv_ts(&js, "wrapped_before_second", (u64)-999999999LL);
		json_kv_ts(&js, "wrapped_second", (u64)-1000000000LL);
		json_kv_ts(&js, "wrapped_after_second", (u64)-1000000001LL);
		json_kv_ts(&js, "wrapped_signed_min", (u64)INT64_MIN);
		json_obj_end(&js);
		json_kv_float(&js, "zero6", "%.6lf", -0.0);
		json_kv_float(&js, "zero9", "%.9lf", -0.0);
		json_kv_float(&js, "positive_zero6", "%.6lf", 0.0);
		json_kv_float(&js, "positive_zero9", "%.9lf", 0.0);
		json_kv_float(&js, "largest6", "%.6lf", DBL_MAX);
		json_kv_float(&js, "largest9", "%.9lf", DBL_MAX);
		json_kv_float(&js, "smallest6", "%.6lf", DBL_MIN);
		json_kv_float(&js, "smallest9", "%.9lf", DBL_MIN);
		json_kv_float(&js, "rounding6", "%.6lf", 1.2345675);
		json_kv_float(&js, "rounding9", "%.9lf", 0.0000000016);
		json_kv_float(&js, "negative6", "%.6lf", -1.2345675);
		json_kv_float(&js, "negative9", "%.9lf", -0.0000000016);
		json_kv_float(&js, "other_format", "%.3lf", 1.2345);
		json_subarr_start(&js, "frames");
		for (int i = 0; i < 4; i++) {
			snprintf(reused, sizeof(reused), "frame_%d", i);
			json_arr_str(&js, reused);
		}
		json_arr_fmt(&js, "%s/%d/%c", reused, 9, '\n');
		strcpy(reused, "overwritten again");
		json_arr_null(&js);
		json_arr_end(&js);
		json_subobj_start(&js, "nested");
		json_kv_null(&js, "missing");
		json_kv_bool(&js, "enabled", true);
		json_kv_bool(&js, "disabled", false);
		json_subarr_start(&js, "tree");
		json_arr_elem(&js);
		json_obj_start(&js);
		json_subarr_start(&js, "leaves");
		json_arr_int(&js, -7);
		json_arr_float(&js, "%.6lf", 1.25);
		json_arr_null(&js);
		json_arr_end(&js);
		json_obj_end(&js);
		json_arr_end(&js);
		json_obj_end(&js);
		json_subobj_start(&js, "empty_object");
		json_obj_end(&js);
		json_subarr_start(&js, "empty_array");
		json_arr_end(&js);
		json_obj_end(&js);

		for (int i = 0; i < 12000; i++) {
			json_obj_start(&js);
#ifdef JSON_REFERENCE
			json_kv_int(&js, "id", i);
			json_kv_ts(&js, "ts", 123456789ULL + i * 17ULL);
#else
			json_kv_int_id(&js, JSON_KEY_ID, i);
			json_kv_ts_id(&js, JSON_KEY_TS, 123456789ULL + i * 17ULL);
#endif
			json_subarr_start(&js, "values");
			json_arr_int(&js, -i);
			json_arr_float(&js, "%.6lf", i / 7.0);
			json_arr_end(&js);
#ifdef JSON_REFERENCE
			json_kv_fmt(&js, "name", "task-%d", i);
#else
			json_kv_fmt_id(&js, JSON_KEY_NAME, "task-%d", i);
#endif
			json_obj_end(&js);
		}

		size_t large_len = 1024 * 1024 + 4;
		char *large = malloc(large_len + 1);

		if (!large)
			return 1;
		for (size_t i = 0; i < large_len; i += 4)
			memcpy(large + i, "😀", 4);
		large[large_len] = 0;
		json_obj_start(&js);
		json_kv_fmt(&js, "large", "%s", large);
		json_kv_str(&js, "after_large", "tail");
		memset(large, 'A', large_len);
		json_kv_str(&js, "large_ascii", large);
		json_kv_str(&js, "after_ascii", "tail");
		memset(large, 'k', large_len);
		json_kv_str(&js, large, "large key");
		json_kv_str(&js, "after_key", "tail");
		for (size_t i = 0; i < large_len; i += 4)
			memcpy(large + i, "\"\\\n\001", 4);
		json_kv_str(&js, "large_escaped", large);
		memset(large, 'x', large_len);
		free(large);
		json_kv_str(&js, "after_escaped", "tail");
		json_obj_end(&js);
	}

#ifndef JSON_REFERENCE
	json_finish(&js);
#endif
	return fflush(stdout) || ferror(stdout) ? 1 : 0;
}

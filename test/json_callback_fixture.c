// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/* Copyright (c) 2026 Meta Platforms, Inc. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef uint64_t u64;
typedef uint32_t u32;
#define __unused __attribute__((unused))
#define BUG(...) abort()
#include "../src/json.h"

struct callback_context {
	pthread_t caller;
	bool worker;
};

static void render_timestamp(void *ctx, const void *event, const void *snapshot, struct json_buf *out)
{
	struct callback_context *context = ctx;
	u64 ns;

	(void)snapshot;
	if (!!pthread_equal(context->caller, pthread_self()) == context->worker)
		abort();
	memcpy(&ns, event, sizeof(ns));
	struct json_state state = JSON_STATE_INIT(NULL);

	state.out = *out;
	json_obj_start(&state);
	json_kv_ts(&state, "ordinary", ns);
	json_kv_ts_id(&state, JSON_KEY_TS, ns);
	json_obj_end(&state);
	*out = state.out;
}

int main(int argc, char **argv)
{
	struct json_state state = JSON_STATE_INIT(stdout);
	struct callback_context context = { .caller = pthread_self() };
	const u64 timestamps[] = { UINT64_MAX, (1ULL << 23) * 1000000000ULL + 1, 999999999ULL };

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--serial") == 0)
			state.no_parallel = true;
		else if (strcmp(argv[i], "--legacy") == 0)
			state.out.options.no_int_ts = true;
		else
			return 2;
	}
	state.emit_ctx = &context;
	json_init(&state);
	context.worker = state.pool != NULL;
	if (!state.no_parallel && !state.pool)
		abort();
	for (size_t i = 0; i < sizeof(timestamps) / sizeof(timestamps[0]); i++) {
		if (state.pool)
			json_emit_reserve(&state, render_timestamp, &timestamps[i], 0);
		else
			render_timestamp(&context, &timestamps[i], NULL, &state.out);
	}
	json_finish(&state);
	return fflush(stdout) || ferror(stdout) ? 1 : 0;
}

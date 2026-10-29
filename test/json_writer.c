// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/* Copyright (c) 2026 Meta Platforms, Inc. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
#define __unused __attribute__((unused))
#define BUG(...) abort()
#define JSON_BUF_SZ 64

static int observed_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
static int observed_join(pthread_t thread, void **result);
#define pthread_cond_wait observed_cond_wait
#define pthread_join observed_join
#include "../src/json.h"
#undef pthread_cond_wait
#undef pthread_join

#define RECORD(id) "{\"id\":" #id "}\n"
static const char *records[] = {
	RECORD(0), RECORD(1), RECORD(2), RECORD(3), RECORD(4), RECORD(5), RECORD(6), RECORD(7),
	RECORD(8), RECORD(9), RECORD(10), RECORD(11), RECORD(12), RECORD(13), RECORD(14), RECORD(15), RECORD(16),
	RECORD(17), RECORD(18), RECORD(19), RECORD(20), RECORD(21), RECORD(22), RECORD(23), RECORD(24),
};
#undef RECORD

struct fixture {
	pthread_mutex_t mutex;
	pthread_cond_t changed;
	struct json_state js;
	pthread_t caller;
	bool writer_writes;
	bool gate_first;
	bool entered;
	bool in_write;
	bool release;
	bool waiting;
	bool joining;
	bool done;
	bool skipped;
	bool fail_writer_start;
	int writer_attempts;
	int count;
	int submitted;
	int ids[25];
	uintptr_t jobs[25];
	uintptr_t buffers[25];
	uintptr_t blocked_buffer;
	char output[512];
	size_t len;
};

static struct fixture *active;

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
	if (start == json_writer_main) {
		active->writer_attempts++;
		if (active->fail_writer_start) {
			errno = EPERM;
			return EAGAIN;
		}
	}
	return __real_pthread_create(thread, attr, start, arg);
}

static int observed_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	if (cond != &active->js.writer.space)
		return pthread_cond_wait(cond, mutex);
	/* The controller takes writer.mutex after this notification to observe the actual space wait. */
	pthread_mutex_lock(&active->mutex);
	active->waiting = true;
	pthread_cond_broadcast(&active->changed);
	pthread_mutex_unlock(&active->mutex);
	int err = pthread_cond_wait(cond, mutex);

	pthread_mutex_lock(&active->mutex);
	active->waiting = false;
	pthread_cond_broadcast(&active->changed);
	pthread_mutex_unlock(&active->mutex);
	return err;
}

static int observed_join(pthread_t thread, void **result)
{
	assert(pthread_equal(thread, active->js.writer.thread));
	pthread_mutex_lock(&active->mutex);
	active->joining = true;
	pthread_cond_broadcast(&active->changed);
	pthread_mutex_unlock(&active->mutex);
	return pthread_join(thread, result);
}

static ssize_t sink_write(void *cookie, const char *data, size_t len)
{
	struct fixture *f = cookie;

	pthread_mutex_lock(&f->mutex);
	assert(!!pthread_equal(pthread_self(), f->caller) == !f->writer_writes);
	if (f->writer_writes) {
		char name[16];

		assert(pthread_equal(pthread_self(), f->js.writer.thread));
		assert(!prctl(PR_GET_NAME, name, 0, 0, 0));
		assert(strcmp(name, "wprof_json_io") == 0);
	}
	if (f->gate_first && !f->entered) {
		char before[64];

		assert(len <= sizeof(before));
		memcpy(before, data, len);
		f->entered = true;
		f->in_write = true;
		assert(f->buffers[0]);
		f->blocked_buffer = f->buffers[0];
		pthread_cond_broadcast(&f->changed);
		while (!f->release)
			pthread_cond_wait(&f->changed, &f->mutex);
		assert(memcmp(data, before, len) == 0);
		f->in_write = false;
	}
	assert(f->len + len <= sizeof(f->output));
	memcpy(f->output + f->len, data, len);
	f->len += len;
	pthread_mutex_unlock(&f->mutex);
	return len;
}

static void emit_record(void *ctx, const void *event, const void *snapshot __unused, struct json_buf *out)
{
	struct fixture *f = ctx;
	int id = *(const int *)event;

	pthread_mutex_lock(&f->mutex);
	assert(!f->in_write || (uintptr_t)out->data != f->blocked_buffer);
	json_write(out, records[id], strlen(records[id]));
	f->jobs[id] = (uintptr_t)((char *)out - offsetof(struct json_job, out));
	f->buffers[id] = (uintptr_t)out->data;
	assert(!f->in_write || f->buffers[id] != f->blocked_buffer);
	pthread_mutex_unlock(&f->mutex);
}

static void *produce(void *ctx)
{
	struct fixture *f = ctx;

	f->caller = pthread_self();
	json_init(&f->js);
	if (!f->js.pool) {
		json_finish(&f->js);
		pthread_mutex_lock(&f->mutex);
		f->skipped = true;
		f->done = true;
		pthread_cond_broadcast(&f->changed);
		pthread_mutex_unlock(&f->mutex);
		return NULL;
	}
	assert(f->js.writer.started);
	for (int i = 0; i < f->count; i++) {
		json_emit_reserve(&f->js, emit_record, &f->ids[i], 0);
		if (i + 1 < f->count) {
			json_submit(&f->js);
			pthread_mutex_lock(&f->mutex);
			f->submitted++;
			pthread_mutex_unlock(&f->mutex);
		}
	}
	json_finish(&f->js);
	pthread_mutex_lock(&f->mutex);
	f->done = true;
	pthread_cond_broadcast(&f->changed);
	pthread_mutex_unlock(&f->mutex);
	return NULL;
}

static int gated_run(struct fixture *f, bool backpressure)
{
	pthread_t producer;

	f->count = backpressure ? 25 : 2;
	f->gate_first = true;
	f->writer_writes = true;
	assert(!pthread_create(&producer, NULL, produce, f));
	pthread_mutex_lock(&f->mutex);
	while (!f->done && (!f->entered || !(backpressure ? f->waiting : f->joining)))
		pthread_cond_wait(&f->changed, &f->mutex);
	bool skipped = f->skipped;
	uintptr_t blocked = f->blocked_buffer;

	if (!skipped) {
		assert(f->entered && f->in_write && !f->done);
		if (backpressure)
			assert(f->waiting && f->submitted < f->count - 1);
	}
	pthread_mutex_unlock(&f->mutex);
	if (!skipped) {
		pthread_mutex_lock(&f->js.writer.mutex);
		assert(f->js.writer.count == (backpressure ? 2 : 1));
		if (!backpressure)
			assert(f->js.writer.closing);
		for (struct json_job *job = f->js.free_jobs; job; job = job->next)
			assert((uintptr_t)job->out.data != blocked);
		for (size_t i = 0; i < f->js.writer.count; i++) {
			struct json_job *job = f->js.writer.queue[(f->js.writer.head + i) % 2];

			assert((uintptr_t)job->out.data != blocked);
		}
		pthread_mutex_unlock(&f->js.writer.mutex);
	}
	pthread_mutex_lock(&f->mutex);
	f->release = true;
	pthread_cond_broadcast(&f->changed);
	pthread_mutex_unlock(&f->mutex);
	assert(!pthread_join(producer, NULL));
	if (skipped)
		return 77;
	if (backpressure) {
		int distinct = 0;

		for (int i = 0; i < f->count; i++) {
			bool seen = false;

			assert(f->jobs[i]);
			for (int j = 0; j < i; j++)
				seen |= f->jobs[i] == f->jobs[j];
			distinct += !seen;
		}
		assert(distinct <= 20);
		assert(distinct < f->count);
	}
	return 0;
}

static int simple_run(struct fixture *f, const char *mode)
{
	f->caller = pthread_self();
	f->count = strcmp(mode, "empty") == 0 ? 0 : 25;
	f->js.no_writer = strcmp(mode, "no-writer") == 0;
	f->js.no_parallel = strcmp(mode, "serial") == 0;
	f->js.out.options.no_batching = strcmp(mode, "unbatched") == 0;
	f->fail_writer_start = strcmp(mode, "start-failure") == 0;
	json_init(&f->js);
	if ((f->fail_writer_start || f->js.no_writer) && !f->js.pool) {
		json_finish(&f->js);
		return 77;
	}
	if (f->fail_writer_start)
		assert(f->writer_attempts == 1 && !f->js.writer.started);
	else if (f->js.no_writer || f->js.no_parallel || f->js.out.options.no_batching)
		assert(!f->writer_attempts && !f->js.writer.started);
	else
		assert(f->js.writer.started == !!f->js.pool);
	f->writer_writes = f->js.writer.started;
	for (int i = 0; i < f->count; i++) {
		json_obj_start(&f->js);
		json_kv_int(&f->js, "id", i);
		json_obj_end(&f->js);
	}
	json_finish(&f->js);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	struct fixture f = { .mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER };
	FILE *out = fopencookie(&f, "w", (cookie_io_functions_t) { .write = sink_write });

	assert(out);
	assert(!setvbuf(out, NULL, _IONBF, 0));
	f.js = (struct json_state)JSON_STATE_INIT(out);
	f.js.emit_ctx = &f;
	active = &f;
	for (int i = 0; i < 25; i++)
		f.ids[i] = i;
	int err;

	if (strcmp(argv[1], "pipeline") == 0 || strcmp(argv[1], "partial") == 0)
		err = gated_run(&f, strcmp(argv[1], "pipeline") == 0);
	else
		err = simple_run(&f, argv[1]);
	assert(!fflush(out));
	assert(!ferror(out));
	assert(!fclose(out));
	if (err == 77) {
		puts("Parallel JSON tests need at least two available CPUs");
		return err;
	}
	size_t pos = 0;

	for (int i = 0; i < f.count; i++) {
		size_t len = strlen(records[i]);

		assert(pos + len <= f.len);
		assert(memcmp(f.output + pos, records[i], len) == 0);
		pos += len;
	}
	assert(pos == f.len);
	puts("Verified JSON writer output and lifecycle");
	return 0;
}

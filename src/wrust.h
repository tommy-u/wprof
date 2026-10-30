/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/* Copyright (c) 2025 Meta Platforms, Inc. */
#ifndef __WRUST_H_
#define __WRUST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * libwrust: C interface to Rust standard-library data structures and helpers
 * (implemented in src/wrust/). Each data structure gets its own section below;
 * add new ones here as more Rust-backed interfaces are introduced.
 */

/* ==================== PRIORITY QUEUE ====================
 * Min priority queue over (key, value) pairs, backed by std BinaryHeap.
 * Ordered ascending and lexicographically on (key, value), so value breaks
 * ties on key.
 */
struct wpq;

struct wpq *wpq_new(size_t cap);
void wpq_free(struct wpq *pq);
void wpq_push(struct wpq *pq, uint64_t key, uint32_t val);
bool wpq_empty(const struct wpq *pq);
void wpq_peek(const struct wpq *pq, uint64_t *key, uint32_t *val);
void wpq_replace_min(struct wpq *pq, uint64_t key, uint32_t val);
void wpq_pop(struct wpq *pq);

/* ==================== SORT ====================
 * In-place stable sort of (timestamp, event pointer) pairs, ascending by ts. The
 * timestamp is kept inline so the sort never dereferences ptr.
 */
struct wrust_ts_ptr {
	uint64_t ts;
	const void *ptr;
};
void wrust_sort_events_by_ts(struct wrust_ts_ptr *items, size_t cnt);

/*
 * ==================== POINTER PRIORITY QUEUE ====================
 * Min priority queue over (key, pointer) pairs, backed by std BinaryHeap.
 * Sibling of wpq whose value is a pointer rather than a uint32_t; the pointer
 * is stored as usize internally. Ordered ascending on key, with the pointer
 * bits breaking ties deterministically. The queue does not own the pointed-to
 * memory: wppq_free() releases only the queue itself, so drain via wppq_pop()
 * first if the values need freeing.
 */
struct wppq;

struct wppq *wppq_new(size_t cap);
void wppq_free(struct wppq *pq);
void wppq_push(struct wppq *pq, uint64_t key, void *val);
bool wppq_empty(const struct wppq *pq);
size_t wppq_len(const struct wppq *pq);
uint64_t wppq_peek_key(const struct wppq *pq);  /* min key; requires non-empty */
void *wppq_pop(struct wppq *pq);                 /* min value, or NULL if empty */

/* ==================== ORDERED WORKER POOL ==================== */
struct wpool;
typedef void (*wpool_work_fn)(void *ctx, void *job);
typedef void (*wpool_done_fn)(void *ctx, void *job);

/* Uses min(available CPUs, max_workers); NULL selects serial work below two.
 * One caller submits/finishes. Each job transfers to work, then to done.
 * work runs on a worker with exclusive job access; shared ctx access must be
 * thread-safe. done runs on the caller in submission order and may free the
 * job. Keep ctx valid through finish. Callbacks must not reenter this pool.
 */
struct wpool *wpool_new(size_t max_workers, wpool_work_fn work, wpool_done_fn done, void *ctx);
/* Uses the same CPU selection, with a shared FIFO input queue. jobs_per_worker
 * must be nonzero. At most selected_workers * jobs_per_worker jobs are admitted,
 * including queued, running, completed, and currently executing done callbacks.
 * admission_bytes is a soft estimate: one oversize job is allowed when empty.
 * Workers never wait for an output allocation budget. done runs directly on one
 * dedicated wprof_json_io thread in submission order, then returns admission
 * credits. Shared ctx access from work/done must be thread-safe; keep ctx valid
 * through finish. One caller submits/finishes; callbacks must not reenter.
 */
struct wpool *wpool_new_windowed(size_t max_workers, size_t jobs_per_worker, size_t byte_limit,
			       wpool_work_fn work, wpool_done_fn done, void *ctx);
/* submit uses zero admission bytes; sized admission is ignored by legacy pools. */
void wpool_submit(struct wpool *pool, void *job);
void wpool_submit_sized(struct wpool *pool, void *job, size_t admission_bytes);
/* Closes, drains, joins and frees the pool before ctx may be freed. NULL is valid. */
void wpool_finish(struct wpool *pool);

#endif /* __WRUST_H_ */

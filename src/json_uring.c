#define _GNU_SOURCE
#include "json_uring.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#define JSON_HAVE_URING_HEADER 1
#endif
#endif

#if defined(JSON_HAVE_URING_HEADER) && defined(__NR_io_uring_setup) && defined(__NR_io_uring_enter)

enum json_uring_slot_state {
	JSON_URING_FREE,
	JSON_URING_FILLING,
	JSON_URING_SUBMITTED,
};

struct json_uring_slot {
	char *data;
	size_t len;
	size_t done;
	off_t offset;
	enum json_uring_slot_state state;
};

struct json_uring {
	int ring_fd;
	int output_fd;
	int error;
	bool broken;
	unsigned depth;
	unsigned pending;
	size_t chunk_size;
	size_t io_align;
	void *sq_map;
	void *cq_map;
	size_t sq_map_size;
	size_t cq_map_size;
	size_t sqes_size;
	unsigned *sq_head;
	unsigned *sq_tail;
	unsigned *sq_mask;
	unsigned *sq_array;
	unsigned *cq_head;
	unsigned *cq_tail;
	unsigned *cq_mask;
	struct io_uring_sqe *sqes;
	struct io_uring_cqe *cqes;
	struct json_uring_slot slots[8];
	struct json_uring_stats stats;
};

static int json_uring_error(struct json_uring *ring, int error)
{
	if (!ring->error)
		ring->error = error;
	return -ring->error;
}

static void json_uring_queue(struct json_uring *ring, unsigned id)
{
	struct json_uring_slot *slot = &ring->slots[id];
	unsigned tail = __atomic_load_n(ring->sq_tail, __ATOMIC_RELAXED);
	unsigned index = tail & *ring->sq_mask;
	struct io_uring_sqe *sqe = &ring->sqes[index];

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = IORING_OP_WRITE;
	sqe->fd = ring->output_fd;
	sqe->off = slot->offset + slot->done;
	sqe->addr = (uintptr_t)(slot->data + slot->done);
	sqe->len = slot->len - slot->done;
	sqe->user_data = id;
	ring->sq_array[index] = index;
	__atomic_store_n(ring->sq_tail, tail + 1, __ATOMIC_RELEASE);
	ring->stats.submitted++;
}

static int json_uring_enter_pending(struct json_uring *ring)
{
	for (;;) {
		unsigned head = __atomic_load_n(ring->sq_head, __ATOMIC_ACQUIRE);
		unsigned tail = __atomic_load_n(ring->sq_tail, __ATOMIC_RELAXED);
		unsigned count = tail - head;

		if (!count)
			return 0;
		int ret = syscall(__NR_io_uring_enter, ring->ring_fd, count, 0, 0, NULL, 0);

		/* A interrupted enter may have consumed SQEs: always reload the kernel head. */
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0 || (!ret && head == __atomic_load_n(ring->sq_head, __ATOMIC_ACQUIRE))) {
			ring->broken = true;
			return json_uring_error(ring, ret < 0 ? errno : EIO);
		}
	}
}

static void json_uring_reap(struct json_uring *ring)
{
	unsigned head = __atomic_load_n(ring->cq_head, __ATOMIC_RELAXED);
	unsigned tail = __atomic_load_n(ring->cq_tail, __ATOMIC_ACQUIRE);

	while (head != tail) {
		struct io_uring_cqe cqe = ring->cqes[head & *ring->cq_mask];
		unsigned id = cqe.user_data;
		struct json_uring_slot *slot = &ring->slots[id];

		head++;
		__atomic_store_n(ring->cq_head, head, __ATOMIC_RELEASE);
		ring->stats.completed++;
		if (cqe.res == -EINTR && !ring->error) {
			json_uring_queue(ring, id);
			continue;
		}
		if (cqe.res < 0) {
			json_uring_error(ring, -cqe.res);
		} else if (!cqe.res || cqe.res % ring->io_align || (size_t)cqe.res > slot->len - slot->done) {
			json_uring_error(ring, EIO);
		} else {
			slot->done += cqe.res;
			ring->stats.completed_bytes += cqe.res;
		}
		if (!ring->error && slot->done < slot->len) {
			json_uring_queue(ring, id);
		} else {
			slot->state = JSON_URING_FREE;
			ring->pending--;
		}
	}
}

static int json_uring_wait(struct json_uring *ring)
{
	int ret;

	do {
		ret = syscall(__NR_io_uring_enter, ring->ring_fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		ring->broken = true;
		return json_uring_error(ring, errno);
	}
	return 0;
}

int json_uring_create(struct json_uring **out, int fd, unsigned depth, size_t chunk_size,
		      size_t mem_align, size_t io_align)
{
	struct io_uring_params params = {};
	struct json_uring *ring;
	int error;

	*out = NULL;
	if (depth < 2 || depth > 8 || !io_align || !chunk_size || chunk_size > UINT_MAX ||
	    chunk_size % io_align || mem_align < sizeof(void *) || (mem_align & (mem_align - 1)) ||
	    mem_align % io_align)
		return -EINVAL;
	ring = calloc(1, sizeof(*ring));
	if (!ring)
		return -ENOMEM;
	ring->ring_fd = syscall(__NR_io_uring_setup, depth, &params);
	if (ring->ring_fd < 0) {
		error = errno;
		free(ring);
		return -error;
	}
	ring->output_fd = fd;
	ring->depth = depth;
	ring->chunk_size = chunk_size;
	ring->io_align = io_align;
	ring->sq_map_size = params.sq_off.array + params.sq_entries * sizeof(unsigned);
	ring->cq_map_size = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
	if (params.features & IORING_FEAT_SINGLE_MMAP) {
		if (ring->cq_map_size > ring->sq_map_size)
			ring->sq_map_size = ring->cq_map_size;
		ring->cq_map_size = ring->sq_map_size;
	}
	ring->sq_map = mmap(NULL, ring->sq_map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			    ring->ring_fd, IORING_OFF_SQ_RING);
	if (ring->sq_map == MAP_FAILED)
		goto fail;
	if (params.features & IORING_FEAT_SINGLE_MMAP)
		ring->cq_map = ring->sq_map;
	else
		ring->cq_map = mmap(NULL, ring->cq_map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
				    ring->ring_fd, IORING_OFF_CQ_RING);
	if (ring->cq_map == MAP_FAILED)
		goto fail;
	ring->sqes_size = params.sq_entries * sizeof(struct io_uring_sqe);
	ring->sqes = mmap(NULL, ring->sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			  ring->ring_fd, IORING_OFF_SQES);
	if (ring->sqes == MAP_FAILED)
		goto fail;
	ring->sq_head = (unsigned *)((char *)ring->sq_map + params.sq_off.head);
	ring->sq_tail = (unsigned *)((char *)ring->sq_map + params.sq_off.tail);
	ring->sq_mask = (unsigned *)((char *)ring->sq_map + params.sq_off.ring_mask);
	ring->sq_array = (unsigned *)((char *)ring->sq_map + params.sq_off.array);
	ring->cq_head = (unsigned *)((char *)ring->cq_map + params.cq_off.head);
	ring->cq_tail = (unsigned *)((char *)ring->cq_map + params.cq_off.tail);
	ring->cq_mask = (unsigned *)((char *)ring->cq_map + params.cq_off.ring_mask);
	ring->cqes = (struct io_uring_cqe *)((char *)ring->cq_map + params.cq_off.cqes);
	for (unsigned i = 0; i < depth; i++) {
		error = posix_memalign((void **)&ring->slots[i].data, mem_align, chunk_size);
		if (error) {
			errno = error;
			goto fail;
		}
	}
	*out = ring;
	return 0;

fail:
	error = errno;
	json_uring_destroy(ring);
	return -error;
}

int json_uring_acquire(struct json_uring *ring, void **data)
{
	for (;;) {
		json_uring_reap(ring);
		if (ring->error)
			return -ring->error;
		if (json_uring_enter_pending(ring))
			return -ring->error;
		for (unsigned i = 0; i < ring->depth; i++) {
			if (ring->slots[i].state != JSON_URING_FREE)
				continue;
			ring->slots[i].state = JSON_URING_FILLING;
			*data = ring->slots[i].data;
			return i;
		}
		if (json_uring_wait(ring))
			return -ring->error;
	}
}

int json_uring_submit(struct json_uring *ring, unsigned id, size_t len, off_t offset)
{
	if (ring->error)
		return -ring->error;
	if (!len || len > ring->chunk_size || len % ring->io_align || offset < 0 || offset % ring->io_align)
		return json_uring_error(ring, EINVAL);
	struct json_uring_slot *slot = &ring->slots[id];

	slot->len = len;
	slot->done = 0;
	slot->offset = offset;
	slot->state = JSON_URING_SUBMITTED;
	ring->pending++;
	if (ring->pending > ring->stats.peak_pending)
		ring->stats.peak_pending = ring->pending;
	json_uring_queue(ring, id);
	return json_uring_enter_pending(ring);
}

int json_uring_drain(struct json_uring *ring)
{
	while (ring->pending) {
		json_uring_reap(ring);
		if (!ring->pending || ring->broken)
			break;
		if (json_uring_enter_pending(ring) || json_uring_wait(ring))
			break;
	}
	return -ring->error;
}

bool json_uring_idle(struct json_uring *ring)
{
	return !ring->pending;
}

void json_uring_get_stats(struct json_uring *ring, struct json_uring_stats *out)
{
	*out = ring->stats;
}

void json_uring_destroy(struct json_uring *ring)
{
	if (!ring)
		return;
	if (ring->pending)
		json_uring_drain(ring);
	if (ring->sqes && ring->sqes != MAP_FAILED)
		munmap(ring->sqes, ring->sqes_size);
	if (ring->cq_map && ring->cq_map != MAP_FAILED && ring->cq_map != ring->sq_map)
		munmap(ring->cq_map, ring->cq_map_size);
	if (ring->sq_map && ring->sq_map != MAP_FAILED)
		munmap(ring->sq_map, ring->sq_map_size);
	close(ring->ring_fd);
	/* Failed ring syscalls cannot prove that the kernel has stopped using these buffers. */
	if (!ring->pending) {
		for (unsigned i = 0; i < ring->depth; i++)
			free(ring->slots[i].data);
	}
	free(ring);
}

#else

int json_uring_create(struct json_uring **out, int fd, unsigned depth, size_t chunk_size,
		      size_t mem_align, size_t io_align)
{
	(void)fd;
	(void)depth;
	(void)chunk_size;
	(void)mem_align;
	(void)io_align;
	*out = NULL;
	return -ENOSYS;
}

int json_uring_acquire(struct json_uring *ring, void **data)
{
	(void)ring;
	(void)data;
	return -ENOSYS;
}

int json_uring_submit(struct json_uring *ring, unsigned slot, size_t len, off_t offset)
{
	(void)ring;
	(void)slot;
	(void)len;
	(void)offset;
	return -ENOSYS;
}

int json_uring_drain(struct json_uring *ring)
{
	(void)ring;
	return -ENOSYS;
}

bool json_uring_idle(struct json_uring *ring)
{
	(void)ring;
	return true;
}

void json_uring_get_stats(struct json_uring *ring, struct json_uring_stats *out)
{
	(void)ring;
	memset(out, 0, sizeof(*out));
}

void json_uring_destroy(struct json_uring *ring)
{
	(void)ring;
}

#endif

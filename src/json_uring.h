#ifndef WPROF_JSON_URING_H
#define WPROF_JSON_URING_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct json_uring;

struct json_uring_stats {
	unsigned long long submitted;
	unsigned long long completed;
	unsigned long long completed_bytes;
	unsigned peak_pending;
};

int json_uring_create(struct json_uring **out, int fd, unsigned depth, size_t chunk_size,
		      size_t mem_align, size_t io_align);
int json_uring_acquire(struct json_uring *ring, void **data);
int json_uring_submit(struct json_uring *ring, unsigned slot, size_t len, off_t offset);
int json_uring_drain(struct json_uring *ring);
bool json_uring_idle(struct json_uring *ring);
void json_uring_get_stats(struct json_uring *ring, struct json_uring_stats *out);
void json_uring_destroy(struct json_uring *ring);

#endif

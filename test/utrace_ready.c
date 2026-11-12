// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "bpf.h"
#include "btf.h"
#include "libbpf.h"

struct recording_window {
	int fd;
	__u32 map_id;
	__u32 start_offset;
	__u32 end_offset;
	void *value;
};

static unsigned long long monotonic_ns(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long long)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static int timestamp_offset(const struct btf *btf, const struct btf_type *section, const char *name, __u32 *offset)
{
	if (!btf_is_datasec(section))
		return -ENOENT;
	const struct btf_var_secinfo *variables = btf_var_secinfos(section);
	for (__u32 i = 0; i < btf_vlen(section); i++) {
		const struct btf_type *variable = btf__type_by_id(btf, variables[i].type);
		if (strcmp(btf__name_by_offset(btf, variable->name_off), name))
			continue;
		if (!btf_is_var(variable) || variables[i].size != sizeof(__u64) ||
		    btf__resolve_size(btf, variable->type) != sizeof(__u64))
			return -EINVAL;
		*offset = variables[i].offset;
		return 0;
	}
	return -ENOENT;
}

static int inspect_map(__u32 map_id, struct recording_window *window)
{
	struct bpf_map_info info = {};
	__u32 info_size = sizeof(info);
	int fd = bpf_map_get_fd_by_id(map_id);
	if (fd < 0)
		return errno == ENOENT ? 0 : -errno;
	if (bpf_map_get_info_by_fd(fd, &info, &info_size)) {
		int err = -errno;
		close(fd);
		return err;
	}
	if (strncmp(info.name, "wprof", 5) || !strstr(info.name, ".bss") || info.type != BPF_MAP_TYPE_ARRAY) {
		close(fd);
		return 0;
	}
	struct btf *btf = btf__load_from_kernel_by_id(info.btf_id);
	int err = libbpf_get_error(btf);
	if (err) {
		close(fd);
		return err;
	}
	const struct btf_type *section = btf__type_by_id(btf, info.btf_value_type_id);
	if (!section || strcmp(btf__name_by_offset(btf, section->name_off), ".bss")) {
		err = -EINVAL;
		goto out;
	}
	err = timestamp_offset(btf, section, "session_start_ts", &window->start_offset);
	if (!err)
		err = timestamp_offset(btf, section, "session_end_ts", &window->end_offset);
	if (err)
		goto out;
	if (window->start_offset + sizeof(__u64) > info.value_size ||
	    window->end_offset + sizeof(__u64) > info.value_size || info.key_size != sizeof(__u32)) {
		err = -EINVAL;
		goto out;
	}
	window->value = malloc(info.value_size);
	if (!window->value) {
		err = -ENOMEM;
		goto out;
	}
	window->fd = fd;
	window->map_id = info.id;
	err = 1;
out:
	btf__free(btf);
	if (err != 1)
		close(fd);
	return err;
}

static int find_window(int pid, struct recording_window *window)
{
	char directory[64], path[PATH_MAX], line[512];
	snprintf(directory, sizeof(directory), "/proc/%d/fdinfo", pid);
	DIR *fds = opendir(directory);
	if (!fds)
		return -errno;
	int result = 0;
	struct dirent *entry;
	while ((entry = readdir(fds))) {
		if (!isdigit(entry->d_name[0]))
			continue;
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		FILE *file = fopen(path, "r");
		if (!file)
			continue;
		__u32 map_id = 0;
		while (fgets(line, sizeof(line), file)) {
			if (sscanf(line, "map_id: %u", &map_id) == 1)
				break;
		}
		fclose(file);
		if (!map_id)
			continue;
		result = inspect_map(map_id, window);
		if (result)
			break;
	}
	closedir(fds);
	return result;
}

static int read_pid(const char *pid_file)
{
	FILE *file = fopen(pid_file, "r");
	if (!file)
		return errno == ENOENT ? 0 : -errno;
	int pid = 0;
	int fields = fscanf(file, "%d", &pid);
	fclose(file);
	if (fields != 1)
		return 0;
	return pid > 0 ? pid : -EINVAL;
}

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s PID_FILE WPROF TIMEOUT_MS LAUNCHER_PID\n", argv[0]);
		return 1;
	}
	struct stat executable;
	if (stat(argv[2], &executable)) {
		perror("stat wprof executable");
		return 1;
	}
	struct pollfd processes[2] = {
		{ .fd = syscall(SYS_pidfd_open, atoi(argv[4]), 0), .events = POLLIN },
		{ .fd = -1, .events = POLLIN },
	};
	if (processes[0].fd < 0) {
		perror("pidfd_open launcher");
		return 1;
	}
	struct recording_window window = { .fd = -1 };
	unsigned long long deadline = monotonic_ns() + strtoull(argv[3], NULL, 10) * 1000000ULL;
	unsigned long long start = 0, end = 0;
	int result = 1, pid = 0;
	while (monotonic_ns() < deadline) {
		if (poll(processes, 2, 0) > 0) {
			fprintf(stderr, "wprof exited before recording readiness\n");
			goto out;
		}
		if (!pid) {
			pid = read_pid(argv[1]);
			if (pid < 0) {
				fprintf(stderr, "read owned wprof PID: %s\n", strerror(-pid));
				goto out;
			}
			if (pid) {
				processes[1].fd = syscall(SYS_pidfd_open, pid, 0);
				if (processes[1].fd < 0) {
					perror("pidfd_open wprof");
					goto out;
				}
			}
		}
		if (pid && window.fd < 0) {
			char path[64];
			struct stat actual;
			snprintf(path, sizeof(path), "/proc/%d/exe", pid);
			if (!stat(path, &actual) && actual.st_dev == executable.st_dev && actual.st_ino == executable.st_ino) {
				int err = find_window(pid, &window);
				if (err < 0) {
					fprintf(stderr, "inspect owned wprof BPF maps: %s\n", strerror(-err));
					goto out;
				}
			}
		}
		if (window.fd >= 0) {
			__u32 key = 0;
			if (bpf_map_lookup_elem(window.fd, &key, window.value)) {
				perror("read wprof recording window");
				goto out;
			}
			memcpy(&start, (char *)window.value + window.start_offset, sizeof(start));
			memcpy(&end, (char *)window.value + window.end_offset, sizeof(end));
			unsigned long long now = monotonic_ns();
			if (start && start <= now && end > now && end > start && end > monotonic_ns() &&
			    poll(processes, 2, 0) == 0) {
				printf("{\"pid\":%d,\"map_id\":%u,\"session_start_ts\":%llu,\"session_end_ts\":%llu}\n",
				       pid, window.map_id, start, end);
				result = 0;
				goto out;
			}
		}
		poll(processes, 2, 10);
	}
	fprintf(stderr, "timeout waiting for wprof recording (pid %d, map %u, start %llu, end %llu)\n",
		pid, window.map_id, start, end);
out:
	if (window.fd >= 0)
		close(window.fd);
	free(window.value);
	if (processes[1].fd >= 0)
		close(processes[1].fd);
	close(processes[0].fd);
	return result;
}

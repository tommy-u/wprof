// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void *libraries[2];
__attribute__((noinline)) void coverage_run(void)
{
	const char *names[] = {"cuLaunchKernel", "cuCtxSynchronize_v2", "cuDeviceGetAttribute", "cudaLaunchKernel", "cudaDeviceSynchronize", "cudaSetDevice", "cudaGetErrorString", "cuLaunchKernel_ptsz", "cudaLaunchKernel_ptsz"};
	for (int i = 0; i < 9; i++) {
		void (*call)(void) = dlsym(libraries[i < 3 || i == 7 ? 0 : 1], names[i]);
		if (call)
			call();
	}
}

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int worker_ready, worker_start;

static void run_calls(void)
{
	for (int i = 0; i < 30; i++) {
		coverage_run();
		struct timespec pause = {.tv_nsec = 1000000};
		nanosleep(&pause, NULL);
	}
}

static void *worker(void *arg)
{
	pthread_setname_np(pthread_self(), "cuda-api-worker");
	pthread_mutex_lock(&mutex);
	worker_ready = 1;
	pthread_cond_signal(&condition);
	while (!worker_start)
		pthread_cond_wait(&condition, &mutex);
	pthread_mutex_unlock(&mutex);
	run_calls();
	return NULL;
}

int main(int argc, char **argv)
{
	if (argc != 3 && argc != 4)
		return 2;
	libraries[0] = dlopen(argv[1], RTLD_NOW);
	libraries[1] = dlopen(argv[2], RTLD_NOW);
	if (!libraries[0] || !libraries[1])
		return 1;
	pthread_t thread;
	if (argc == 4) {
		pthread_mutex_lock(&mutex);
		if (pthread_create(&thread, NULL, worker, NULL))
			return 1;
		while (!worker_ready)
			pthread_cond_wait(&condition, &mutex);
		pthread_mutex_unlock(&mutex);
	}
	puts("ready");
	fflush(stdout);
	if (getchar() == EOF)
		return 1;
	if (argc == 4) {
		pthread_mutex_lock(&mutex);
		worker_start = 1;
		pthread_cond_signal(&condition);
		pthread_mutex_unlock(&mutex);
		pthread_join(thread, NULL);
	} else {
		run_calls();
	}
	puts("done");
	fflush(stdout);
	return getchar() == EOF;
}

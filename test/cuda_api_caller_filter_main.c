// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include <dlfcn.h>
#include <stdio.h>

static int (*attribute)(void);
static int (*synchronize)(void);
static int (*runtime)(void);
#ifdef STATIC_RUNTIME
int cudaDeviceSynchronize(void);
#endif

__attribute__((noinline)) int coverage_run(void)
{
	if (attribute() != -3)
		return 1;
	if (runtime && runtime() != 4)
		return 1;
	return synchronize() != 7;
}

int main(int argc, char **argv)
{
	if (argc != 2 && argc != 3)
		return 2;
	void *driver = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
	if (!driver)
		return 1;
	attribute = dlsym(driver, "cuDeviceGetAttribute");
	synchronize = dlsym(driver, "cuCtxSynchronize");
	if (!attribute || !synchronize)
		return 1;
#ifdef STATIC_RUNTIME
	runtime = cudaDeviceSynchronize;
#endif
	if (argc == 3) {
		void *library = dlopen(argv[2], RTLD_NOW);
		if (!library)
			return 1;
		runtime = dlsym(library, "cudaDeviceSynchronize");
		if (!runtime)
			return 1;
	}
	puts("ready");
	fflush(stdout);
	if (getchar() == EOF)
		return 1;
	for (int i = 0; i < 8; i++) {
		if (coverage_run())
			return 1;
	}
	puts("done");
	fflush(stdout);
	return getchar() == EOF;
}

// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include <stdint.h>

__attribute__((noinline)) int cuLaunchKernel(void) { return 0; }
__attribute__((noinline)) int cuCtxSynchronize_v2(void) { return 0; }
__attribute__((noinline)) int cuDeviceGetAttribute(void) { return -3; }
__attribute__((noinline)) int cudaLaunchKernel(void) { return 0; }
__attribute__((noinline)) int cudaDeviceSynchronize(void) { return 0; }
#ifdef AMBIGUOUS_VERSION
__attribute__((noinline)) int cudaSetDeviceFirst(void) { return 1; }
__attribute__((noinline)) int cudaSetDeviceSecond(void) { return 2; }
__asm__(".symver cudaSetDeviceFirst,cudaSetDevice@FIRST");
__asm__(".symver cudaSetDeviceSecond,cudaSetDevice@SECOND");
#else
__attribute__((noinline)) int cudaSetDevice(void) { return 0; }
#endif
__attribute__((noinline)) const char *cudaGetErrorString(void) { return "fixture error"; }

int cuLaunchKernel_ptsz(void) __attribute__((alias("cuLaunchKernel")));
__attribute__((noinline)) int cudaLaunchKernel_ptsz(void) { return 4; }

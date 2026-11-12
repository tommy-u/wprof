// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)

int cuDeviceGetAttribute(void);
int cuCtxSynchronize(void);

__attribute__((noinline)) int cudaDeviceSynchronize(void)
{
	int attribute = cuDeviceGetAttribute();
	int synchronized = cuCtxSynchronize();
	return attribute + synchronized;
}

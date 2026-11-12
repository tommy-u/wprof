// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cuda_api.h"

int main(void)
{
	assert(!strcmp(cuda_api_status_params(CUDA_API_STATUS_S32, false), ", arg:ret:s32/name(status)"));
	assert(!strcmp(cuda_api_status_params(CUDA_API_STATUS_S32, true), ""));
	assert(!strcmp(cuda_api_status_params(CUDA_API_STATUS_NONE, false), ""));
	assert(!strcmp(cuda_api_status_params(CUDA_API_STATUS_NONE, true), ""));
	puts("CUDA return policy PASS");
}

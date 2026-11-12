/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
#ifndef __CUDA_API_H_
#define __CUDA_API_H_
#include <stdbool.h>
#include "wprof_types.h"
#include "utrace.h"

#define CUDA_API_TRACK_NAME "CUDA host API"

enum cuda_api_status {
	CUDA_API_STATUS_NONE,
	CUDA_API_STATUS_S32,
};

struct cuda_api_site {
	const char *library;
	const char *name;
	enum cuda_api_status status;
};

static inline const char *cuda_api_status_params(enum cuda_api_status status, bool sealed)
{
	return !sealed && status == CUDA_API_STATUS_S32 ? ", arg:ret:s32/name(status)" : "";
}

int cuda_api_setup(void);
int cuda_api_definition_pid(const char *definition);
int cuda_api_record_attachment(enum utrace_mapped_attach_mode mode, u64 elapsed_ns);
void cuda_api_cleanup(void);
#endif

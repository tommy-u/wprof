// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/* Copyright (c) 2026 Meta Platforms, Inc. */
#include "cuda_api.h"
#include "env.h"
#include "strs.h"
#include "utrace.h"

static const struct cuda_api_site sites[] = {
	{"libcuda.so", "cuLaunchKernel", CUDA_API_STATUS_S32},
	{"libcuda.so", "cuCtxSynchronize_v2", CUDA_API_STATUS_S32},
	{"libcuda.so", "cuDeviceGetAttribute", CUDA_API_STATUS_S32},
	{"libcudart.so", "cudaLaunchKernel", CUDA_API_STATUS_S32},
	{"libcudart.so", "cudaDeviceSynchronize", CUDA_API_STATUS_S32},
	{"libcudart.so", "cudaSetDevice", CUDA_API_STATUS_S32},
};
static const char *libraries[] = {"libcuda.so", "libcudart.so"};

static int outcome(int pid, const char *status, int err)
{
	wprintf("cuda-api: PID %d: %s (%d)\n", pid, status, err);
	return append_str(&env.metadata, &env.metadata_cnt, sfmt("cuda-api.target.%d=%s,error:%d", pid, status, err));
}

static int prepare_target(int pid)
{
	int err = 0;
	int count = 0;
	for (int i = 0; i < ARRAY_SIZE(libraries); i++) {
		char *path = NULL;
		int fd = utrace_open_mapped_binary(pid, libraries[i], &path);
		if (fd < 0) {
			wprintf("cuda-api: PID %d %s: unavailable_or_ambiguous (%d)\n", pid, libraries[i], fd);
			err = append_str(&env.metadata, &env.metadata_cnt,
					 sfmt("cuda-api.library.%d.%s=unavailable_or_ambiguous,error:%d", pid, libraries[i], fd));
			if (err)
				return err;
			continue;
		}
		struct sbuf def = sbuf_new();
		for (int j = 0; j < ARRAY_SIZE(sites); j++) {
			if (strcmp(sites[j].library, libraries[i]))
				continue;
			const char *name = sites[j].name;
			int first = env.utrace_cfg_cnt;
			const char *status = cuda_api_status_params(sites[j].status, env.seals & SEAL_UPROBE_ARGS);
			sbuf_reset(&def);
			sbuf_appendf(&def, "uspan:%s (pid:%d, path:%s%s%s) | id:\"%s\", scope:thread |",
				     name, pid, path,
				     env.requested_stack_traces & ST_UTRACE ? ", stack" : "",
				     status, CUDA_API_TRACK_NAME);
			err = ucfg_parse(sbuf_str(&def));
			if (!err)
				err = ucfg_validate_seals(first);
			if (!err)
				err = utrace_bind_mapped_uprobe(&env.utrace_cfgs[first], fd);
			if (err == -ENOENT || err == -ESRCH || err == -EEXIST) {
				/* A missing/ambiguous symbol adds no probe; other available sites remain useful. */
				env.utrace_cfg_cnt = first;
				wprintf("cuda-api: PID %d site %s missing_or_ambiguous (%d)\n", pid, name, err);
				err = append_str(&env.metadata, &env.metadata_cnt,
						 sfmt("cuda-api.site.%d.%s=missing_or_ambiguous,error:%d", pid, name, err));
			} else if (!err) {
				count++;
				wprintf("cuda-api: PID %d site %s prepared (%s)\n", pid, name, path);
			}
			if (err)
				break;
		}
		sbuf_free(&def);
		close(fd);
		free(path);
		if (err)
			return err;
	}
	return outcome(pid, count ? "span_prepared" : "no_available_sites", 0);
}

int cuda_api_definition_pid(const char *definition)
{
	const char *params = strstr(definition, ", pid:");
	if (params)
		params += 2;
	else if ((params = strstr(definition, " (pid:")))
		params += 2;
	int pid;
	if (strncmp(definition, "uspan:", 6) || !strstr(definition, " | id:'" CUDA_API_TRACK_NAME "'") ||
	    !params || sscanf(params, "pid:%d", &pid) != 1 || pid <= 0)
		return -1;
	return pid;
}

int cuda_api_setup(void)
{
	if (!env.cuda_api_enabled)
		return 0;
	int err = append_str(&env.metadata, &env.metadata_cnt,
			     sfmt("cuda-api.status=%s",
				  env.seals & SEAL_UPROBE_ARGS ? "disabled_by_uprobe_args_seal" : "signed32_return"));
	if (err)
		return err;
	for (int i = 0; i < env.cuda_api_pid_cnt; i++) {
		err = prepare_target(env.cuda_api_pids[i]);
		if (err)
			return err;
	}
	return 0;
}

int cuda_api_record_attachment(enum utrace_mapped_attach_mode mode, u64 elapsed_ns)
{
	if (!env.cuda_api_enabled)
		return 0;
	const char *names[] = {"none", "individual_uprobes", "uprobe_multi", "mixed"};
	int err = append_str(&env.metadata, &env.metadata_cnt, sfmt("cuda-api.attach_mode=%s", names[mode]));
	if (!err)
		err = append_str(&env.metadata, &env.metadata_cnt, sfmt("cuda-api.utrace_attach_ns=%llu", elapsed_ns));
	return err;
}

void cuda_api_cleanup(void)
{
	free(env.cuda_api_pids);
	env.cuda_api_pids = NULL;
	env.cuda_api_pid_cnt = 0;
}

// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/* Copyright (c) 2026 Meta Platforms, Inc. */
#include "elf_utils.h"
#include "cuda_api.h"
#include "env.h"
#include "strs.h"
#include "utrace.h"

static const struct cuda_api_site sites[] = {
#include "cuda_api_sites.h"
};
static const char *libraries[] = {"libcudart.so", "libcuda.so"};

static int outcome(int pid, const char *status, int err)
{
	wprintf("cuda-api: PID %d: %s (%d)\n", pid, status, err);
	return append_str(&env.metadata, &env.metadata_cnt, sfmt("cuda-api.target.%d=%s,error:%d", pid, status, err));
}

static int prepare_target(int pid)
{
	struct utrace_mapped_range runtime_range = {};
	bool skip_nested_driver = false;
	int err = 0, count = 0;
	for (int i = 0; i < ARRAY_SIZE(libraries); i++) {
		char *path = NULL;
		struct utrace_mapped_range exec_range = {};
		int fd = utrace_open_mapped_binary(pid, libraries[i], &path, &exec_range);
		if (fd < 0) {
			wprintf("cuda-api: PID %d %s: unavailable_or_ambiguous (%d)\n", pid, libraries[i], fd);
			err = append_str(&env.metadata, &env.metadata_cnt,
					 sfmt("cuda-api.library.%d.%s=unavailable_or_ambiguous,error:%d", pid, libraries[i], fd));
			if (err)
				return err;
			continue;
		}
		if (!strcmp(libraries[i], "libcudart.so"))
			runtime_range = exec_range;
		struct sbuf def = sbuf_new();
		char fd_path[64];
		const char *names[ARRAY_SIZE(sites)];
		const struct cuda_api_site *selected[ARRAY_SIZE(sites)];
		struct elf_sym_result resolved[ARRAY_SIZE(sites)];
		int total = 0, missing = 0;
		bool shared_address = false;
		for (int j = 0; j < ARRAY_SIZE(sites); j++) {
			if (strcmp(sites[j].library, libraries[i]))
				continue;
			names[total] = sites[j].name;
			selected[total] = &sites[j];
			total++;
		}
		u64 begin = ktime_now_ns();
		snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
		err = elf_find_dynsyms(fd_path, STT_FUNC, names, total, resolved);
		u64 resolve_ns = ktime_now_ns() - begin;
		for (int j = 0; !err && j < total; j++) {
			const struct cuda_api_site *site = selected[j];
			if (resolved[j].err) {
				missing++;
				err = append_str(&env.metadata, &env.metadata_cnt,
						 sfmt("cuda-api.site.%d.%s=missing_or_ambiguous,error:%d", pid, site->name, resolved[j].err));
				continue;
			}
			for (int k = 0; k < j; k++) {
				if (!resolved[k].err && resolved[k].offset == resolved[j].offset) {
					shared_address = true;
					vprintf("cuda-api: PID %d %s: %s and %s share file address %#lx\n",
						pid, libraries[i], names[k], site->name, resolved[j].offset);
					break;
				}
			}
			int first = env.utrace_cfg_cnt;
			sbuf_reset(&def);
			sbuf_appendf(&def, "uspan:%s (pid:%d, path:%s%s%s) | id:\"%s\", scope:thread |",
				     site->name, pid, path, env.requested_stack_traces & ST_UTRACE ? ", stack" : "",
				     cuda_api_status_params(site->status, env.seals & SEAL_UPROBE_ARGS), CUDA_API_TRACK_NAME);
			err = ucfg_parse(sbuf_str(&def));
			if (!err)
				err = ucfg_validate_seals(first);
			if (!err)
				err = utrace_bind_mapped_uprobe_offset(&env.utrace_cfgs[first], fd, resolved[j].offset);
			if (err)
				break;
			if (runtime_range.end && !strcmp(site->library, "libcuda.so")) {
				env.utrace_cfgs[first].skip_caller_start = runtime_range.start;
				env.utrace_cfgs[first].skip_caller_end = runtime_range.end;
				skip_nested_driver = true;
			}
			count++;
			vprintf("cuda-api: PID %d site %s prepared (%s)\n", pid, site->name, path);
		}
		sbuf_free(&def);
		close(fd);
		free(path);
		if (err)
			return err;
		err = append_str(&env.metadata, &env.metadata_cnt,
				 sfmt("cuda-api.library.%d.%s=sites:%d,missing:%d,resolve_ns:%llu", pid, libraries[i], total, missing, resolve_ns));
		if (err)
			return err;
		wprintf("cuda-api: PID %d %s: %d sites, %d missing\n", pid, libraries[i], total, missing);
		if (shared_address)
			wprintf("cuda-api: PID %d %s: one call may appear under several names\n", pid, libraries[i]);
	}
	err = append_str(&env.metadata, &env.metadata_cnt,
			 sfmt("cuda-api.nested_driver.%d=%s", pid, skip_nested_driver ? "skipped" : "retained"));
	if (err)
		return err;
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
				  env.seals & SEAL_UPROBE_ARGS ? "disabled_by_uprobe_args_seal" : "per_site_return_policy"));
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

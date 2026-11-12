/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/* Copyright (c) 2025 Meta Platforms, Inc. */
#ifndef __UTRACE_H_
#define __UTRACE_H_

struct wprof_bpf;
struct bpf_state;
struct utrace_cfg;

int utrace_open_mapped_binary(int pid, const char *library, char **display_path);
int utrace_bind_mapped_uprobe(struct utrace_cfg *cfg, int fd);
int utrace_bind_mapped_uprobe_offset(struct utrace_cfg *cfg, int fd, unsigned long offset);
void utrace_close_mapped_files(void);
void utrace_cleanup_mapped_files(void);

int utrace_setup(struct wprof_bpf *skel);
int utrace_attach(struct bpf_state *st, struct wprof_bpf *skel);

#endif /* __UTRACE_H_ */

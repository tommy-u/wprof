// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/bpf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>

#if !defined(__x86_64__)
#error This fault injector preserves the native x86-64 syscall ABI.
#endif

static long (*real_syscall)(long, ...);
static const char *failure;
static int injected;

__attribute__((constructor)) static void initialize(void)
{
    real_syscall = dlsym(RTLD_NEXT, "syscall");
    failure = getenv("WPROF_TEST_FAIL_MULTI");
    if (!real_syscall)
        abort();
}

__attribute__((used)) static long fault_bpf_syscall(long nr, long cmd, union bpf_attr *attr, unsigned long size)
{
    int fail = 0;
    if (failure && cmd == BPF_PROG_LOAD && attr->expected_attach_type == BPF_TRACE_UPROBE_MULTI)
        fail = !strcmp(failure, "load");
    if (failure && !injected && cmd == BPF_LINK_CREATE && attr->link_create.attach_type == BPF_TRACE_UPROBE_MULTI) {
        int is_return = attr->link_create.uprobe_multi.flags & BPF_F_UPROBE_MULTI_RETURN;
        fail |= (!is_return && !strcmp(failure, "attach-entry")) || (is_return && !strcmp(failure, "attach-return"));
        if (fail)
            injected = 1;
    }
    if (fail) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return real_syscall(nr, cmd, attr, size);
}

#define STR_(x) #x
#define STR(x) STR_(x)
/* Forward non-BPF syscalls unchanged, including callers that pass fewer than six arguments. */
__asm__(".pushsection .text\n"
        ".globl syscall\n"
        ".type syscall,@function\n"
        "syscall:\n"
        "cmp $" STR(__NR_bpf) ",%rdi\n"
        "je fault_bpf_syscall\n"
        "jmp *real_syscall(%rip)\n"
        ".size syscall,.-syscall\n"
        ".popsection\n");

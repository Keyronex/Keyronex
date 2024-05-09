#ifndef KRX_EXECUTIVE_EXP_H
#define KRX_EXECUTIVE_EXP_H

#include <keyronex/syscall.h>

#include "kdk/nanokern.h"

void ex_init(void *);

uintptr_t ex_syscall_dispatch(enum krx_syscall syscall, uintptr_t arg1,
    uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, uintptr_t arg5,
    uintptr_t arg6, uintptr_t *out1);

extern kthread_t *ex_init_thread;

#endif /* KRX_EXECUTIVE_EXP_H */

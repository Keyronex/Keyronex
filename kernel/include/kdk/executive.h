#ifndef KRX_KDK_EXECUTIVE_H
#define KRX_KDK_EXECUTIVE_H

#include "kdk/nanokern.h"

int
ps_create_kernel_thread(kthread_t **out, const char *name, void (*fn)(void *),
    void *arg);
void ps_exit_this_thread(void);

extern kprocess_t kernel_process;

#endif /* KRX_KDK_EXECUTIVE_H */

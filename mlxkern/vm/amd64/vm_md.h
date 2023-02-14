/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Mon Feb 13 2023.
 */

#ifndef MLX_AMD64_VMAMD64_H
#define MLX_AMD64_VMAMD64_H

#include <melantix/kerndefs.h>

#define PGSIZE 4096

/*! first address at which anything will be mapped in userspace */
#define USER_BASE 0x1000
/* beginning of higher half, which also begins the higher-half direct map */
#define HHDM_BASE 0xffff800000000000
/* addresses > this are unconditionally mapped user-accessible */
#define KAREA_BASE HHDM_BASE
/*! HHDM base + 1TiB */
#define KHEAP_BASE 0xffff810000000000
/* -2GiB */
#define KERN_BASE 0xffffffff80000000

#define ASSERT_IN_KHEAP(PTR)                    \
	kassert((uintptr_t)PTR >= KHEAP_BASE && \
	    (uintptr_t)PTR < KHEAP_BASE + 0x100000000)

#define USER_SIZE 0x100000000  /* 4GiB */
#define HHDM_SIZE 0x1000000000 /* 1TiB */
#define KHEAP_SIZE 0x100000000 /* 4GiB */
#define KERN_SIZE 0x10000000   /* 256MiB */

/*! Size of a kernel stack - wired */
#define KERNEL_STACK_NPAGES 6

/*! Size of a user program's stack - anonymous memory. */
#define USER_STACK_SIZE PGSIZE * 32

#define P2V(addr) (((void *)(addr)) + HHDM_BASE)
#define V2P(addr) (((void *)(addr)) - HHDM_BASE)

struct vm_ps_md {
	paddr_t cr3;
};

#endif /* MLX_AMD64_VMAMD64_H */

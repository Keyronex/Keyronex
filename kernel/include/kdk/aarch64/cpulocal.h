/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Fri Sep 20 2024.
 */
/*!
 * @file aarch64/cpulocal.h
 * @brief AArch64-specific CPU-local functionality.
 */

#ifndef KRX_AARCH64_CPULOCAL_H
#define KRX_AARCH64_CPULOCAL_H

struct md_kcpu_local_data {
};

#define KCPU_LOCAL_OFFSET(FIELD) __builtin_offsetof(kcpu_local_data_t, FIELD)

#define KCPU_LOCAL_LOAD(FIELD) ({ 				\
	register struct kcpu_local_data *_local asm("x18");	\
	_local->FIELD;						\
})

#define KCPU_LOCAL_STORE(FIELD, VALUE) ({			\
	register struct kcpu_local_data *_local asm("x18");	\
	_local->FIELD = VALUE;					\
})

#endif /* KRX_AARCH64_CPULOCAL_H */

/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Fri Sep 20 2024.
 */
/*!
 * @file amd64/cpulocal.h
 * @brief AMD64-specific CPU-local functionality.
 */

#ifndef KRX_AMD64_CPULOCAL_H
#define KRX_AMD64_CPULOCAL_H

struct md_kcpu_local_data {
	struct kcpu_local_data *self;
};

#define KCPU_LOCAL_OFFSET(FIELD) __builtin_offsetof(kcpu_local_data_t, FIELD)

#define KCPU_LOCAL_LOAD(FIELD) ({ 				\
	static struct kcpu_local_data __seg_gs *_local ;	\
	_local->FIELD;						\
})

#define KCPU_LOCAL_STORE(FIELD, VALUE) ({			\
	static struct kcpu_local_data __seg_gs *_local ;	\
	_local->FIELD = VALUE;					\
})

#endif /* KRX_AMD64_CPULOCAL_H */

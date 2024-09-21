/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Fri Sep 20 2024.
 */
/*!
 * @file m68k/cpulocal.h
 * @brief m68k-specific CPU-local functionality.
 */

#ifndef KRX_M68K_CPULOCAL_H
#define KRX_M68K_CPULOCAL_H

struct md_kcpu_local_data {
	struct kcpu_local_data *self;
};

extern struct kcpu_local_data cpu_local_data;

#define KCPU_LOCAL_OFFSET(FIELD) __builtin_offsetof(kcpu_local_data_t, FIELD)

#define KCPU_LOCAL_LOAD(FIELD) (cpu_local_data.FIELD)

#define KCPU_LOCAL_STORE(FIELD, VALUE) (void)(cpu_local_data.FIELD = VALUE)

#endif /* KRX_M68K_CPULOCAL_H */

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

#define KCPU_LOCAL_LOAD(FIELD) ({ 					\
	__typeof(((kcpu_local_data_t *)0)->FIELD) value;		\
	switch(sizeof(value)) 						\
	{								\
	case 1: 							\
	case 2: 							\
	case 4: 							\
	case 8: 							\
		asm volatile("mov %%gs:%c1, %0"				\
			: "=r"(value)					\
			: "i"(KCPU_LOCAL_OFFSET(FIELD))			\
		); 							\
		break; 							\
	default:							\
		__builtin_trap(); 					\
	} 								\
	value; 								\
})

#define KCPU_LOCAL_STORE(FIELD, VALUE) ({ 				\
	switch(sizeof(((kcpu_local_data_t *)0)->FIELD)) 		\
	{								\
	case 1: 							\
	case 2: 							\
	case 4: 							\
	case 8: 							\
		asm volatile("mov %0, %%gs:%c1"				\
			: 						\
			: "r"(VALUE),					\
			  "i"(KCPU_LOCAL_OFFSET(FIELD))			\
		);							\
		break; 							\
	default:							\
		__builtin_trap(); 					\
	} 								\
})

#endif /* KRX_AMD64_CPULOCAL_H */

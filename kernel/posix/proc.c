/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Tue Jan 14 2025.
 */
/*!
 * @file proc.c
 * @brief POSIX process.
 */

#include "kdk/executive.h"

typedef struct psx_proc {
	uintptr_t pid;
} psx_proc_t;

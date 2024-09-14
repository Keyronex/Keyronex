/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Thu Sep 12 2024.
 */
/*!
 * @file kpaged.c
 * @brief Kernel paged heap.
 */

#include <kdk/kern.h>
#include <kdk/vm.h>

static kmutex_t kpaged_mutex = KMUTEX_INITIALIZER(kpaged_mutex);

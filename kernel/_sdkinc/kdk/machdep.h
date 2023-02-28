/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Feb 21 2023.
 */

#ifndef KRX_KDK_MACHDEP_H
#define KRX_KDK_MACHDEP_H

#ifdef __amd64
#include "./amd64/mdamd64.h"
#else
#error "Port machdep to this platform"
#endif

#endif /* KRX_KDK_MACHDEP_H */

/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Sun Feb 16 2025.
 */

#ifndef KRX_DDK_DKUTILITIES_H
#define KRX_DDK_DKUTILITIES_H

#define kAnsiYellow "\e[0;33m"
#define kAnsiReset "\e[0m"

#define DKLog(SUB, ...)({                            \
	kprintf(kAnsiYellow "%s: " kAnsiReset, SUB); \
	kprintf(__VA_ARGS__);                        \
})
#define DKDevLog(DEV, ...) DKLog((DEV).name, __VA_ARGS__)

#endif /* KRX_DDK_DKUTILITIES_H */

/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Tue Jan 28 2025.
 */

#ifndef KRX_LIBKERN_OSENUMERATOR_H
#define KRX_LIBKERN_OSENUMERATOR_H

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if __has_feature(nullability)
#else
#define _Nullable
#endif

#if __has_feature(objc_arc)
#else
#undef __unsafe_unretained
#define __unsafe_unretained
#endif

typedef struct {
	unsigned long state;
	id __unsafe_unretained _Nullable *_Nullable itemsPtr;
	unsigned long *_Nullable mutationsPtr;
	unsigned long extra[5];
} OSFastEnumerationState;

typedef OSFastEnumerationState NSFastEnumerationState;

#endif /* KRX_LIBKERN_OSENUMERATOR_H */

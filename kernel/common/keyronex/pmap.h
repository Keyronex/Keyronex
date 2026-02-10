/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file pmap.h
 * @brief Physical mapping definitions.
 */

#ifndef ECX_KEYRONEX_PMAP_H
#define ECX_KEYRONEX_PMAP_H

#include <keyronex/pmap_arch.h>

enum pmap_pte_kind {
	kPTEKindZero = 0,
	kPTEKindSwap,
	kPTEKindTrans,
	kPTEKindBusy,
	kPTEKindFork,
	kPTEKindHW,
};

static inline enum pmap_pte_kind
pmap_pte_characterise(pte_t pte)
{
	if (pmap_pte_is_hw(pte))
		return kPTEKindHW;
	else
		return pte.soft.kind;
}


#endif /* ECX_KEYRONEX_PMAP_H */

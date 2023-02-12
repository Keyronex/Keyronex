/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Sat Feb 11 2023.
 */
/*!
 * @file mm.h
 * @brief The memory manager's internal data structures.
 *
 * Working Set Lists
 * -----------------
 *
 * The implementation of these is based very closely on that of Mintia:
 * see https://github.com/xrarch/mintia
 *
 *
 *
 * Locking
 * -------
 *
 * The general principle: the PFN lock is big and covers everything done from
 * within an object down.
 *
 * The VAD list mutex protects process' trees of VAD lists.
 */

#ifndef MLX_MM_MM_H
#define MLX_MM_MM_H

#include <bsdqueue/tree.h>
#include <ke/ke.h>
#include <stdint.h>

/*! A virtual address. */
typedef uintptr_t vaddr_t;
/*! A physical address. */
typedef uintptr_t paddr_t;

/*!
 * Physical page frame description. Whole thing (?) locked by the PFN lock.
 */
typedef struct mm_page {
	/*! By how many PTEs is it mapped? */
	uint16_t reference_count;
	enum mm_page_use {
		/*! The page is on the freelist. */
		kPageUseFree,
		/*! The page is on the modified or standby list. */
		kPageUseModified,
		/*! The page is mapped in at least one working set. */
		kPageUseActive,
		/*! The page is in transition. */
		kPageUseTransition,
		/*! The page is used by kernel wired memory. */
		kPageUseWired,
	} use : 4;

	/*! Page's physical address. */
	paddr_t address;

	union {
		/*! Virtual page, if ::is_anonymous. */
		struct mm_vpage *vpage;
		/*! File, if ::is_file */
		struct mm_file *file;
	};
} mm_page_t;

/*!
 * Virtual page frame for anonymous pages, which are created both for anonymous
 * memory proper as well as for to provide the copy-on-write layer for mapped
 * files mapped copy-on-write.
 *
 * Whole thing (?) locked by the PFN lock.
 */
typedef struct mm_vpage {
	/*! physical page frame description if resident */
	mm_page_t *page;
	/*! swap descriptor, if it's been written */
	uintptr_t swapdesc;
} mm_vpage_t;

/*!
 * Working set list (or WSL). A modified ring buffer which can be grown or
 * shrunk, and employing a freelist so that cleared entries can be used.
 */
typedef struct mm_working_set_list {
	/*! Pointer to array of virtual addresses/freelist linkage. */
	uintptr_t *entries;
	/*! Actual size of the array. */
	size_t array_size;
	/*! Current maximum size of the working set, may be lower than
	 * array_size.*/
	size_t max_size;
	/*! Currently used number of entries (including cleared ones). */
	size_t cur_size;
	/*! Index of least recently inserted page. */
	size_t head;
	/*! Index of most recently inserted page */
	size_t tail;
	/*! Head of the freelist, if there is one. */
	uintptr_t **freelist_head;
} mm_working_set_list_t;

/*!
 * @brief Adds a virtual address to the working set list.
 *
 * This function adds a virtual address entry to the working set list.
 * If the working set list is below its maximal size, and there is no low-memory
 * condition, it will be appended.
 * Otherwise, if the working set list is at its maximum, it will try to expand
 * its size and add the entry.
 * If the expansion fails, the function will dispose of the least recently added
 * entry in the working set list and add the new entry in its place.
 *
 * @param ws Pointer to the working set list.
 * @param entry The virtual address entry to add to the working set list.
 */
void mi_wsl_insert(mm_working_set_list_t *ws, vaddr_t entry);

/**
 * @brief Trims a specified number of pages from the working set list.
 *
 * This function removes a specified number of pages, starting with the least
 * recently used, from a working set list. If the number of entries to be
 * trimmed is equal to the current size of the working set list, then all the
 * entries will be disposed.
 *
 * @param ws Pointer to the working set list.
 * @param n Number of entries to be trimmed.
 */
void mi_wsl_trim_n_entries(mm_working_set_list_t *ws, size_t n);

#endif /* MLX_MM_MM_H */

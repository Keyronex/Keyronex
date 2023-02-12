#include <bsdqueue/queue.h>
#include <stdbool.h>

#include "mm/mm.h"

#define IS_FREELIST_LINK(entry) (((entry)&1) == 1)
#define DETAG(PENTRY) ((uintptr_t **)(((uintptr_t)PENTRY) & ~1))

/*! Dispose of an entry in the working set list. */
static void
wsl_dispose(vaddr_t vaddr)
{
	(void)vaddr;
}

static void
wsl_realloc(mm_working_set_list_t *ws, size_t new_size)
{
	uintptr_t *new_entries = (uintptr_t *)malloc(
	    new_size * sizeof(uintptr_t));
	int i = ws->head;
	int new_tail = 0;

	ws->cur_size = 0;

	while (i != ws->tail) {
		uintptr_t entry = ws->entries[i++];
		if (!IS_FREELIST_LINK(entry)) {
			new_entries[new_tail++] = entry;
		}
		if (i >= ws->max_size) {
			i = 0;
		}
	}
	new_entries[new_tail] = ws->entries[ws->tail];
	free(ws->entries);
	ws->entries = new_entries;
	ws->head = 0;
	ws->tail = new_tail;
	ws->array_size = new_size;
}

static bool
wsl_grow(mm_working_set_list_t *ws)
{
	size_t increment = 16;

	/* todo: check if we're ALLOWED to grow first.... */

	if (ws->max_size + increment <= ws->array_size) {
		/* can fit new max size within the array size; simply grow */
		ws->max_size += increment;
	} else {
		/* need to allocate a bigger array. increase its size a bit
		 * more. */
		wsl_realloc(ws, ws->array_size + increment * 4);
		ws->max_size += increment;
	}

	return true;
}

void
mi_wsl_insert(mm_working_set_list_t *ws, vaddr_t entry)
{
	if (ws->freelist_head != 0) {
		/* we are always allowed to use freelist entries, they're
		 * cruelly included in our working set size*/
		uintptr_t *freelist = *DETAG(ws->freelist_head);
		if (*freelist != 0) {
			ws->freelist_head = DETAG(*freelist);
		}
		*freelist = entry;
	} else {
		size_t new_tail = (ws->tail + 1) % ws->array_size;
		if (new_tail == ws->head) {
			/* out of slots, first try to expand the WSL */
			if (wsl_grow(ws)) {
				/* expanded, can now append as normal */
				goto append;
			} else {
				/* could not expand, dispose of head entry and
				 * replace it */
				wsl_dispose(ws->entries[ws->head]);
				ws->entries[ws->head] = entry;
				ws->head = (ws->head + 1) % ws->array_size;
				ws->tail = (ws->tail + 1) % ws->array_size;
			}
		} else {
		append:
			/* the simple case: no disposal, just appending */
			new_tail = (ws->tail + 1) % ws->array_size;
			ws->entries[new_tail] = entry;
			ws->tail = new_tail;
			ws->cur_size++;
		}
	}
}

void
mi_wsl_trim_n_entries(mm_working_set_list_t *ws, size_t n)
{
	uintptr_t i, entry;

	assert(!(n > ws->cur_size));

	if (n == ws->cur_size) {
		/*! in this case, clear everything */
		for (i = ws->head; i != ws->tail; i = (i + 1) % ws->max_size) {
			entry = ws->entries[i];
			wsl_dispose(entry);
		}
		ws->head = ws->tail = 0;
		ws->cur_size = 0;
		return;
	}
	for (i = ws->head; i != (ws->head + n) % ws->max_size;
	     i = (i + 1) % ws->max_size) {
		entry = ws->entries[i];
		wsl_dispose(entry);
	}
	ws->head = (ws->head + n) % ws->max_size;
	ws->cur_size -= n;
}
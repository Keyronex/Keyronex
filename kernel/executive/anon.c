/*!
 * @file anon.c
 * @brief Implements the "Memory Object" object type, a wrapper for
 * vm_object_ts.
 */

#include "kdk/kern.h"
#include "kdk/object.h"
#include "kdk/vm.h"
#include "vm/vmp.h"

typedef struct ex_memory_object {
	vm_object_t obj;
} ex_memory_object_t;

extern obj_class_t anonymous_class;

int
ex_memory_object_new(ex_memory_object_t *out, size_t size)
{
	ex_memory_object_t *obj;
	vm_page_t *vpml4;
	int r;

	r = obj_new(&obj, anonymous_class, sizeof(ex_memory_object_t),
	    "memory object");
	if (r != 0)
		goto out;

	r = vm_page_alloc(&vpml4, 0, kPageUseVPML4, false);
	if (r != 0)
		goto free_obj;

	obj->obj.kind = kAnon;
	obj->obj.vpml4 = vmp_page_paddr(vpml4);
	ke_mutex_init(&obj->obj.map_entry_list_lock);
	LIST_INIT(&obj->obj.map_entry_list);

free_obj:
	obj_release(obj);
out:
	return r;
}

int
ex_memory_object_map(ex_memory_object_t *obj)
{
	vaddr_t vaddr;
	int r;

	obj_retain(obj);
	r = vm_ps_map_object_view(ex_curproc()->vm, &obj->obj, &vaddr, PGSIZE,
	    0, kVMAll, kVMAll, true, false, true);

	return r;
}

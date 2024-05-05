#include <stdint.h>

#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/nanokern.h"
#include "kdk/object.h"

struct object_header {
	/*! entry in type_object::objects */
	LIST_ENTRY(object_header) list_link;
	/*! object name, may be null */
	char *name;
	/*! count of handles and pointer references to object */
	uint32_t refcount;
	/*! size of object */
	size_t size : 24;
	/*! index into class_list */
	obj_class_t class;
};

struct type_object {
	/*! all objects of this type */
	LIST_HEAD(, object_header) objects;
	/*! count of objects of this type */
	uint32_t count;
	/*! guards objects */
	kmutex_t mutex;
};

struct type_object_with_header {
	struct object_header header;
	struct type_object object;
};

static struct type_object_with_header types[255];

static inline struct object_header *
header_from_object(void *object)
{
	return (void *)(((char *)object) - sizeof(struct object_header));
}

static inline void *
object_from_header(void *header)
{
	return (void *)(((char *)header) + sizeof(struct object_header));
}

static void
new_common(struct object_header *hdr, struct type_object *type,
    const char *name, size_t size)
{
	kassert(splget() < kIPLDPC);
	obj_retain(type);
	ke_wait(&type->mutex, "new_common:type->mutex", false, false, -1);
	LIST_INSERT_HEAD(&type->objects, hdr, list_link);
	type->count++;
	hdr->name = name == NULL ? NULL : strdup(name);
	hdr->size = size;
	hdr->refcount = 1;
	ke_mutex_release(&type->mutex);
}

obj_class_t
obj_new_type(const char *name)
{
	obj_class_t type_index = (uint8_t)-1;

	for (int i = 0; i < 255; i++)
		if (types[i].header.class == (uint8_t)-1) {
			type_index = i;
			break;
		}

	kassert(type_index != (uint8_t)-1);

	types[type_index].header.refcount = 1;
	/* the class of Type is Type */
	types[type_index].header.class = 0;

	new_common(&types[type_index].header, &types[0].object, name,
	    sizeof(struct type_object));

	kprintf("obj_new_type: new type %s defined (id %d)\n", name,
	    type_index);

	return type_index;
}

void
obj_init(void)
{
	for (int i = 0; i < 255; i++) {
		ke_mutex_init(&types[i].object.mutex);
		LIST_INIT(&types[i].object.objects);
		types[i].header.class = (uint8_t)-1;
	}

	obj_new_type("Type");
}

int
obj_new(void *ptr_out, obj_class_t klass, size_t size, const char *name)
{
	struct type_object *type_obj;
	struct object_header *hdr;

	type_obj = &types[klass].object;
	obj_retain((type_obj));

	hdr = kmem_alloc(sizeof(struct object_header) + size);
	new_common(hdr, &types[klass].object, name, size);

	*(void **)ptr_out = object_from_header(hdr);

	return 0;
}

void *
obj_retain(void *object)
{
	struct object_header *hdr = header_from_object(object);
	uint32_t count = __atomic_fetch_add(&hdr->refcount, 1,
	    __ATOMIC_SEQ_CST);
	kassert(count != 0 && count < 0xfffffff0);
	return object;
}

/*! @brief Release a pointer to an object. */
void
obj_release(void *object)
{
	struct object_header *hdr = header_from_object(object);

	while (true) {
		uint32_t old_count = __atomic_load_n(&hdr->refcount,
		    __ATOMIC_ACQUIRE);
		if (old_count > 1) {
			if (__atomic_compare_exchange_n(&hdr->refcount,
				&old_count, old_count - 1, false,
				__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
				return;
			}
		} else if (old_count == 1) {
			if (splget() >= kIPLDPC) {
				kfatal("just call obj_release in a worker");
			} else {
#if 0
				if (do the release(obj))
					return;
#endif
			}
		} else {
			kassert("unreached\n");
		}
	}
}

const char *
obj_name(void *obj)
{
	struct object_header *hdr = header_from_object(obj);
	return hdr->name;
}

char **
obj_name_ptr(void *obj)
{
	struct object_header *hdr = header_from_object(obj);
	return &hdr->name;
}

void
obj_dump(void)
{
	kprintf(" -- Objects --\n");
	for (int i = 0; i < 255; i++) {
		struct type_object_with_header *type = &types[i];
		struct object_header *header;

		if (type->header.class == (uint8_t)-1)
			continue;

		kprintf("Type: %s\n", type->header.name);

		LIST_FOREACH (header, &type->object.objects, list_link) {
			kprintf(" Object <%s> (%p) RC %u\n",
			    header->name == NULL ? "?" : header->name,
			    object_from_header(header), header->refcount);
		}
	}
}

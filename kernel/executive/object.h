#ifndef KRX_EXECUTIVE_OBJECT_H
#define KRX_EXECUTIVE_OBJECT_H

#include "kdk/object.h"
#include "kdk/vfs.h"

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

struct file {
	namecache_handle_t nch;
	io_off_t offset;
};

#endif /* KRX_EXECUTIVE_OBJECT_H */

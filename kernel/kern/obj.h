#ifndef OBJ_H_
#define OBJ_H_

#include <stdatomic.h>

/*!
 * object types
 */
typedef enum objecttype {
	/*! a mappable virtual memory object */
	kOTObject,
	/*! a virtual node */
	kOTVNode,
} objecttype_t;

/*!
 * a reference counted object's header
 */
typedef struct objectheader {
	objecttype_t type;
	atomic_uint_fast32_t refcount;
} objectheader_t;

/*!
 * handle to an object; an entry in a process' handle table
 */
typedef struct handle {
	objectheader_t *obj;
} handle_t;

/* initialise an object header */
void obj_init(objectheader_t *hdr, objecttype_t type);

/*! retain an object (increase refcount) */
void obj_retain(objectheader_t *hdr);

/*! release an object (reduce refcount and potentially destroy) */
void obj_release(objectheader_t *hdr);

#endif /* OBJ_H_ */

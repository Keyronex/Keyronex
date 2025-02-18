#ifndef KRX_KDK_OBJECT_H
#define KRX_KDK_OBJECT_H

#include <kdk/queue.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t obj_class_t;
typedef void obj_t;

typedef void (*obj_free_fn_t)(void *);

/*! @brief Set up the object manager. */
void obj_init(void);

/*!
 * @brief Create a new object class.
 *
 * @param name Name of the class to create.
 */
obj_class_t obj_new_type(const char *name, obj_free_fn_t free);

/*!
 * @brief Create a new object instance.
 *
 * @param ptr_out Pointer to a pointer to write the object's address to.
 * @param klass Class of object to create.
 * @param size Size of object body.
 * @param name[optional] Name of the object.
 */
int obj_new(void *ptr_out, obj_class_t klass, size_t size, const char *name);

/*! @brief Retain a pointer to an object. */
void *obj_retain(void *object);
/*! @brief Try to retain a pointer to an object if its refcount is not 0. */
void * obj_tryretain_rcu(void *object);
/*! @brief Release a pointer to an object. */
void obj_release(void *object);

/*! @brief Get an object's name. */
const char *obj_name(void *obj);
/*! @brief Get a pointer to an object's name pointer field. */
char **obj_name_ptr(void *obj);

void obj_dump(void);

#endif /* KRX_KDK_OBJECT_H */

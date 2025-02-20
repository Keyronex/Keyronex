/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Jan 27 2025.
 */
/*
 * @file OSArray.h
 * @brief Libkern array class interface.
 */

#ifndef LIBKERN_OSARRAY_H_
#define LIBKERN_OSARRAY_H_

#include <libkern/OSEnumerator.h>
#include <libkern/OSObject.h>

/*!
 * @brief A mutable array class for the kernel environment.
 *
 * OSArray provides an ordered, mutable collection of objects.
 */
@interface OSArray : OSObject {
	id *_objects;
	size_t _count;
	size_t _capacity;
}

/*!
 * @brief Initializes an empty array.
 * @return A newly initialized array, or nil if initialization failed.
 */
- (id)init;

/*!
 * @brief Adds an object to the end of the array.
 * @param object The object to add. Must not be nil.
 */
- (void)addObject:(id)object;

/*!
 * @brief Inserts an object at the specified index.
 * @param object The object to insert. Must not be nil.
 * @param index The index at which to insert the object. Must be less than or
 * equal to count.
 */
- (void)insertObject:(id)object atIndex:(size_t)index;

/*!
 * @brief Removes the object at the specified index.
 * @param index The index of the object to remove. Must be less than count.
 */
- (void)removeObjectAtIndex:(size_t)index;

/*!
 * @brief Removes the last object in the array.
 */
- (void)removeLastObject;

/*!
 * @brief Removes all objects from the array.
 */
- (void)removeAllObjects;

/*!
 * @brief Returns the object at the specified index.
 * @param index The index of the object to retrieve. Must be less than count.
 * @return The object at index, or nil if the index is out of bounds.
 */
- (id)objectAtIndex:(size_t)index;

/*!
 * @brief Returns the number of objects in the array.
 * @return The number of objects.
 */
- (size_t)count;

/*!
 * @brief Replaces the object at the specified index.
 * @param index The index of the object to replace. Must be less than count.
 * @param object The object with which to replace the object at index.
 */
- (void)replaceObjectAtIndex:(size_t)index withObject:(id)object;

- (int)countByEnumeratingWithState:(OSFastEnumerationState *)state
			   objects:(id __unsafe_unretained[])buffer
			     count:(int)len;

@end

#endif /* LIBKERN_OSARRAY_H_ */

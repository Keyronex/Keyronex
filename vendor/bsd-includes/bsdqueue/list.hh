/*
   Intrusive single/double linked list for C++.

   version 0.1, augusti, 2013

   Copyright (C) 2013- Fredrik Kihlander

   This software is provided 'as-is', without any express or implied
   warranty.  In no event will the authors be held liable for any damages
   arising from the use of this software.

   Permission is granted to anyone to use this software for any purpose,
   including commercial applications, and to alter it and redistribute it
   freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
      claim that you wrote the original software. If you use this software
      in a product, an acknowledgment in the product documentation would be
      appreciated but is not required.
   2. Altered source versions must be plainly marked as such, and must not be
      misrepresented as being the original software.
   3. This notice may not be removed or altered from any source distribution.

   Fredrik Kihlander
*/

#ifndef INTRUSIVE_LIST_H_INCLUDED
#define INTRUSIVE_LIST_H_INCLUDED

#if defined(_INTRUSIVE_LIST_ASSERT_ENABLED)
#if !defined(_INTRUSIVE_LIST_ASSERT)
#include <assert.h>
#define _INTRUSIVE_LIST_ASSERT(cond, ...) assert(cond)
#endif
#else
#if !defined(_INTRUSIVE_LIST_ASSERT)
#define _INTRUSIVE_LIST_ASSERT(...)
#endif
#endif

template <typename T> struct list_node {
	list_node()
	    : next(0x0)
	    , prev(0x0)
	{
	}

	T *next;
	T *prev;
};

/**
 * intrusive double linked list.
 */
template <typename T, list_node<T> T::*NODE> class list {
	T *head_ptr;
	T *tail_ptr;

    public:
	list()
	    : head_ptr(0x0)
	    , tail_ptr(0x0)
	{
	}
	~list() { clear(); }

	/**
	 * insert item at the head of list.
	 * @param item item to insert in list.
	 */
	void insert_head(T *elem)
	{
		list_node<T> *node = &(elem->*NODE);

		_INTRUSIVE_LIST_ASSERT(node->next == 0x0);
		_INTRUSIVE_LIST_ASSERT(node->prev == 0x0);

		node->prev = 0;
		node->next = head_ptr;

		if (head_ptr != 0x0) {
			list_node<T> *last_head = &(head_ptr->*NODE);
			last_head->prev = elem;
		}

		head_ptr = elem;

		if (tail_ptr == 0x0)
			tail_ptr = head_ptr;
	}

	/**
	 * insert item at the tail of list.
	 * @param item item to insert in list.
	 */
	void insert_tail(T *item)
	{
		if (tail_ptr == 0x0)
			insert_head(item);
		else {
			list_node<T> *tail_node = &(tail_ptr->*NODE);
			list_node<T> *item_node = &(item->*NODE);
			_INTRUSIVE_LIST_ASSERT(item_node->next == 0x0);
			_INTRUSIVE_LIST_ASSERT(item_node->prev == 0x0);
			tail_node->next = item;
			item_node->prev = tail_ptr;
			item_node->next = 0x0;
			tail_ptr = item;
		}
	}

	/**
	 * insert item in list after other list item.
	 * @param item item to insert in list.
	 * @param in_list item that already is inserted in list.
	 */
	void insert_after(T *item, T *in_list)
	{
		list_node<T> *node = &(item->*NODE);
		list_node<T> *in_node = &(in_list->*NODE);

		if (in_node->next) {
			list_node<T> *in_next = &(in_node->next->*NODE);
			in_next->prev = item;
		}

		node->next = in_node->next;
		node->prev = in_list;
		in_node->next = item;

		if (in_list == tail_ptr)
			tail_ptr = item;
	}

	/**
	 * insert item in list before other list item.
	 * @param item item to insert in list.
	 * @param in_list item that already is inserted in list.
	 */
	void insert_before(T *item, T *in_list)
	{
		list_node<T> *node = &(item->*NODE);
		list_node<T> *in_node = &(in_list->*NODE);

		if (in_node->prev) {
			list_node<T> *in_prev = &(in_node->prev->*NODE);
			in_prev->next = item;
		}

		node->next = in_list;
		node->prev = in_node->prev;
		in_node->prev = item;

		if (in_list == head_ptr)
			head_ptr = item;
	}

	/**
	 * remove the first item in the list.
	 * @return the removed item.
	 */
	T *remove_head()
	{
		T *ret = head();
		remove(head());
		return ret;
	}

	/**
	 * remove the last item in the list.
	 * @return the removed item.
	 */
	T *remove_tail()
	{
		T *ret = tail();
		remove(tail());
		return ret;
	}

	/**
	 * remove item from list.
	 * @param item the element to remove
	 * @note item must exist in list!
	 */
	void remove(T *item)
	{
		list_node<T> *node = &(item->*NODE);

		T *next = node->next;
		T *prev = node->prev;

		list_node<T> *next_node = &(next->*NODE);
		list_node<T> *prev_node = &(prev->*NODE);

		if (item == head_ptr)
			head_ptr = next;
		if (item == tail_ptr)
			tail_ptr = prev;
		if (prev != 0x0)
			prev_node->next = next;
		if (next != 0x0)
			next_node->prev = prev;

		node->next = 0x0;
		node->prev = 0x0;
	}

	/**
	 * return first item in list.
	 * @return first item in list or 0x0 if list is empty.
	 */
	T *head() { return head_ptr; }

	/**
	 * return last item in list.
	 * @return last item in list or 0x0 if list is empty.
	 */
	T *tail() const { return tail_ptr; }

	/**
	 * return next element in list after argument element or 0x0 on end of
	 * list.
	 * @param item item to get next element in list for.
	 * @note item must exist in list!
	 * @return element after item in list.
	 */
	T *next(T *item)
	{
		list_node<T> *node = &(item->*NODE);
		return node->next;
	}

	/**
	 * return next element in list after argument element or 0x0 on end of
	 * list.
	 * @param item item to get next element in list for.
	 * @note item must exist in list!
	 * @return element after item in list.
	 */
	const T *next(const T *item)
	{
		const list_node<T> *node = &(item->*NODE);
		return node->next;
	}

	/**
	 * return previous element in list after argument element or 0x0 on
	 * start of list.
	 * @param item item to get previous element in list for.
	 * @note item must exist in list!
	 * @return element before item in list.
	 */
	T *prev(T *item)
	{
		list_node<T> *node = &(item->*NODE);
		return node->prev;
	}

	/**
	 * return previous element in list after argument element or 0x0 on
	 * start of list.
	 * @param item item to get previous element in list for.
	 * @note item must exist in list!
	 * @return element before item in list.
	 */
	const T *prev(const T *item)
	{
		const list_node<T> *node = &(item->*NODE);
		return node->prev;
	}

	/**
	 * clear queue.
	 */
	void clear()
	{
		while (!empty())
			remove(head());
	}

	/**
	 * check if the list is empty.
	 * @return true if list is empty.
	 */
	bool empty() { return head_ptr == 0x0; }
};

#if 0
/**
 * macro to define node in struct/class to use in conjunction with list.
 *
 * @example
 * struct my_struct
 * {
 *     LIST_NODE( my_struct ) list1;
 *     LIST_NODE( my_struct ) list2;
 *
 *     int some_data;
 * };
 */
#define LIST_NODE(owner) list_node<owner>

/**
 * macro to define list that act upon specific member in struct-type.
 *
 * @example
 * LIST( my_struct, list1 ) first_list;
 * LIST( my_struct, list2 ) second_list;
 */
#define LIST(owner, member) list<owner, &owner::member>

#define LIST_INSERT_HEAD(PLIST, VAR, UNUSED_FIELD) (PLIST)->insert_head(VAR)
#define LIST_FOREACH(VAR, PLIST, UNUSED) \
	for ((VAR) = ((PLIST)->head()); (VAR); (VAR) = ((PLIST)->next(VAR)))
#endif

#define CXXLIST_FOREACH(VAR, PLIST, UNUSED) \
	for ((VAR) = ((PLIST)->head()); (VAR); (VAR) = ((PLIST)->next(VAR)))


#endif /* INTRUSIVE_LIST_H_INCLUDED */

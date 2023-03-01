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

#ifndef INTRUSIVE_SLIST_H_INCLUDED
#define INTRUSIVE_SLIST_H_INCLUDED

#if defined( _INTRUSIVE_LIST_ASSERT_ENABLED )
#  if !defined( _INTRUSIVE_LIST_ASSERT )
#    include <assert.h>
#    define _INTRUSIVE_LIST_ASSERT( cond, ... ) assert( cond )
#  endif
#else
#  if !defined( _INTRUSIVE_LIST_ASSERT )
#    define _INTRUSIVE_LIST_ASSERT( ... )
#  endif
#endif

template <typename T>
struct slist_node
{
	slist_node() : next( 0x0 ) {}
	T* next;
};

/**
 * intrusive single linked list.
 */
template <typename T, slist_node<T> T::*NODE>
class slist
{
	T* head_ptr;
	T* tail_ptr;

public:
	 slist()
		 : head_ptr( 0x0 )
		 , tail_ptr( 0x0 )
	 {}
	~slist() { clear(); }

	/**
	 * insert item at the head of list.
	 * @param item item to insert in list.
	 */
	void insert_head( T* elem )
	{
		slist_node<T>* node = &(elem->*NODE);
		_INTRUSIVE_LIST_ASSERT( node->next == 0x0 );
		node->next = head_ptr;
		head_ptr = elem;
		if( tail_ptr == 0x0 )
			tail_ptr = elem;
	}

	/**
	 * insert item at the tail of list.
	 * @param item item to insert in list.
	 */
	void insert_tail( T* elem )
	{
		if( tail_ptr == 0x0 )
			insert_head( elem );
		else
		{
			slist_node<T>* tail_node = &(tail_ptr->*NODE);
			_INTRUSIVE_LIST_ASSERT( tail_node->next == 0x0 );
			tail_node->next = elem;
			tail_ptr = elem;
		}
	}

	/**
	 * insert item in list after other list item.
	 * @param item item to insert in list.
	 * @param in_list item that already is inserted in list.
	 */
	void insert_after( T* elem, T* in_list )
	{
		slist_node<T>* insert_node  = &(elem->*NODE);
		slist_node<T>* in_list_node = &(in_list->*NODE);

		insert_node->next  = in_list_node->next;
		in_list_node->next = elem;
	}

	/**
	 * remove the first item in the list.
	 * @return the removed item.
	 */
	T* remove_head()
	{
		_INTRUSIVE_LIST_ASSERT( !empty() );
		slist_node<T>* node = &(head_ptr->*NODE);
		T* ret = head_ptr;
		if( head_ptr == tail_ptr )
			tail_ptr = node->next;
		head_ptr = node->next;
		(&(ret->*NODE))->next = 0x0;
		return ret;
	}

	/**
	 * remove item from list.
	 * @param item the element to remove
	 * @note item must exist in list!
	 */
	void remove( T* elem )
	{
		if( elem == head_ptr )
			remove_head();
		else
		{
			T* iter = head_ptr;
			while( iter != 0x0 )
			{
				slist_node<T>* iter_node = &(iter->*NODE);
				if( iter_node->next == elem )
				{
					if( iter_node->next == tail_ptr )
						tail_ptr = iter;

					slist_node<T>* elem_node = &(elem->*NODE);
					iter_node->next = elem_node->next;
					elem_node->next = 0;
					return;
				}

				iter = iter_node->next;
			}
		}
	}

	/**
	 * return first item in list.
	 * @return first item in list or 0x0 if list is empty.
	 */
	T* head() const { return head_ptr; }

	/**
	 * return last item in list.
	 * @return last item in list or 0x0 if list is empty.
	 */
	T* tail() const { return tail_ptr; }

	/**
	 * return next element in list after argument element or 0x0 on end of list.
	 * @param item item to get next element in list for.
	 * @note item must exist in list!
	 * @return element after item in list.
	 */
	T* next( T* prev )
	{
		slist_node<T>* node = &(prev->*NODE);
		return node->next;
	}

	/**
	 * return next element in list after argument element or 0x0 on end of list.
	 * @param item item to get next element in list for.
	 * @note item must exist in list!
	 * @return element after item in list.
	 */
	const T* next( const T* prev ) const
	{
		const slist_node<T>* node = &(prev->*NODE);
		return node->next;
	}

	/**
	 * clear queue.
	 */
	void clear()
	{
		while( !empty() )
			remove_head();
	}

	/**
	 * check if the list is empty.
	 * @return true if list is empty.
	 */
	bool empty() { return head_ptr == 0x0; }
};

#if 0
/**
 * macro to define node in struct/class to use in conjunction with slist.
 *
 * @example
 * struct my_struct
 * {
 *     SLIST_NODE( my_struct ) list1;
 *     SLIST_NODE( my_struct ) list2;
 *
 *     int some_data;
 * };
 */
#define SLIST_NODE( owner )    slist_node<owner>

/**
 * macro to define slist that act upon specific member in struct-type.
 *
 * @example
 * SLIST( my_struct, list1 ) first_list;
 * SLIST( my_struct, list2 ) second_list;
 */
#define SLIST( owner, member ) slist<owner, &owner::member>
#endif

#define CXXSLIST_FOREACH(VAR, PLIST, UNUSED) \
	for ((VAR) = ((PLIST)->head()); (VAR); (VAR) = ((PLIST)->next(VAR)))

#endif // INTRUSIVE_SLIST_H_INCLUDED
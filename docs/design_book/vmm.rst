Virtual Memory Manager
======================

The Keyronex virtual memory manager is principally derived from the
design of NetBSD's UVM. This provides for a flexible and portable
system. VM management APIs are platform-independent, with abstract
representation of address spaces, while the ``pmap`` module handles the
translation of that abstract representation into the form demanded by a
particular platform's memory management unit. The VMM is relatively
sophisticated when compared to those of other hobby kernels, providing
virtual memory proper (swap-backed anonymous memory) and page caching
(in collaboration with the VFS).

Data Structures
---------------

A number of data structures play a role in the system. This is an
overview of the major players:

``vm_page_t``
~~~~~~~~~~~~~

Represents a physical page of usable memory. These are placed on queues
according to their use and state. Queues include the free queue, wired
queues, and others - queues (and ``vm_page_t`` in greater depth) are
described in the section on paging. The set of all ``vm_page_t``\ s is
collectively called the Resident Pagetable (or RPT).

A ``vm_page_t`` is analogous to a ``struct page`` in GNU/Linux or a PFN
database entry in NT.

If it is being used as a page to store part of anonymous memory, it
holds a pointer to the ``vm_anon_t`` it belongs to; if it is being used
to store data belonging to a vnode VM object (analogous to a page cache
in GNU/Linux) it stores a pointer to the VM object it belongs to,
linkage for that object's RB tree of pages, and the offset of this page
within that object. In either of these cases, it also contains a
``pv_map`` list, which stores, for each mapping of the page, the
``vm_map_t`` into which it is mapped and its virtual address within that
map.

``vm_anon_t``
~~~~~~~~~~~~~

A virtual page of anonymous memory. It may link to a ``vm_page_t`` if it
is resident in memory, or if it has been swapped out, it stores an
identifier by which its contents can be retrieved by the swap pager. It
also stores a reference count used in copy-on-write logic, and a mutex
lock.

It is hoped that ``vm_anon_t``\ s will themselves become pageable in the
future, which will help to dissociate availability of virtual memory
from physical memory; currently availability of virtual memory is
indirectly constrained by available physical memory, because used
virtual memory must be described by ``vm_anon_t``\ s.

``vm_amap_t``
~~~~~~~~~~~~~

A map of anons associated with an anonymous VM object. The map is a
2-level sparse array - the first level is contained in the ``vm_amap_t``
holding pointers to ``vm_amap_chunk_t``\ s called chunks; chunks contain
a certain number (currently 32) of pointers to ``vm_anon_t``\ s.

``vm_object_t``
~~~~~~~~~~~~~~~

These are mappable VM objects. They may be anonymous or vnode objects.
If an anonymous object, it holds a pointer to a ``vm_amap_t``, otherwise
it is a vnode object and contains a handle to the vnode it belongs to
and the head of the RB tree of ``vm_page_t``\ s which store
currently-resident data of the vnode VM object.

``vm_map_t``
~~~~~~~~~~~~

Represents a virtual address space, composed of a splay tree of
``vm_map_entry_t``\ s. There is a special ``kmap`` which is the kernel's
mappings (these are present in all processes, but not
usermode-accessible).

A ``vm_map_entry_t`` is an entry within an address space map. They refer
to a ``vm_object_t`` which they map, and also carry an offset (relative
to which faults in the map entry are processed by the ``vm_object_t``),
a size, and a maximum protection value - this allows for read-only or
non-execute mappings (these are not properties of VM objects
themselves).

Anonymous mappings
------------------

Anonymous mappings supporting copy-on-write semantics are implemented
efficiently with reference-counting. The core principle is that a
``vm_anon_t`` with a reference count greater than 1 is always mapped
read-only, and if there is a write fault at an address which is
represented by that ``vm_anon_t``, it must copy the ``vm_anon_t`` and
its underlying page before mapping it read-write.

todo: as well as the example below, fully detail the logic in an
anonymous fault

Consider a region of anonymous memory newly allocated in a process with
PID 1. There are no ``vm_anon_t``\ s yet because they are lazily
allocated on first fault. PID 1 writes to the first page of region; a
``vm_anon_t`` is allocated with a refcnt of 1. PID 1 also writes to the
2nd page, and the same logic is followed.

Now PID 1 forks into PID 2. PID 2 is given a new anonymous VM object for
that region with a copy of the ``vm_amap_t`` of that of its parent's
equivalent VM object. The copy refers to the same ``vm_anon_t``, but the
copying process has incremented the reference count of the 1st and 2nd
``vm_anon_t`` as they are now referenced by two ``vm_amap_t``\ s. The
copying process has also made all the old writeable mappings of these
pages read-only again.

PID 2 now writes to the 2nd page of the anonymous region. The fault
handler finds the corresponding ``vm_anon_t`` and notices that its
refcnt is 2. As this is a write fault, it must copy the ``vm_anon_t``
and its underlying page. After copying it, it releases its reference to
the ``vm_anon_t`` that was shared with PID 1, and maps the new copied
``vm_anon_t``\ ’s underlying page read-write. The same thing would
happen if PID 1 had tried to do the write.

Anonymous-on-vnode mappings
---------------------------

A special case of mapping is used for ``MAP_PRIVATE`` ``mmap()``'s of a
vnode. An anonymous VM object is created with a parent pointer; the
parent pointer points to the VM object associated with the vnode which
is to be mapped ``MAP_PRIVATE``.

Fault handling for this case is modified with respect to handling for
faults in simple anonymous memory. A read fault will first check for a
``vm_anon_t`` that already exists, but if there is none, it will instead
ask the parent vnode object to map the page for the faulting address
into the faulting process' address space.

In the case of a write fault, the page for the faulting address in the
parent vnode object will be copied into a new page allocated which will
be associated with a ``vm_anon_t`` and placed in the anonymous-on-vnode
object’s ``amap``. This is then mapped read-write into the faulting
process’ address space, and copy-on-write has been achieved.

It should be noted that this is one-directional; that is, once an
anonymous-on-vnode mapping is established, if the vnode object’s pages
are changed by writes into a mapping of that vnode object, then it
doesn’t subject them to copy-on-write (xxx is this clearly written?).
This means that an anonymous-on-vnode mapping's contents, where there
have not been writes (which cause the copy-on-write process), the
content of the pages will change if they change in the parent vnode
object.

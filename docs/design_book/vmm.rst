Virtual Memory Manager
======================

Overview
--------

The virtual memory (or VM) subsystem is the queen and mother of all Keyronex
subsystems.

It provides an implementation of virtual memory supporting all the
features any virtual memory system should have - memory mapped files with page
caching, anonymous memory, virtual copy with copy-on-write optimisation, and
paging - the retrieval from and putting-back to disk of pages, both pages of
memory-mapped files and pages of anonymous memory, which are put back to a
swapfile.

.. note::
	"Paging" and "virtual memory" are sometimes mistakenly used online as
	simple synonyms of "virtual address translation", the process by which a
	virtual memory address is translated to a physical address. This is a
	misapprehension.


The Keyronex virtual memory manager has several responsibilities. Broadly, these
are:

- Page management: the allocation of pages of physical memory, reading contents
  of pages in from their backing store, and putting them back to their backing
  store, according to demand.
- Address space management: maintaining an abstract, machine-independent
  description of virtual memory state, and providing interfaces to alter this.
- Physical mapping: Translating that abstract representation to the form
  demanded by the platform's memory-management unit.

The Keyronex virtual memory manager is principally derived from the design of
NetBSD's UVM, a design that meets these three responsibilities well.

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

.. hint::
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
``vm_map_entry_t``\ s. There is a special ``kmap`` which holds the kernel's
mappings (these are mapped into all processes, but not
usermode-accessible).

A ``vm_map_entry_t`` is an entry within an address space map. They refer
to a ``vm_object_t`` which they map, and also carry an offset (relative
to which faults in the map entry are processed by the ``vm_object_t``),
a size, and a maximum protection value - this allows for read-only or
non-execute mappings (these are not properties of VM objects
themselves).

Page management
---------------

In any good virtual memory system, main memory is treated as the cache of
secondary storage, so the virtual memory manager tries to ensure that a large
amount of memory is used at all times, as unused memory is memory wasted. The
technnique by which this is done in Keyronex is called paging - the movement of
pages of data to and from the backing store with which that page is associated.
Pages are associated with backing store according to which VM object they belong
to; if they belong to a vnode VM object, their backing store is a file or block
device, while if they belong to anonymous memory, their backing store is the
swap space.

.. note::
	In the future Keyronex might support user-defined VM object types as well as
	the built-in vnode and anonymous objects. A custom pager would be required
	for these to carry out page-in and page-out. One use-case would be to allow
	for a single-level store for the Oopsilon programming environment.

Pages are paged-in from their backing store in response to page faults, and
paged out according to a page replacement policy. Keyronex uses a simple global
page replacement algorithm, the FIFO second-chance approach, a variant of the
general category of Not Recently Used page replacement algorithms. This involves
maintaining two queues of pageable pages - active and inactive.

The page daemon
~~~~~~~~~~~~~~~

The page daemon, a kernel thread named `vm_pagedaemon`, is responsible for
maintaining the page replacement policy. It maintains high and low watermarks
for number of free pages and number of inactive pages, and spends most of its
time sleeping on an event.

The event is signalled under two conditions:

- there has been a request to allocate a physical page, but the numer of pages
  on the free queue is less than the low watermark for the free page queue; or
- greater than 75% of main memory is in use, and the number of pages on the
  inactive queue is less than the low watermark for the inactive queue.

The page daemon will wake up and calculate new watermarks for the inactive
queue; these aim to keep around 33% of pageable pages on the inactive queue.
If the number of pages on the inactive queue is less than that of the low
watermark, the page daemon will move pages from the tail of the active queue to
the head of the inactive queue until the inactive queue high water mark is
reached. Pages carry used bits to determine whether they have been accessed or
not; this bit is reset when the page is moved to the inactive queue (this may
involve a TLB shootdown; see the Physical Mapping section).

If the number of free pages is below the free page low watermark, the pagedaemon
will now also take pages from the tail of the inactive queue and check their
used bit. If it is set, the pages get a second chance - they are replaced to
the head of the active queue. Otherwise, they are put back to their backing
store. This is done by invoking the relevant *pager* according to the VM object
to which the page belongs. For vnode VM objects, the vnode pager is used, while
for anonymous VM objects, the swap pager is used.

After the pager has completed the put back to backing store, the page is placed
on the free queue. This process will continue in a loop until the number of
pages on the free page queue reaches the high watermark.

.. todo::
	describe what happens when no pages can be put back to backing store
	anymore, e.g. when pagefile space is exhausted.

Pagers
~~~~~~

.. todo::
	describe the page-busying mechanism; how a page which is being paged in has
	its page allocated and placed in the object, but marked busy, and likewise
	pages being paged out have the same busy bit set, and page faults which
	encounter this bit, they wait on an event and when the event is signalled
	they reattempt the fault.

.. todo::
	describes how the low watermark for free pages is there so that when pagers
	need to run, it's still possible for them to allocate pages if need be.



Anonymous mappings
------------------

Anonymous mappings supporting copy-on-write semantics are implemented
efficiently with reference-counting. The core principle is that a
``vm_anon_t`` with a reference count greater than 1 is always mapped
read-only, and if there is a write fault at an address which is
represented by that ``vm_anon_t``, it must copy the ``vm_anon_t`` and
its underlying page before mapping it read-write.

.. todo::
	as well as the example below, fully detail the logic in an
	anonymous fault?

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

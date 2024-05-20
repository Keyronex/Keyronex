Virtual Memory Manager
======================

Overview
--------

Virtual memory is the centrepiece of Keyronex and so is the subject of a lot of
attention in the design. The goal of the design is to provide the following
features:

 - Portability to many architectures
 - Memory mapped files
 - Shareable anonymous memory
 - Posix fork() support with copy-on-write optimisation
 - Copy-on-write optimisation for private file/anonymous object mappings
 - Paging dynamics in line with Denning's Working Set Model
 - Efficient and coherent cached file I/O
 - Pageability of as much memory as possible, including page tables themselves

The design of the virtual memory manager draws on three distinct traditions:
that of Mach, that of VMS and family (VMS itself, Windows NT, and MINTIA), and
that of NetBSD's UVM. The broad design and the paging dynamics are from the VMS
family, the interfaces and VM object concept are from Mach, and the approach to
forked memory is from NetBSD's UVM (which itself borrowed it from SunOS).

Compromises
-----------

Keyronex VM assumes native support in the architecture for a traditional system
of tree-based page tables, which is true of the 68k, amd64, aarch64, and risc64
architectures - the current and probable future targets of Keyronex. It could
run on systems based on software-refilled TLBs by constructing page tables of
its own format and walking these in the TLB refill trap handler.


Principle of Operation
----------------------

The founding principle of the virtual memory system is that main memory should
act as the cache of secondary memory, with exceptions for certain data that
cannot be paged or where efficiency demands it not be.

Denning's Working Set Model proposes that a **working set** is the set of pages
that a process must have resident in main memory to run without thrashing
(excessive page faults) within a given period of time. As processes' needs
change over the course of their execution, their working set may vary.

To this the concept of **balance set** can be added: a sort of working set of
working sets. The balance set is the set of processes permitted to reside in
main memory over a given period of time.

With these concepts the fundamental goals of the virtual memory manager is made
clear: it should determine how many pages a process needs to run without
thrashing, and permit it to retain those many pages in memory, and it should
adapt to changes in this number over the lifetime of the process. It should
further determine which and how many processes it can keep resident in memory
over a given period of time, such that there is room for each's working set to
be wholly resident.

On that basis, the page replacement approach that the VMM takes is designed to
optimise for this. The algorithm is called Segmented FIFO. It is so-called
because the page cache is segmented into two entities:

Primary Page Cache (aka Balance Set, Working Sets, Resident Set)
    This is composed of per-process queues of pages, called Working Set Queues;
    these pages are those which are currently mapped with valid PTEs in that
    process. These queues are growable and shrinkable, and replacement is by
    FIFO: when the working set queue of a process cannot be grown, the least
    recently mapped page in a process is locally replaced when a new page
    is mapped in that process.

Secondary Page Cache
    This is composed of two queues: the Modified Page Queue and the Standby Page
    Queue. When a page is no longer mapped in any Working Set, it is placed onto
    either the Modified or Standby page queue depending on whether it was
    modified while mapped into a working set. On these queues, the page gets a
    second chance to be brought back into the Primary Page Cache without
    incurring a backing store read.
    It is from these queues that pages actually get replaced on the global
    level, by being written to disk if dirty and made available for reuse for
    new data when clean.

Two specific tasks of the VMM can be inferred from these:

1. To balance the sizes of the primary page cache and secondary page cache to
   minimise unnecessary disk reads.
2. To balance the size of the modified page queue and the standby page queue, as
   only from the standby page queue can pages be immediately repurposed.


Page Frame Database (aka PFN Database, PFNDB)
-----------------------------------------------------

The PFN database describes the current state of each page of main memory. It is
organised as an array at the start of each contiguous region of main memory,
each linked into a queue of regions so that the PFNDB entry corresponding to a
physical address can be quickly determined. Every page of allocatable main
memory is described by an entry.

.. note::
    as an efficiency to eliminate the need to iterate the queue of regions, in
    the future this could become a fixed region of the virtual address space
    which is mapped such that e.g. `pfndb[pfn]` is all it takes to access the
    PFNDB entry for a given page frame number. This would also allow translation
    from PFNDB entry back to PFN trivially, and free up the `pfn` field of PFNDB
    entries.

The total size of an entry in the PFNDB amounts to 32 bytes on 32-bit platforms,
and 64 bytes for 64-bit ports. The data stored is different depending on the use
and state of a page. The fields are:

`pfn`
    The physical page frame number of a page.

`use`
    Tracks what a page is being used for. The uses are Free, Deleted, Anonymous
    Private; Anonymous Forked; Anonymous Shared; File; Page Table Levels 4
    to 1; and Prototype Page Table Levels 4 to 1.

`refcnt`
    This field, together with `use`, determines the state of a page. It is the
    number of wires (reasons to remain resident) on a page and determines
    whether the page can be evicted or freed; the refcount dropping to zero will
    place a page on either the Modified or Standby list depending on whether
    it's dirty, or onto the Free list if the page use has been set to Deleted.

`dirty`
    Whether this page is known to be dirty. It is OR'd into the PFNDB entry at
    the time of a page's eviction from a working set, or may be set explicitly
    on an active page.

`queue_entry`
    Links a page of refcnt 0 either onto the modified, standby, or free page
    queues.

`nonzero_ptes` / `noswap_ptes`
    Used for pages used as page tables. `nonzero_ptes` counts how many actually
    existing PTEs of any kind that there are in a table; if it reaches 0, the
    page table can be freed. The `noswap_ptes` counts how many PTEs there are in
    the table that prevent the table from being paged out to the pagefile, and
    is incremented and decremented together with `refcnt`.

`offset`
    Denotes the offset (in units of page size) into either the virtual address
    space of the process (for private pages), or into the object (for file and
    shared anonymous pages). Shares its location in the PFNDB entry structure
    with the PTE counts, as page table pages don't use this field.

`owner`
    Points to either the process the page is part of the private address space
    of (for private anonymous memory, including page tables), or to the object -
    either a shared anonymous VM object or a file VM object.

`pte_address`
    Points to the physical address of the PTE mapping this page - a PTE in a
    process page table for private memory, a PTE in a prototype page table for
    shared memory, or the `pte` field of a `vmp_anon` for forked memory.

`drumslot`
    Holds the pagefile address that backs this process. This is lazily allocated
    when the page needs to be paged out for the first time, and restored when
    it's paged back in.

Page state is an important concept which depends on the value of `refcnt`,
`dirty`, and `use`. State is determined in this way:

.. code-block::

    If Page.refcnt > 0
        State = Active
    Else If Page.use = Free
        State = Free
    Else If Page.dirty
        State = Modified
    Else
        State = Standby
    Fi

The states mean:

Active
    The page is mapped in at least one working set or has been wired, e.g.
    by an MDL.

Modified
    The page is not validly mapped anywhere, but it is dirty and must be
    flushed to disk. It is on the Modified Page Queue.

Standby
    The page is not validly mapped anywhere and has already been flushed to
    disk (or was never dirtied), so it is free to be reused. It is on the
    Standby Page Queue.

Free
    The page is available for immediate reuse.

Note that a page being written to disk is in the Active state because of the
reference to it held by the paging MDL. A page being read from disk is also in
the Active state, and has a `busy` bit set to indicate this.


Page Tables and PTEs
--------------------

The VMM by relying on the existence of traditional multi-level page tables can
store metadata more optimally. In contrast to Mach-style VMMs, Keyronex VM
uses the native page tables of the architecture to store metadata and does not
treat them as purely caches of more abstract datastructures.

For consistency, the PTE format is also used by abstract datastructures of the
Keyronex VM - when PTEs are used in this way, in locations where they will never
be interpreted by the MMU itself, they are called prototype PTEs. Prototype
PTEs are used to implement shared anonymous, file cache, and forked anonymous
memory.

Page table entries can then be either software or hardware PTEs. A hardware PTE
has the valid bit set, while a software PTE does not. The general format of
software PTEs varies depending on the architecture, but looks roughly like this
on a 32-bit platform:

.. code-block:: c

    enum soft_pte_kind kind: 2;
    uintptr_t   data:   28;
    bool        valid:   2; /* set to invalid */

On 64-bit platforms, the `data` field is instead around 61 bits in length.

There are several kinds of software PTEs:

Busy PTEs
    These indicate a page being read in from backing store.

Transition PTEs
    These are created when a private anonymous page is evicted from a process'
    working set. The `data` field is the PFN number of the anonymous page.

Swap Descriptor PTEs
    These are created when a private anonymous page is paged out at the global
    level, i.e. written to disk and removed from the standby page queue. The
    `data` field is a unique number by which the swapped-out page can be
    retrieved from the pagefile.

Fork PTEs:
    These are created when the Posix fork() operation is carried out. The `data`
    field is a pointer to the `vmp_anon` structure (described later) which holds
    the prototype PTE (again described later). The pointer can fit here because
    `vmp_anon`\ s are always 8-byte aligned, meaning the 3 low bits are always
    zero and can accordingly be shifted away. (If it were necessary to shrink
    the number of bits used for the `data` field even further, we could do so
    by storing the vmp_anon as an offset from the kernel heap base instead; this
    would save yet more bits).

Forked Anonymous and `vmp_anon`\ s
----------------------------------

.. code-block:: c

    pte_t       pte;
    uint32_t    refcnt;

On 32-bit platforms this makes 8 bytes, while on 64-bit platforms padding is
added to extend it from 12 to 16 bytes.

.. todo::
    describe support for fork()

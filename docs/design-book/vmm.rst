Virtual Memory Manager
======================

Overview
--------

Virtual memory is the centrepiece of Keyronex, receiving significant attention
in its design. The goal of that design is to provide the following:

 - Portability to many architectures
 - Memory mapped files
 - Shareable anonymous memory
 - Posix fork() support with copy-on-write optimisation
 - Copy-on-write optimisation for private file/anonymous object mappings
 - Paging dynamics aligned with Denning's Working Set Model
 - Efficient and coherent cached file I/O
 - Maximising the pageability of memory, including page tables themselves

The design of the virtual memory manager draws on three distinct traditions:
that of Mach, that of VMS and family (VMS itself, Windows NT, and MINTIA), and
that of NetBSD's UVM. The broad design and the paging dynamics are from the VMS
family, the interfaces and VM object concept are from Mach, and the approach to
forked memory is from NetBSD's UVM (which itself borrowed it from SunOS).

Compromises
-----------

Keyronex VM assumes native architectural support for traditional tree-based page
tables, as in the 68k, amd64, aarch64, and risc64 architectures - current and
future targets of Keyronex. It could run on systems with software-refilled TLBs
by constructing page tables of its own format and walking these in the TLB
refill trap handler.


Principle of Operation
----------------------

The founding principle of the virtual memory system is that main memory should
act as the cache of secondary memory, with exceptions for certain data that
cannot be paged or where efficiency demands it not be.

Denning's Working Set Model defines a **working set** as the set of pages
that a process needs resident in main memory to run without excessive page
faults (thrashing) over a given period of time. The working set of a process can
vary as its needs change during execution.

The **balance set** extends the working set concept: it is a sort of working set
of working sets. The balance set is the set of processes permitted to reside in
main memory over a given period of time such that all their working sets can
be resident.

Based on these concepts, the fundamental goals of the virtual memory manager
are:

- to determine how many pages a process needs to avoid thrashing, to allow a
  process to retain that many pages in memory, and to adapt to changes over time
- to determine which and how many processes can be coresident in memory
  simultaneously, ensuring their working sets are fully resident.


On that basis, the page replacement approach that the VMM takes is designed to
optimise for this. The algorithm is called Segmented FIFO. It is so-called
because the page cache is segmented into two entities:

Primary Page Cache (aka Balance Set, Working Sets, Resident Set)
    This consists of per-process queues of pages, known as Working Set Queues.
    Pages in the Primary Page Cache are currently mapped with valid PTEs in
    at least one process, or otherwise wired into memory.

    The queues are dynamic, capable of growing and shrinking as needed, but this
    is subject to controls. Replacement within a queue is handled by FIFO:
    when a process's working set queue cannot be expanded, the least recently
    mapped page in that process is replaced when a new page is mapped.

Secondary Page Cache
    This conists of two queues:

    -  Modified Page Queue
    -  Standby Page Queue

    When a page is no longer mapped in any Working Set Queue nor otherwise
    wired, it is placed onto one of these queues, depending on whether it was
    modified while mapped into a working set. These queues give the page a
    second chance to be brought back into the Primary Page Cache without
    incurring a disk read.

    Pages are replaced globally from these queues by being written to disk if
    dirty and made available for reuse when clean.

Two specific tasks of the VMM can be inferred from these:

1. To balance the sizes of the primary page cache and secondary page cache to
   minimise unnecessary disk reads.
2. To balance the size of the modified page queue and the standby page queue,
   because only from the standby page queue can pages be immediately repurposed.


Page Frame Database (aka PFN Database, PFNDB)
-----------------------------------------------------

The PFN database describes the current state of each page of main memory. It is
organised as an array at the start of each contiguous region of main memory,
each linked into a queue of regions so that the PFNDB entry corresponding to a
physical address can be quickly determined. Every page of allocatable main
memory is described by an entry.

.. note::
    As an efficiency to eliminate the need to iterate the queue of regions, in
    the future this could become a fixed region of the virtual address space
    which is mapped such that e.g. `pfndb[pfn]` is all it takes to access the
    PFNDB entry for a given page frame number. This would also allow translation
    from PFNDB entry back to PFN trivially, and free up the `pfn` field of PFNDB
    entries.

The total size of an entry in the PFNDB amounts to 32 bytes on 32-bit platforms,
and 64 bytes for 64-bit ports. The data stored is different depending on the use
and state of a page. The fields are:

`pfn`
    The physical page frame number of the page.

`use`
    Tracks what a page is being used for. The uses are Free, Deleted, Kernel
    Wired, Anonymous Private, Anonymous Forked, Anonymous Shared, File,
    Page Table Levels 4 to 1, and Prototype Page Table Levels 4 to 1.

`refcnt`
    This field, together with `use`, determines the state of a page. It is the
    number of wires (reasons to remain resident) on a page and determines
    whether the page can be evicted or freed; the refcount dropping to zero will
    place a page on either the Modified or Standby list depending on whether
    it's dirty, or onto the Free list if the page use has been set to Deleted.

`dirty`
    Indicates whether the page is known to be dirty. When a page is evicted from
    a working set, the dirty value of the PTE that mapped the page is OR'd into
    this field. It can also be set explicitly while a page is active.

`queue_entry`
    Links a page with a refcnt 0 to either the modified, standby, or free page
    queue.

`nonzero_ptes` / `noswap_ptes`
    Used for pages used as page tables.

    - `nonzero_ptes` counts how many PTEs exist in in a table. When the count
      reaches 0, the page table can be freed.
    - `noswap_ptes` counts PTEs that prevent the table from being paged out to
      the pagefile. Adjustments to this count are always made with the same
      adjustment made to `refcnt`.

`offset`
    Specifies the offset (in units of page size) within the virtual address
    space of a process (for private pages), or within the object (for pages
    belonging to shared VM objects).
    This field shares its location in the structure with the PTE counts above,
    as page table pages do not use this field.

`owner`
    Points to either the process to which the page belongs (for private
    anonymous memory, including page tables), to the VM object (for file or
    shared anonymous pages), or to the `vmp_anon` (for fork anonymous pages).

`pte_address`
    The physical address of the PTE mapping this page. This could be:
    - A PTE in a process page table for private pages
    - A PTE in a prototype page table for shared memory
    - The `pte` field of a `vmp_anon` for forked memory.

`drumslot`
    Stores the pagefile address backing this page. This address is lazily
    allocated when the page is written back for the first time, and restored
    when it is paged back in.

Page state is an important concept determined by the values of `refcnt`,
`dirty`, and `use`:

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
    Mapped in at least one working set or wired (e.g. by an MDL).

Modified
    Not validly mapped, dirty, and must be flushed to disk; on the Modified Page
    Queue.

Standby
    Not validly mapped, already flushed to disk (or never dirtied); on the
    Standby Page Queue.

Free
    The page is available for immediate reuse.

Note that a page being written to disk is in the Active state because of the
reference to it held by the paging MDL. A page being read from disk is also in
the Active state, and has a `busy` bit set to indicate this.

VM Objects
----------

VM Objects are objects that can be mapped into an address space and come in two
types: shared anonymous VM objects and vnode VM objects.

Shared Anonymous VM Objects
    Backed by the page file like private anonymous memory. These are used by the
    tmpfs filesystem to provide space for file contents.

Vnode VM Objects
    Backed by other files (or by block devices). A vnode VM object is allocated
    the first time a file is mapped into memory and then associated with the
    vnode.

Common code handles VM objects where possible, but there are some deviations.
These are described in the later sections on fault handling and page
replacement.


Page Tables and PTEs
--------------------

The VMM optimises storage of metadata by relying on traditional multi-level page
tables. Unlike Mach-style VMMs, Keyronex VM uses the architecture's native page
tables for metadata storage, treating them as first-class entities rather than
simple caches for abstract data structures.

Page tables are classified by the use field in their PFN database entries. PML1
refers to leaf page tables with PTEs pointed to data pages, while PML2 and above
are the higher-level page tables whose PTEs point to lower page tables.

PTE types
^^^^^^^^^

Page table entries can be either software or hardware. A hardware PTE has the
valid bit set, while a software PTE does not. The general format of software
PTEs varies depending on the architecture, but looks roughly like this on a
32-bit platform:

.. code-block:: c

    enum soft_pte_kind kind: 2;
    uintptr_t   data:   28;
    bool        valid:   2; /* set to invalid */

On 64-bit platforms, the `data` field is instead around 61 bits in length.

PTEs can also be zero. The full set of non-zero PTE types, then, are:

Valid (or Hardware) PTEs
    True PTEs as understood by the MMU. They may vary in format depending on
    what level of page table they are entries in.

Busy PTEs
    Indicate a page being read in from backing store.

Transition PTEs
    Created when a page enters the standby or modified state (reference count
    reaches 0). The `data` field holds the PFN of the anonymous page.

Drumslot PTEs
    Created when an anonymous page is paged out at the global level (written to
    a pagefile and removed from the standby page queue). The `data` field holds
    a unique number by which the swapped-out page can be retrieved from a
    pagefile.

Fork PTEs:
    Created during a Posix fork() operation. The `data` field holds a pointer to
    a `vmp_anon` structure (described later), which itself holds a prototype
    PTE.
    The pointer fits into the 28 or 60 available bits because `vmp_anon`\ s are
    always 16-byte aligned, meaning the 4 low bits are always zero and can
    accordingly be shifted away. Further bit savings could be achieved by
    storing vmp_anon as an offset from the kernel heap base.


Prototype Page Tables and VM objects
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Keyronex uses **prototype page tables**, purely-software and never walked by the
MMU, to track memory belonging to shared VM objects. These are always 4-level,
with each level containing 512 entries on 64-bit ports and 1024 on 32-bit ports.

For consistency, the PTE format in prototype page tables is the same as hardware
PTEs - though while hardware PTE formats may vary depending on the level of the
page table they are in, the valid PTE format for prototype page tables is always
the same as that of the hardware PML1. Prototype page tables have the PFNDB
`use` field set to PPML1, PPML2, PPML3, or PPML4.

Page Table Paging
^^^^^^^^^^^^^^^^^

Page table pages, like other anonymous memory, can be paged out to the swapfile.
For a page table page to be eligible for paging out, it must contain no PTEs
other than drumslot or fork PTEs. This condition is reflected by the noswap_ptes
count, updated when PTEs that prevent paging out are created or deleted. The
refcnt is adjusted in tandem with `noswap_ptes`; each PTE making a table
ineligible for paging adds 1 to both counts.

When a page table meets the condition of containing only drumslot or fork PTEs,
the PTE in the parent page table that points to this page table is placed into
the transition state. Typically, the page table's reference count drops to zero
at the same time, causing its PFNDB entry to be linked to the modified page
queue. The page can then be written to the pagefile and potentially replaced.
Replacement converts the PTE pointing to the now-paged-out table from a
transition PTE to a drumslot PTE. This change can make the parent table eligible
for paging out as well.

Prototype page tables for anonymous VM objects also participate in this process.
However, the prototype page tables of vnode VM objects are never paged out.
Paging out can only occur if there are drumslot or fork PTEs in a table but no
valid, transitional, or busy PTEs.
When a vnode VM object page is replaced, the PTE in the PPML1 (the first level
of the prototype page table) referring to it is zeroed. The prototype page
tables of vnode VM objects can therefore only contain valid, busy, or transition
PTEs.
Accordingly, the `noswap_ptes` count of a prototype page table belonging to a
vnode VM object will always match the `nonzero_ptes` count, and when the
`nonzero_ptes` count reaches 0, the prototype page table is destroyed and the
PTE pointing to it is zeroed.

Forked Anonymous and `vmp_anon`\ s
----------------------------------

.. code-block:: c

    pte_t       pte;
    uint32_t    refcnt;

On 32-bit platforms this makes 8 bytes, while on 64-bit platforms padding is
added to extend it from 12 to 16 bytes.

.. todo::
    describe support for fork()

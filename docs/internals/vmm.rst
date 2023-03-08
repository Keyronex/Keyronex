Virtual Memory Manager
======================

Overview
--------

The Keyronex Virtual Memory (or Keyronex VM) service is the queen of all
Keyronex kernel services. It provides memory-mapped files and a page cache,
anonymous memory, virtual copy with copy-on-write optimisation, including
support for the POSIX `fork()` facility, and paging: the dynamic retrieval from
and putting back to backing store of pages of memory, driven by an
implementation of the Working Set Model, an approach to decide on which pages to
bring into memory and which to put back to backing store.


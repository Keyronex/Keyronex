Ideas for a new VMM
===================

Core data structures
 - the VAD rbtree: associated with each process; protects each process' list of
   mappings + its working set + its page tables. Acquired on fault entry, stays
   acquired. **Paged**
   for user processes, **Nonpaged** for kernel process.
 - the vnode object: stores an RB tree of cached pages of a file. Vnode + the
   RB tree entries all **Nonpaged**. The RB tree is not used for tmpfs.
 - the anonymous object: less important than anonymous objects in old keyronex -
   this one's job is to act as the basis for tmpfs files and memfds. Not
   splittable anymore. It will probably be a multi-level table. It and its
   contents to be **Paged** as part of the kernel working set.
 - the working set list - **Nonpaged** for now for simplicity, and in any case
   the WSL is proportionate to resident memory, so this probably doesn't even
   matter.
 - page tables: **Nonpaged** for kernel process, **Paged** for user processes.
   Treated as *process-private* anonymous memory and linearly mapped into the
   process' address space, I suppose.

Mapping e.g. a vnode or an anonymous object into a process with `MAP_PRIVATE`
no longer involves making any seperate objects (?). The process' page tables have
precedence over objects referred to by VADs.

How the devil do we do `fork()`?

Probably something like this:
 - Process 1 calls fork(): fork() proceeds like it did in previous iterations of
   keyronex vmm, i.e. shared mappings are retained, uninherited mappings are
   ditched, but foreach *process-private anonymous page*, which *has been
   faulted into existence*, it replaces it with a special reference into a new
   *forked memory object*. 
 - The *forked memory object* is a bit like a dense array of `vm_anon`s. It has
   a reference count for each page in it, as well as a true prototype PTE.
   The pages can be either in-memory or
   swapped out as per usual. This object is of course **paged**. Special PTEs
   point to it (There needs to be a way to identify such PTEs; probably they can
   be just non-valid bit set + some bit to indicate it's not a paged out page +
   existing within a private-anonymous VAD.)
    - I think we need a way to know that a *valid* but *read-only* PTE which
      was created to map a page pointed to by an entry of the forked memory
      object, and in turn to be able to get back from that to the forked memory
      object entry.
    - If the `vm_page` stores a pointer to the entry within the forked memory
      object then we can do this, I think.
    - When we remove such a page from our working set list, we can simply reset
      it back to a special PTE which points to the forked memory object entry.
 - Fault occurs and we determine it's in a private anonymous memory VAD. We
   check the PTE and find it non-valid, not demand zero, not swap PTE, not
   *absent* (if absent we may be a shadow of a vnode mapping). Now we
   know it points to a forked memory object entry.
 - Note; we can associate with each process a list of forked memory objects, and
   scan all these to determine within whose range of entries a PTE falls. 
 - Note; we can refcnt number of PTEs referring to an entry in a process' list
   of forked memory objects. When that refcnt falls below 0 we can drop that
   entry.

Initial compromises
-------------------

There are potential nasty circuitous dependencies around allocating kernel paged
memory. Large sparse anonymous objects also might pose a problem? I am seeing
*1130 gib* regions regularly allocated on Linux.

Virtual address space required to describe an anonymous object of a given
size, assuming simple linear table, is size of object / 512. So if we allow 1
tib of kernel VA space for this we can have 512 tib of anonymous memory objects.
That ought to be plenty for anyone.

But for tmpfs anonymous objects we really need to think on this carefully. We
can't just allocate the maximum size for them, and can we permit realloc of
them? We don't actually need to put prototype PTEs into a process when we map
one because we can simply figure out where to look at from the VAD. That implies
realloc is ok. But realloc would be damned annoying to implement efficiently for
these large objects. We'd need to manipulate the kernel page tables to do it
effectively, because for even a 1gib object (let's say a tmpfs file) we'd have
to copy *2 mib* if we couldn't simply extend the area!

How *do* we allocate anonymous memory?
 - a VAD is allocated to describe the area
 - that's it!
 - demand zero faults will ensue

 - *Anonymous memory objects* will initially be **nonpageable** and RB tree
   based. We can still page the *pages* of them without excessive pain.
   We'd have their RB tree entries refer either to `vm_page`s or to some kind of
   swap descriptor. Figuring out how to do these effectively will take time.

   We could make the RB tree entries be allocated out of kernel paged memory
   also. We do need to think carefully about how well that would or would not
   work. If an anonymous memory object is mapped into kernel VA space, we can
   end up doing this:
    - lock kproc's vad lock
    - make sure its page tables are brought in
    - determine that we need to look into the VAD's associated anon object
    - page faults galore as we descend the RB tree! (We could perhaps mitigate
      this a bit by, when the mapping is set up in kernel space, )

    So let's just accept that anonymous memory objects' supporting structures
    won't be getting paged themselves any time soon.

    ***reconsideration*** - this really isn't nearly as much of a hassle as I
    first thought. We need a "Pinning page fault" anyway.
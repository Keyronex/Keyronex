As goes anons
- ++ nonswap_ptes for each anon in the page. Pin a page at a time's worth of
anons while doing the fork operation.

- Paging them in - what locks do we need to hold?
    - We'll have to lock the page in memory when we resolve the anon anyway.
    - Can just increment that refcount after we page one in, with the appropriate lock (the kernel's pte state lock? + page queues lock?) held.

- Paging them out?
    - We acquire the page queues lock, ++ refcount on head of standby queue, remove it, unlock
    - Acquire the owner lock - for anons, a hashed PTE state lock? (do anons get two spinlocks like objects might?) - modify the PTE
    - Then do we unlock? Becuase next we need to unpin the refernece on the kernel page table page!
    - I think we can safely unlock here. We are the custodyholders of the nonswap_ptes count on the kernel page.
    - And if the anon is freed - then it won't --nonswap_ptes on the kernel page if the contents of the page is a swap PTE.

In the page fault codepath
- If we have to release our own (userspace!) working set mutex at any point
- Then I think we're in trouble and can't really proceed.
- Why? Because a collided page fault might in theory race to *ELIMINATE* the anon
- (i.e. because refcnt was 1; anon can be freed.)
- A big problem. It seems if we release our working set mutex, we have no choice but to restart the fault.
- In fact, I don't even know that we can continue at all, not even with proceeding to faulting in the page in the system working set
- Because no lock is held that would prevent the anonymous page itself from being abolished.
- Perhaps we just agree: restart entire fault if we have to.

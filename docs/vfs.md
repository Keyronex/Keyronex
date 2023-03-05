VFS
===

The kernel VFS is mostly modeled after SunOS VFS. 

Two public objects are associated with it:
 - the File object - resembles a Unix kernel file descriptor.
 - the Section object - represents a mappable VM object (do we associate it w/ VNode or File?)

One private object is also associated:
 - the VNode object - represents a file, device, socket, in order to maintain Unixy semantics.

Principles:
- filesystems are responsible for providing a useful way to get from something FS-specific to a vnode (in practice will usually be inode numbers since i don't do bizarre and unusual filesystems)
- for most FSes this will be an RB tree mapping inode numbers to FS-specific node structures, each FS-specific node structure having the same lifespan as the underlying vnode.
- i am not initially doing an LRU vnode cache. Solaris did away with it because it complicates the lifespan of a vnode. it is an ill fit to the object manager in keyronex too. instead, vnodes immediately get destroyed when the last ref is released.
- the penalty imposed by having no LRU vnode cache is mitigated, hopefully eliminated altogether, by the vnodes anyway being referenced either by name caches or by being a filesystem root
- the lack of a global LRU vnode cache means no need for a global vnode cache lock that would contend things. it also means i can leave out a mechanism for "collided vnode loads" (i.e. two threads both want the vnode for the inode of /usr/bin/gcc, the first locks the filesystem's vnode cache, finds there is no vnode for that inode, so it unlocks the cache - it has to since a global vnode cache could be queried lower down the device stack- and does the needful to load the vnode; meanwhile thread 2 does the same stuff and also tries to load the vnode, it's a catastrophe, everything falls apart. my lazy interim solution: lock the filesystem's local vnode cache for the entirety of the load-a-vnode operation. in the future i will add a 'transition' bit to the vnode and an associated event to wait on, like how collided page faults are handled)

Changes Mar 5 2023
------------------

Global LRU dropped, chat log bove under 'principles' replaces it.
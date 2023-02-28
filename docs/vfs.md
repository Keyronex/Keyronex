VFS
===

The kernel VFS is mostly modeled after SunOS VFS. 

Two public objects are associated with it:
 - the File object - resembles a Unix kernel file descriptor.
 - the Section object - represents a mappable VM object (do we associate it w/ VNode or File?)

One private object is also associated:
 - the VNode object - represents a file, device, socket, in order to maintain Unixy semantics.

Principles:
 - we have a global VNode cache lock. Why? Because we want to do a global LRU
 ordering and I struggle to think of a way to do it without.
 - filesystems maintain their own (e.g.) inode number to a filesystem node cache
 with the fs nodes pointing to their vnodes. Accessing that cache also goes
 under the global vnode cache lock. Why? Because when a vnode is dropped from
 the global vnode cache, it needs to be able to destroy the associated fs node
 and cause the fs node to be dropped from the tree/hashtable/whatever associating
 e.g. inode numbers to fs nodes.
<img src="docs/scaluxnofont.svg" width=200/>

---

**SCAL/UX**™ - the operating system for those who take scalability seriously

Welcome to the SCAL/UX repository.

***The First Hobbyist's Operating System with Virtual Memory Compression***

SCAL/UX is a Unix-like operating system targeting amd64 PCs. It is internally
structured into three components: the core kernel, DeviceKit, and the POSIX
services. 

The core kernel implements basic primitives: scheduling, virtual memory
management, synchronisation, and simple message-passing. These services are used
to implement a POSIX personality by the POSIX services. DeviceKit implements an
object-oriented driver framework in Objective-C, with adapters to expose
relevant devices to the POSIX personality. All three are implemented in
kernel-space and are logically distinct but currently quite closely coupled.

This is a rewrite of much of the system. **It is not very functional at all
yet**. The previous iteration had a number of working ports (including the BASH
shell and GNU Coreutils) but many components suffered from being written to
accommodate inadequate scheduling, synchronisation, and intercommunication
primitives. This iteration aims to address these flaws. The previous iteration
is in the `old-22-08-07` branch.

Building
--------

The SCAL/UX operating system has a BSD Make based meta-build system loosely
inspired by pkgsrc. You need an existing SCAL/UX toolchain for now and mlibc
headers installed into a sysroot.
Other tools required to build are Meson, xorriso...

To-do
-----

- vm_kernel doesn't do TLB shotodowns for unmaps of kernel wired memory;
  problem?

Third-party components
----------------------

Several third-party components are used. These are some of them:
- mlibc: Provides libc.
- liballoc: Provides one of the in-kernel allocators.
- nanoprintf: used for `kprintf`.
- NetBSD:
  - (`kernel-3/dev/fbterm/nbsdbold.psfu`): Bold8x16 font used for FBTerminal.
  - (`kernel-3/dev/nvmereg.h`): NVMe register definitions.
- Solaris (`kernel-3/dev/fbterm/sun12x22.psfu`): Sun Demi Gallant font available
  for FBTerminal
- ObjFW: provides an Objective-C runtime.
- limine/`limine-terminal-port` (some files in`kernel-3/dev/fbterm/`): used by
  FBTerminal to provide a terminal.
- LUX ACPI Implementation (`kernel-3/dev/acpi/lai`): ACPI implementation from
  Managarm used by Acpi* drivers.
- LZ4 (`kernel-3/libkern/lz4.{c,h}`): Used by VM Compressor to compress pages.
- libuuid (`kernel-3/libkern/uuid*`)
- Linux (`kernel-3/ext2fs/ext2_fs.h`): Ext2 filesystem definitions

Licence
-------

Code original to SCAL/UX is licenced under the Mozilla Public Licence v2.0.
Other components are under their own licences

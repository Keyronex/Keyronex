<img src="docs/keyronexnofont.svg" width=200/>

---

**Keyonex**™ - the key to scalability.

Welcome to the Keyronex repository. Keyronex is an operating system currently
targeting 64-bit PCs, with Amiga 68k and QEMU Virt-68k ports under way.
**It is early in development, and not suitable for use**. Keyronex is a hobby,
and the goals of the projet reflect this.

Features & Goals
----------------


### Platforms

 - [x] 64-bit PC
 - [ ] Amiga 68k (68040/68060 only) - in progress
 - [ ] QEMU Virt-68k (68040/68060 only) - in progress

### Nanokernel

 - [x] SPL - interrupt prioritisation and synchronisation
 - [x] Shared-memory multiprocessing
 - [x] Synchronisation objects: events, message queues, mutexes, semaphores,
   timers
 - [x] Flexible waiting on multiple synchronisation objects
 - [ ] Priority scheduling algorithm
 - [ ] Priority inversion mitigation - will probably be only for mutexes,
 priority inheritance w/ a special API for waiting that does so.

### Device Drivers

 - [x] DeviceKit: Object-oriented driver framework in Objective-C
 - [x] NVMe
 - [x] VirtIO basis - only PCI for now, need virtio-mmio for Virt-68k.
 - [x] VirtIO NIC
 - [x] VirtIO Disk
 - [ ] Generic `bus_space`/`bus_dma`-like framework to abstract access to bus
 space and registers.

### Memory manager

 - [x] High-level management abstracted from low-level MMU details.
 - [x] Virtual copy (copy-on-write) of VM objects
 - [x] Memory-mapped files
 - [ ] Anonymous page swapping and cached page replacement - in progress

### Portable Applications Subsystem

 - [x] Unix-like libc (mlibc)
 - [*] VFS patterned after SunOS, with VMM integration.
 - [*] Efficient TmpFS - uses anonymous VM objects to store data, which are
 `mmap()`'d directly to avoid page duplication.
 - [ ] Page cache with memory window cache.
 - [ ] Networking stack - early in progress, low priority
 - [ ] Unix-domain sockets

Architecture
------------

The Keyronex kernel is internally structured into three components: the
nanokernel, DeviceKit, and the Portable Applications Subsystem:

- The nanokernel implements basic primitives: scheduling, virtual memory
  management, synchronisation, and simple message-passing.
- These services are used to implement a POSIX personality by the Portable
  Applications Subsystem.
- DeviceKit implements an object-oriented driver framework in Objective-C, with
  adapters to expose relevant devices to the Portable Applications Subsystem.

All three are logically but not physically distinct; they are all implemented in
kernel-space, the nanokernel is only minimally dependent on the higher services,
but the Portable Applications Subsystem and DeviceKit interdepend on each other
and on the nanokernel.

The architecture of Keyronex is modelled after several sources of inspiration:
primarily NetBSD, NeXTSTEP (and its Mach kernel), and the VMS/NT tradition
(mostly through [MINTIA](https://github.com/xrarch/mintia)).

Building
--------

For 64-bit PCs:
You need patched binutils and GCC installed into `/opt/x86_64-keyronex'.
Instructions in [docs/toolchain.md]().

Repeating the installation of mlibc headers into the sysroot should be
unnecessary (I hope). If it is necessary, you can do:

`meson --cross-file=amd64.ini --prefix=/usr build`
`ninja -C build`
`DESTDIR=/tmp/keyronex-sysroot ninja -C build install`

Third-party components
----------------------

Several third-party components are used. These are some of them:
 - Limine: Bootloader for 64-bit PCs.
 - mlibc: Provides libc.
 - nanoprintf: used for `kprintf`.
 - FreeBSD: `queue.h`, `tree.h` generic lists and trees
 - LUX ACPI Implementation (`kernel-3/dev/acpi/lai`): ACPI mplementation from
  Managarm used by Acpi* drivers.
 - ObjFW: provides an Objective-C runtime.
 - libuuid (`kernel-3/libkern/uuid*`) - UUID manipulation
<!--
 - liballoc: Provides one of the in-kernel allocators.
 - NetBSD:
  - (`kernel-3/dev/fbterm/nbsdbold.psfu`): Bold8x16 font used for FBConsole.
  - (`kernel-3/dev/nvmereg.h`): NVMe register definitions.
 - Solaris (`kernel-3/dev/fbterm/sun12x22.psfu`): Sun Demi Gallant font available
  for FBConsole
 - limine/`limine-terminal-port` (some files in`kernel-3/dev/fbterm/`) used by
  FBConsole to provide a terminal.
 - LZ4 (`kernel-3/libkern/lz4.{c,h}`): Used by VM Compressor to ompress pages.
- Linux (`kernel-3/ext2fs/ext2_fs.h`): Ext2 filesystem definitions
-->

Licence
-------

Code original to Keyronex is licenced under the Mozilla Public Licence v2.0.
Other components are under their own licences.

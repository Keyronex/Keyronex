![Keyronex Logo](docs/keyronex.svg)

Keyronex is a hobby operating system. It runs to varying degrees on m68k (QEMU
`virt` and Amiga), amd64, and AArch64 PCs. It makes no pretences to be anything
novel or exciting, and doesn't do anything likely to be interesting to anyone.

The long-term goal is to build a fairly competent operating system by the
standards of the early 90s, with as much scalability and as many mod-cons as is
reasonably possible for a single person to implement. The system combines
technical influences from both the Unix tradition (particularly Mach/NeXTSTEP,
NetBSD, and Solaris) with influences from the VMS tradition (particularly
OpenVMS itself, [Mintia], and Windows NT). Special attention has been given to
the virtual memory system.

*[ This is a rewrite  branch which currently lacks features; the previous, more
featured branch is `23-jul`, which features ports of such apps as the GNU
Coreutils, BASH shell, Binutils, and GCC, as well as Xorg and some basic X11
apps such as Twm and Xeyes, and the Links text-mode web browser. ]*


[Mintia]: https://github.com/limnarch/mintia

### Architecture

In kernel mode, a rough distinction can be drawn between the kernel, executive
services, and the driver framework. This is a non-inclusive list of the features
of Keyronex:

- Kernel
  - [x] Interrupt management with Priority Level (IPL) and soft interrupts (DPCs).
  - [x] Waiting/synchronisation objects, including waiting on multiple of these.
  - [x] Remote-Copy-Update (RCU) mechanism.
  - [x] Time management.
  - [x] Process/thread distinction with pre-emptive scheduling of threads.
  - [x] Kernel Ports, an efficient basis for message queues.
  - [ ] (incomplete) Balance set management.

- Virtual Memory
  - [x] Bundy resident page-frame allocator with page stealing.
  - [x] Demand-paged anonymous and file-backed memory.
  - [x] Working set-based local page replacement.
  - [x] Page-out of anonymous and file pages, including page tables themselves.
  - [x] Mapped file cache for coherent cached read/write of files.
  - [ ] (incomplete) Global 2nd-chance page queues with balancing and writeback.

- Executive Services
  - [x] Profoundly asynchronous system of I/O Packet (IOP) message-based I/O.
  - [x] Futexes.
  - [x] Virtual Filesystem Switch.
  - [x] Namecache, including "NullFS" (bind mount) support.
  - [ ] (incomplete) Object-oriented, handle-based userland interface.
  - [ ] (not yet started) File write-behind
  - [ ] (not yet started) IPC mechanism (will be L4 like or Mach like?)

- Drivers & Filesystems
  - [x] Objective-C framework.
  - [x] ACPI-based device discovery for ACPI-supported ports.
  - [x] VirtIO Disk, basic GPU, and 9p port.
  - [x] Intel E1000 NIC driver.
  - [ ] (not yet started) PS/2 keyboard.
  - [ ] (incomplete) Windows "StorPort" driver shim.
  - [ ] (incomplete) FAT, Ext2 filesystems..
  - [ ] (incomplete) 9p filesystem (both VirtIO and TCP transport).

- Miscellaneous Kernel:
  - [ ] (incomplete) TCP/IP stack based on LwIP modified with fine-grained locks.
  - [ ] (incomplete) IOP-based socket framework with async send, receive,
  connect, etc.
  - [ ] (incomplete) Kernel debugging GDB port over UDP.

- POSIX subsystem server:
  - [ ] (not yet started) Processes, process groups, sessions, TTYs, etc.


Third-party components
----------------------

Several third-party components are used. These are some of them:


 - **Limine**: The bootloader used for Keyronx on AArch64 and amd64.
 - **FreeBSD**: `queue.h` and `tree.h`, generic type-safe list/queue and tree
  macros for C.
 - **NetBSD**:
    - The **Boldface** font used on the Amiga port's PAC console.
    - Reference for Amiga port and source of chipset register definition headers
      `cia.h`, `custom.h`.
 - **mlibc**: A portable C standard library. Provides the libc.
 - **nanoprintf**: Printf implementation; provides `kprintf` and family.
 - **uACPI**: The UltraOS ACPI Implementation from UltraOS. Used by the ACPI
   drivers.
 - **LwIP**: Lightweight TCP/IP stack. Adapted parts of the LwIP core provide
   the basis of the TCP/IP stack, while original code implements the socket
   layer.
 - Various headers (mostly BSDs, some Linux): BSD-licenced headers mainly for
 device register definitions.

Licence
-------

Code original to Keyronex is licenced under the Mozilla Public Licence v2.0
(MPLv2).
Other components are under their own licences, all of which are MPL compatible;
these are mostly under the BSD or similar licences.
See the `vendor` and `subprojects` folders where the licences of the third-party
components can be found.

Building
--------

To build Keyronex and all of the userspace you will need the following
dependencies:

```
autopoint
gettext
git
gperf
help2man
libgmp-dev
libmpc-dev
libmpfr-dev
libtool
m4
meson (>= 0.57.0)
pkg-config
python3
python3-mako
python3-pip
texinfo
yacc
xbstrap
xorriso
```

These packages are gotten with `apt install` on Ubuntu, except for `xbstrap`,
which is gotten with `pip install xbstrap`.

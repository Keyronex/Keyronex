![Keyronex Logo](docs/keyronex.svg)

Keyronex is a hobby operating system. It runs to varying degrees on AArch64,
m68k (Amiga and qemu `virt`), and amd64 PCs. It makes no pretences to be
anything novel or exciting, and doesn't do anything likely to be interesting to
anyone.

This is a refactoring branch which currently lacks features; the previous more
featured branch is `23-jul`, which features ports of such apps as the GNU
Coreutils, BASH shell, Binutils, and GCC, as well as Xorg and some basic X11
apps such as Twm and Xeyes, and the Links text-mode web browser.

### Architecture

Keyronex combines influences from the Unix tradition (particularly Mach, NetBSD,
and Solaris) and the VMS tradition (particularly [Mintia], Windows NT, and VMS
itself). The virtual memory system has been given special attention.

[Mintia]: https://github.com/limnarch/mintia

- The kernel: providing multiprocessor scheduling, time management, interrupt
  management synchronisation, waiting, and remote-copy-update (RCU).
- The executive: higher-level services including virtual memory, the virtual
  filesystem switch, and the device framework.
- The drivers: run hardware and virtual devices and carry out I/O on behalf of
  the device framework.

The virtual memory manager is the most well-developed part of Keyronex.
Its design draws on the VM tradition of OpenVMS, Windows NT, and Mintia. It is
not complete in this branch, but already features memory-mapped files coherent
with cached I/O, and paging to swapfile of both anonymous memory and page tables
themselves.
branch it hardly does anything yet; its goal is to implement a VMM based on the
Working Set Model of Denning.

The device framework is also quite well-developed in some respects. It is
profoundly asynchronous, object-oriented, packet-based, and designed to minimise
recursion. Devices are objects organised in a tree and operate by
message-passing. Messages take the form of I/O packets (or IOPs), to which an
explicit stack is attached, and these traverse the device tree through an
iterative, continuation-based mechanism that maintains state in the explicit
stack of the IOP, minimising kernel stack use. Drivers themselves are written in
Objective-C for ease of programming.

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
 - **LwIP**: Lightweight TCP/IP stack. Adapted parts of LwIP provide the basis
   of the TCP/IP stack, while original code
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

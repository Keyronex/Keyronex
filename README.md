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

Several logically distinct components run in kernel mode. These include:

- The kernel: providing multiprocessor scheduling, time management, interrupt
  management synchronisation, waiting, and remote-copy-update (RCU).
- The executive: higher-level services including virtual memory, the virtual
  filesystem switch, and the device manager.
- The drivers: run hardware and virtual devices and carry out I/O on behalf of
  the device manager.

The virtual memory manager is the most well-developed part of Keyronex,
especially with respect to most other hobby OSes.
Its design draws on the VM tradition of OpenVMS, Windows NT, and Mintia. In this
branch it hardly does anything yet; its goal is to implement a VMM based on the
Working Set Model of Denning.

The device manager is also quite well-developed in some respects. It is
profoundly asynchronous, object-oriented, packet-based, and designed to minimise
recursion. Devices are objects organised in a tree and operate by
message-passing. Messages take the form of I/O packets (or IOPs), to which an
explicit stack is attached, and these descend the device tree through an
iterative, continuation-based mechanism that maintains state in the explicit
stack of the IOP, minimising kernel stack use. Drivers themselves are written in
Objective-C for ease of programming.

Requirements
------------

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


<!--
Platform Support
----------------

Keyronex runs only on amd64 for now. Drivers are mostly for virtual hardware.
These include:
 - Disk controllers: VirtIO-Disk
 - Disk/other: VirtIO-FS and VirtIO-9p
 - NICs: VirtIO-NIC
 - Filesystems: TmpFS, FUSE, 9p (9p2000.L only for now).
-->

Third-party components
----------------------

Several third-party components are used. These are some of them:


 - **Limine**: The bootloader used for Keyronx on AArch64 and amd64.
 - **FreeBSD**: `queue.h` and `tree.h`, generic type-safe list/queue and tree
  macros for C.
 - **nanoprintf**: Minimal printf() implementation used for kprintf.
 - **NetBSD**:
    - The **Boldface** font used on the Amiga port's PAC console.
    - Reference for Amiga port and source of chipset register definition headers
      `cia.h`, `custom.h`.
 - **Limine**: A bootloader for PCs and UEFI-based AArch64 systems. Keyronex's
  default bootloader for the amd64 and aarch64 port. It also provides a terminal
  which is used as a library to provide Keyronex's FBConsole graphical console.
 - **mlibc**: A portable C standard library. Provides the libc.
 - **nanoprintf**: Printf implementation; provides `kprintf` and family.
 - **uACPI**: The UltraOS ACPI Implementation from UltraOS. Used by the ACPI
   drivers.
 - Various headers (mostly BSDs, some Linux): BSD-licenced headers for device
   registers definitions.
<!--
 - NetBSD:
  - (`kernel-3/dev/fbterm/nbsdbold.psfu`): Bold8x16 font used for FBTerminal.
  - (`kernel-3/dev/nvmereg.h`): NVMe register definitions.
 - Solaris (`kernel-3/dev/fbterm/sun12x22.psfu`): Sun Demi Gallant font available
  for FBTerminal
 - LZ4 (`kernel-3/libkern/lz4.{c,h}`): Used by VM Compressor to ompress pages.
- Linux (`kernel-3/ext2fs/ext2_fs.h`): Ext2 filesystem definitions
-->

Licence
-------

Code original to Keyronex is licenced under the Mozilla Public Licence v2.0
(MPLv2).
Other components are under their own licences, all of which are MPL compatible;
these are mostly under the BSD or similar licences.
See the `vendor` and `subprojects` folders where the licences of the third-party
components can be found.

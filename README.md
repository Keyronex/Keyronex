![Keyronex Logo](docs/keyronex.svg)

Keyronex is an operating system for 64-bit IBM-compatible PCs and their
emulators. Some preliminary work towards Amiga 68k and Virt68k ports has also
been done. It is purely a hobbyist system and makes no pretences to be anything
else.

It doesn't do anything useful yet.

#### Architecture

Keyronex combines influences from the Unix tradition (particularly Mach, NetBSD,
and Solaris) and the VMS tradition (particularly [Mintia], Windows NT, and VMS
itself). The virtual memory system has been given special attention.

[Mintia]: https://github.com/limnarch/mintia

Several logically distinct components run in kernel mode. These include:

- The kernel: providing multiprocessor scheduling, time management,
  synchronisation, waiting, and a simple message-passing mechanism.
- The executive: higher-level services including virtual memory, the virtual
  filesystem switch, and the device manager.
- The drivers: run hardware and virtual devices and carry out I/O on behalf of
  the device manager.
- The portable applications subsytem: provides those parts of the emulation of 
  the POSIX API in terms of Keyronex native APIs  which cannot be emulated in
  userland.

The virtual memory manager is the most well-developed part of Keyronex,
especially with respect to most other hobby OSes.
It adopts the working set model and provides memory
mapped files and anonymous memory with page replacement and swapping to disk.
In the future it will hopefully be able to page not just anonymous and
mapped-file pages themselves, but also the structures describing these, so that
virtual memory availability is more completely dissociated from physical memory
availability.

The device manager is also quite well-developed in some respects. It is
profoundly asynchronous, packet-based, and designed to minimise recursion by
dispatching I/O packets, to which an explicit stack is attached, through a
continuation mechanism.

Third-party components
----------------------

Several third-party components are used. These are some of them:
 - Limine: Bootloader for 64-bit PCs.
 - mlibc: Provides libc.
 - nanoprintf: used for `kprintf`.
 - FreeBSD: `queue.h`, `tree.h` generic lists and trees
 - LUX ACPI Implementation: ACPI implementation from
  Managarm used by the Acpi drivers.
 - libuuid- UUID manipulation
 - Various (mostly BSDs, some Linux): BSD-licenced headers for device registers
 and suchlike.
<!--
 - liballoc: Provides one of the in-kernel allocators.
 - NetBSD:
  - (`kernel-3/dev/fbterm/nbsdbold.psfu`): Bold8x16 font used for FBTerminal.
  - (`kernel-3/dev/nvmereg.h`): NVMe register definitions.
 - Solaris (`kernel-3/dev/fbterm/sun12x22.psfu`): Sun Demi Gallant font available
  for FBTerminal
 - limine/`limine-terminal-port` (some files in`kernel-3/dev/fbterm/`) used by
  FBTerminal to provide a terminal.
 - LZ4 (`kernel-3/libkern/lz4.{c,h}`): Used by VM Compressor to ompress pages.
- Linux (`kernel-3/ext2fs/ext2_fs.h`): Ext2 filesystem definitions
-->

Licence
-------

Code original to Keyronex is licenced under the Mozilla Public Licence v2.0.
Other components are under their own licences.

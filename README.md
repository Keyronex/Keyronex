<img src="docs/scaluxnofont.svg" width=200/>

---

**Keyonex**™ - the key to scalability.

Welcome to the Keyronex repository. Keyronex is an operating system currently
targeting amd64 PCs. It is early in development, so much of what follows is just
the plan. The Keyronix kernel is internally structured into three components:
the core kernel, DeviceKit, and the POSIX services.

The core kernel implements basic primitives: scheduling, virtual memory
management, synchronisation, and simple message-passing. These services are used
to implement a POSIX personality by the POSIX services. DeviceKit implements an
object-oriented driver framework in Objective-C, with adapters to expose
relevant devices to the POSIX personality. All three are implemented in
kernel-space and are logically distinct but currently quite closely coupled.

Within the core kernel, the basic scheduling services (located under `kern`) are
logically distinct and avoid depending on the higher-level facilities.

Building
--------

You need patched binutils and GCC installed into `/opt/x86_64-keyronex'.
Instructions in [docs/toolchain.md](). TODO: explain further.


Third-party components
----------------------

Several third-party components are used. These are some of them:
- mlibc: Provides libc.
- nanoprintf: used for `kprintf`.

<!--
 - liballoc: Provides one of the in-kernel allocators.
 - NetBSD:
  - (`kernel-3/dev/fbterm/nbsdbold.psfu`): Bold8x16 font used for FBTerminal.
  - (`kernel-3/dev/nvmereg.h`): NVMe register definitions.
 - Solaris (`kernel-3/dev/fbterm/sun12x22.psfu`): Sun Demi Gallant font available
  for FBTerminal
 - ObjFW: provides an Objective-C runtime.
 - limine/`limine-terminal-port` (some files in`kernel-3/dev/fbterm/`) used by
  FBTerminal to provide a terminal.
 - LUX ACPI Implementation (`kernel-3/dev/acpi/lai`): ACPI mplementation from
  Managarm used by Acpi* drivers.
 - LZ4 (`kernel-3/libkern/lz4.{c,h}`): Used by VM Compressor to ompress pages.
 - libuuid (`kernel-3/libkern/uuid*`)
- Linux (`kernel-3/ext2fs/ext2_fs.h`): Ext2 filesystem definitions
-->

Licence
-------

Code original to Keyronex is licenced under the Mozilla Public Licence v2.0.
Other components are under their own licences

Supported Platforms
===================

ACPI 64-bit PC
--------------

Fully supported.

ACPI AArch64
------------

Mostly tested under QEMU KVM. Userland code execution triggers synchronous
exceptions with ESR=0x2000000 regularly; despite what appears to be adequate
instruction cache maintenance, I have not been able to get rid of this, so there
is a workaround in place (clearing the TLB for the faulting SEPC and returning).


ACPI RISC-V 64
--------------

No SMP, not tested on real hardware.

Amiga
-----

Not yet in the codebase.

QEMU Virt m68k
--------------

Fully supported.

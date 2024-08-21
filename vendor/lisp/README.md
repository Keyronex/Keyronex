# Limine (boot protocol) In Silly Places

This is a boot shim for the qemu m68k 'virt' machine, enabling limine protocol kernels to run on this platform. Please note that this project is entirely separate to the limine bootloader and mainstream limine protocol - if you're using this shim please dont ask for help upstream or bother actual limine devs.

## Changes to Original LBP Specification

- All pointers are now machine-word sized, rather than always 64-bit. This means for 32-bit platforms (m68k) pointers are 32-bits wide. While this was not *necessary*, it is convinient for the kernel and loader when iterating through arrays of pointers, like the memory map.
- The kernel is still requried to be in the upper 2G of virtual memory, however it's recommended the kernel lives as high as possible.
- The kernel **must** be a 32-bit elf.

It's also worth noting that LBP requests in the kernel must still be 8-byte aligned. This won't happen naturally with the default m68k ABI, so be sure to do this yourself (with `alignas(8)` or `__attribute__((aligned(8)))`).

### SMP Feature

The loader does not support SMP, but this feature still has a defined response. For now it will always return info about exactly one processor.

### Paging Mode Feature

The following values are added for m68k:

```c
#define LIMINE_PAGING_MODE_M68K_4K 0
#define LIMINE_PAGING_MODE_M68K_8K 1
```

No flags are currently defined. The default value (if no request is present) is `LIMINE_PAGING_MODE_M68K_4K`.

## Usage

Building the shim requires GNU make and a cross compiler targetting `m68k-elf`. Both GCC and clang can be used, clang will need to be passed the `--target=m68k-elf` option.

After cloning this repository, open the root makefile for editing. You'll need to populate the `KERNEL_BLOB` field with the path to your kernel, as the kernel is embedded into the loader at compile-time. You will also need to fill in the cross compiler options (`X_CXX_BIN` for the C++ compiler, `X_LD_BIN` for the linker and `X_AS_BIN` for the assembler). If you're using GCC, point these to your cross compiler binaries, if you're using clang you can simply write `clang --target=m68k-elf`.

You'll also need to run `make bootstrap` to download some dependencies. This can also be re-run at anytime to fetch updated versions.

At this point, you should be ready to execute `make`/`make all` to build the loader. If all goes well, you will find a file at `loader/build/loader-m68k.elf` ready to pass to qemu via the `-kernel` flag.

## Future Plans

- Actually implement support for 8K pages, right now this does nothing.

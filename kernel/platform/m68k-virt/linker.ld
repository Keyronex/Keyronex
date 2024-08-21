/* Tell the linker that we want an x86_64 ELF64 output file */
OUTPUT_FORMAT(elf32-m68k)

/* We want the symbol _start to be our entry point */
ENTRY(_start)

/* Define the program headers we want so the bootloader gives us the right */
/* MMU permissions */
PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata  PT_LOAD    FLAGS((1 << 2)) ;            /* Read only */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ; /* Write + Read */
    dynamic PT_DYNAMIC FLAGS((1 << 1) | (1 << 2)) ; /* Dynamic PHDR for relocations */
}

SECTIONS
{
    /* We wanna be placed in the topmost 2GiB of the address space, for optimisations */
    /* and because that is what the Limine spec mandates. */
    /* Any address in this region will do, but often 0xffffffff80000000 is chosen as */
    /* that is the beginning of the region. */
    . = 0xfd000000;

    TEXT_SEGMENT_START = .;
    .text : {
        *(.text .text.*)
    } :text
    TEXT_SEGMENT_END = .;

    /* Move to the next memory page for .rodata */
    . += CONSTANT(MAXPAGESIZE);

    RODATA_SEGMENT_START = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    .eh_frame_hdr : { *(.eh_frame_hdr) }
    .eh_frame : { *(.eh_frame) }

    .init_array : {
        PROVIDE (init_array_start = .);
        KEEP (*(SORT (.init_array.*)))
        KEEP (*(.init_array))
        PROVIDE (init_array_end = .);
    }
    RODATA_SEGMENT_END = .;

    /* Move to the next memory page for .data */
    . += CONSTANT(MAXPAGESIZE);

    DATA_SEGMENT_START = .;
    .data : {
        *(.data .data.*)
    } :data

    /* Dynamic section for relocations, both in its own PHDR and inside data PHDR */
    .dynamic : {
        *(.dynamic)
    } :data :dynamic

    /* NOTE: .bss needs to be the last thing mapped to :data, otherwise lots of */
    /* unnecessary zeros will be written to the binary. */
    /* If you need, for example, .init_array and .fini_array, those should be placed */
    /* above this. */
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    KDATA_SEGMENT_END = .;

    /* Discard .note.* and .eh_frame since they may cause issues on some hosts. */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
    }
}

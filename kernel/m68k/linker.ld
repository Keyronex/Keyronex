/* Tell the linker that we want an m68k ELF32 output file */
OUTPUT_FORMAT(elf32-m68k)

/* We want the symbol _start to be our entry point */
ENTRY(_start)

/* Define the program headers we want so the bootloader gives us the right */
/* MMU permissions; this also allows us to exert more control over the linking */
/* process. */
PHDRS
{
	headers PT_PHDR PHDRS;
	text	PT_LOAD FILEHDR PHDRS;
	rodata	PT_LOAD;
	data	PT_LOAD;
	dynamic PT_DYNAMIC;
}

SECTIONS
{
	/* We want to be placed in the topmost 2GiB of the address space, for optimisations */
	/* and because that is what the Limine spec mandates. */
	/* Any address in this region will do, but often 0xffffffff80000000 is chosen as */
	/* that is the beginning of the region. */
	/* Additionally, leave space for the ELF headers by adding SIZEOF_HEADERS to the */
	/* base load address. */
	. = 0xfd000000 + SIZEOF_HEADERS;

	TEXT_SEGMENT_START = .;
	.text : {
		*(.text .text.*)
	} :text
	TEXT_SEGMENT_END = .;

	/* Move to the next memory page for .rodata */
	. = ALIGN(CONSTANT(MAXPAGESIZE));

	RODATA_SEGMENT_START = .;
	.rodata : {
		*(.rodata .rodata.*)
	} :rodata

	.init_array : {
		PROVIDE (init_array_start = .);
		KEEP (*(SORT (.init_array.*)))
		KEEP (*(.init_array))
		PROVIDE (init_array_end = .);
	}

	.eh_frame_hdr : { *(.eh_frame_hdr) }
	.eh_frame : { *(.eh_frame) }
	RODATA_SEGMENT_END = .;

	/* Move to the next memory page for .data */
	. = ALIGN(CONSTANT(MAXPAGESIZE));

	DATA_SEGMENT_START = .;
	.data : {
		*(.data .data.*)

		/* Place the sections that contain the Limine requests as part of the .data */
		/* output section. */
		KEEP(*(.requests_start_marker))
		KEEP(*(.requests))
		KEEP(*(.requests_end_marker))
	} :data

	/* Dynamic section for relocations, both in its own PHDR and inside data PHDR. */
	.dynamic : {
		*(.dynamic)
	} :data :dynamic

	.cpu_local : {
		PCPU_SEGMENT_START = .;
		*(.cpu_local_fixed)
		*(.cpu_local_dynamic)
		PCPU_SEGMENT_END = .;
	}

	/* NOTE: .bss needs to be the last thing mapped to :data, otherwise lots of */
	/* unnecessary zeros will be written to the binary. */
	/* If you need, for example, .init_array and .fini_array, those should be placed */
	/* above this. */
	.bss : {
		*(.bss .bss.*)
		*(COMMON)
	} :data

	KDATA_SEGMENT_END = .;

	/* Discard .note.* and .eh_frame* since they may cause issues on some hosts. */
	/* Also discard the program interpreter section since we do not need one. This is */
	/* more or less equivalent to the --no-dynamic-linker linker flag, except that it */
	/* works with ld.gold. */
	/DISCARD/ : {
		*(.note .note.*)
		*(.interp)
	}
}

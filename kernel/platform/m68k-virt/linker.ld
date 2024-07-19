ENTRY (_start)

SECTIONS
{
    . = 0xfd000000;
	.text ALIGN (4K) : AT (ADDR (.text))
	{
		*(.text)
	}

	.rodata ALIGN (4K) : AT (ADDR (.rodata))
	{
		*(.rodata)
	}

	.init_array : {
		PROVIDE (init_array_start = .);
		KEEP (*(SORT (.init_array.*)))
		KEEP (*(.init_array))
		PROVIDE (init_array_end = .);
	}

	.eh_frame_hdr : { *(.eh_frame_hdr) }
	.eh_frame : { *(.eh_frame) }

	.data ALIGN (4K) : AT (ADDR (.data))
	{
		*(.data)
	}

	.bss ALIGN (4K) : AT (ADDR (.bss))
	{
		*(COMMON)
		*(.bss)
	}

    . = ALIGN(32 / 8);
    _bss_end = .;

    /* Align the end to ensure anything after the kernel ends up on its own pages */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    _end = .;

    /* Strip unnecessary stuff */
    /DISCARD/ : { *(.comment .note ) }
}

ENTRY (_start)

SECTIONS
{
    . = 0x1000;
	.text ALIGN (4K) : AT (ADDR (.text))
	{
		*(.text)
	}

	.rodata ALIGN (4K) : AT (ADDR (.rodata))
	{
		*(.rodata)
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

    . = ALIGN(2);
    _bss_end = .;

    /* Strip unnecessary stuff */
    /DISCARD/ : { *(.comment .note ) }
}

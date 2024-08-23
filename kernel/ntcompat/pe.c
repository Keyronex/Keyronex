#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "ntcompat/ntcompat.h"
#include "ntcompat/win_image.h"
#include "vm/vmp.h"

static void NTAPI unresolved_shim(...)
{
	kfatal("Unresolved import\n");
}

static void *
find_func(const char *module, const char *symbol)
{
	image_export_t *table = NULL;

	if (strcmp(module, "NTOSKRNL.exe") == 0 ||
	    strcmp(module, "ntoskrnl.exe") == 0)
		table = ntoskrnl_exports;
	else if (strcmp(module, "storport.sys") == 0)
		table = storport_exports;

	if (!table)
		return NULL;

	while (table->func != NULL) {
		if (strcmp(table->name, symbol) == 0)
			return table->func;
		table++;
	}
	return NULL;
}

void
pe_load(const char *path, void *addr)
{
	kprintf("ntcompat_load_driver: loading %s\n", path);
	vaddr_t filebase;
	vaddr_t base;

	filebase = (vaddr_t)addr;

	PIMAGE_DOS_HEADER pdoshdr = (PIMAGE_DOS_HEADER)addr;
	kassert(pdoshdr->e_magic == IMAGE_DOS_SIGNATURE);

	PIMAGE_NT_HEADERS pnthdr = (PIMAGE_NT_HEADERS)((char *)addr +
	    pdoshdr->e_lfanew);
	kassert(pnthdr->Signature == IMAGE_NT_SIGNATURE);

	size_t pages = ROUNDUP(pnthdr->OptionalHeader.SizeOfImage, PGSIZE) /
	    PGSIZE;
	base = vm_kalloc(pages, 0);
	memset((void *)base, 0x0, pages * PGSIZE);

	PIMAGE_SECTION_HEADER sechdr = IMAGE_FIRST_SECTION(pnthdr);

	/* load sections */
	for (size_t i = 0; i < pnthdr->FileHeader.NumberOfSections;
	     i++, sechdr++) {
		void *dest = (void *)(base + sechdr->VirtualAddress);
		void *src = (void *)(filebase + sechdr->PointerToRawData);
#ifdef DEBUG_PE
		kprintf("Copying section %s (base %p; file 0x%x)\n",
		    sechdr->Name, dest, sechdr->PointerToRawData);
#endif
		memcpy(dest, src, sechdr->SizeOfRawData);
	}

	size_t delta = base - pnthdr->OptionalHeader.ImageBase;
	kprintf("Base: 0x%lx - Delta is %lu\n", base, delta);

	/* relocate */
	IMAGE_DATA_DIRECTORY *dir =
	    &pnthdr->OptionalHeader
		 .DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
	if (dir->Size > 0) {
		PIMAGE_BASE_RELOCATION reloc = (PIMAGE_BASE_RELOCATION)(base +
		    dir->VirtualAddress);
		while (reloc->VirtualAddress > 0) {
			uint8_t *dest = (uint8_t *)(base +
			    reloc->VirtualAddress);
			uint16_t *rel = (uint16_t *)((char *)reloc +
			    IMAGE_SIZEOF_BASE_RELOCATION);
			size_t nrels = (reloc->SizeOfBlock -
					   IMAGE_SIZEOF_BASE_RELOCATION) /
			    2;

			for (size_t i = 0; i < nrels; i++, rel++) {
				int type = *rel >> 12;
				int offset = *rel & 0xfff;

				switch (type) {
				case IMAGE_REL_BASED_ABSOLUTE:
					break;

				case IMAGE_REL_BASED_DIR64: {
					uint64_t *addr = (uint64_t *)(dest +
					    offset);
					*addr += delta;
					break;
				}

				default:
					kfatal("Unhandled relocation type\n");
				}
			}

			reloc = (PIMAGE_BASE_RELOCATION)(((char *)reloc) +
			    reloc->SizeOfBlock);
		}
	}

	/* import tables */
	IMAGE_DATA_DIRECTORY *import_dir =
	    &pnthdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (import_dir->Size > 0) {
		PIMAGE_IMPORT_DESCRIPTOR import =
		    (PIMAGE_IMPORT_DESCRIPTOR)((char *)base +
			import_dir->VirtualAddress);

		while (import->Characteristics) {
			const char *modname = (const char *)(base +
			    import->Name);
#ifdef DEBUG_PE
			kprintf("Import %s\n", modname);
#endif

			uint64_t *thunk, *func;
			int result = 1;

			if (import->OriginalFirstThunk) {
				thunk = (uint64_t *)(base +
				    import->OriginalFirstThunk);
				func = (uint64_t *)(base + import->FirstThunk);
			} else {
				thunk = (uint64_t *)(base + import->FirstThunk);
				func = (uint64_t *)(base + import->FirstThunk);
			}
			for (; *thunk; thunk++, func++) {
				PIMAGE_IMPORT_BY_NAME thunkData =
				    (PIMAGE_IMPORT_BY_NAME)(base + (*thunk));
				void *resolved = find_func(modname,
				    (const char *)thunkData->Name);

#ifdef DEBUG_PE
				kprintf("\tName: %s (%p)\n", thunkData->Name,
				    resolved);
#endif

				if (!resolved) {
					kprintf("\t%s!%s not resolved\n",
					    modname, thunkData->Name);
					resolved = unresolved_shim;
				}

				*func = (uint64_t)resolved;
			}

			if (!result) {
				kfatal("Bad result\n");
			}

			import++;
		}
	}

	/* security cookie */
	IMAGE_DATA_DIRECTORY *loadcfg_dir =
	    &pnthdr->OptionalHeader
		 .DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
	if (loadcfg_dir->Size > 0) {
		PIMAGE_LOAD_CONFIG_DIRECTORY loadcfg = (void *)((char *)base +
		    loadcfg_dir->VirtualAddress);

		if (loadcfg->SecurityCookie != 0)
			*(void **)loadcfg->SecurityCookie = (void *)0x12345678;
	}

	kprintf("ntcompat_load_driver: calling DriverEntry (%x)\n",
	    pnthdr->OptionalHeader.AddressOfEntryPoint);

	driver_entry_fn_t entry = (void *)(base +
	    pnthdr->OptionalHeader.AddressOfEntryPoint);

	nt_driver_object_t *driver = kmem_alloc(
	    sizeof(nt_driver_object_t) + sizeof(struct pci_match) * 2);
#if 1
	driver->name = "viostor";
	driver->matches[0].vendor_id = 0x1af4;
	driver->matches[0].device_id = 0x1004; // 1001 blk, 1004 scsi
#elif 0
	driver->name = "storahci";
	driver->matches[0].vendor_id = 0x8086;
	driver->matches[0].device_id = 0x2922;
#else
	driver->name = "nvme";
	driver->matches[0].vendor_id = 0x1b36;
	driver->matches[0].device_id = 0x0010;
#endif

	driver->matches[1].vendor_id = 0x0;
	driver->matches[1].device_id = 0x0;

	entry(driver, (void *)0x1);
}

int vmp_enter_kwired(vaddr_t virt, paddr_t phys);

void
ntcompat_init(void)
{
	vm_page_t *user_page;
	ipl_t ipl;

	vm_page_alloc(&user_page, 0, kPageUseKWired, 1);
	ipl = vmp_acquire_pfn_lock();
	vmp_enter_kwired(0xfffff78000000000, vm_page_paddr(user_page));
	vmp_release_pfn_lock(ipl);
}

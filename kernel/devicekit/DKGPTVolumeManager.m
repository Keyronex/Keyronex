#include <devicekit/DKGPTVolumeManager.h>
#include <kern/kmem.h>
#include <libkern/uuid.h>

struct gpt_header {
	char	 signature[8];
	uint32_t revision;
	uint32_t size;
	uint32_t cksumHeader;
	uint32_t reserved;
	uint64_t lbaHeader;
	uint64_t lbaAltHeader;
	uint64_t lbaFirstUsable;
	uint64_t lbaLastUsable;
	uuid_t	 guid;
	uint64_t lbaEntryArrayStart;
	uint32_t nEntries;
	uint32_t sizEntry;
	uint32_t cksumPart;
	uint32_t reserved2;
};

struct gpt_entry {
	uuid_t	 type;
	uuid_t	 identifier;
	uint64_t lbaStart;
	uint64_t lbaEnd;
	uint64_t attributes;
	uint16_t name[36]; /* UCS-2 */
};

@interface
GPTVolumeManager (Private)

@end

@implementation GPTVolumeManager

+ (BOOL)probe:(DKLogicalDisk *)disk
{
	vm_mdl_t	 *mdl;
	struct gpt_header hdrGpt;
	blksize_t	  blockSize;
	int		  r;

	r = vm_mdl_new_with_capacity(&mdl, sizeof(struct gpt_header));
	if (r != 0)
		return NO;

	blockSize = [disk blockSize];

	r = [disk readBytes:blockSize
			 at:blockSize * 1
		 intoBuffer:mdl
		 completion:NULL];
	assert(r >= 0);

	memcpy(&hdrGpt, P2V(mdl->pages[0]->paddr), sizeof(hdrGpt));
	if (memcmp(hdrGpt.signature, "EFI PART", 8) != 0) {
		kprintf("Not a GPT disk\n");
		return NO;
	}

	if ([[self alloc] initWithDisk:disk mdl:mdl header:&hdrGpt] != NULL)
		return YES;

	return NO;
}

- initWithDisk:(DKLogicalDisk *)disk
	   mdl:(vm_mdl_t *)mdl
	header:(struct gpt_header *)hdrGpt
{
	int	  r;
	size_t	  sizGptEntryArray;
	blksize_t blockSize;

	self = [super init];
	if (self) {
		[self initWithProvider:disk];
		kmem_asprintf(&m_name, "GPTVolumeManager");
		[self registerDevice];
		DKLogAttach(self);

		blockSize = [disk blockSize];

		sizGptEntryArray = hdrGpt->sizEntry * hdrGpt->nEntries;
		if (sizGptEntryArray > vm_mdl_capacity(mdl)) {
			r = vm_mdl_expand(&mdl, sizGptEntryArray);
			assert(r >= 0);
		}

		r = [disk readBytes:sizGptEntryArray
				 at:blockSize * hdrGpt->lbaEntryArrayStart
			 intoBuffer:mdl
			 completion:NULL];
		assert(r >= 0);

		for (int i = 0; i < hdrGpt->nEntries; i++) {
			struct gpt_entry ent;
			char	       parttype[UUID_STRING_LENGTH + 1] = { 0 };
			char	       partname[37];
			DKLogicalDisk *ld;

			vm_mdl_copy(mdl, &ent, sizeof(ent),
			    hdrGpt->sizEntry * i);

			if (uuid_is_null(ent.type))
				continue;

			/** TODO(low): handle UTF-16 properly */
			for (int i = 0; i < 36; i++)
				partname[i] = ent.name[i] > 127 ? '?' :
								  ent.name[i];
			uuid_unparse(ent.type, parttype);

			ld = [[DKLogicalDisk alloc]
			    initWithUnderlyingDisk:disk
					      base:blockSize * ent.lbaStart
					      size:(ent.lbaEnd - ent.lbaStart) *
					      blockSize
					      name:partname
					  location:i + 1
					  provider:self];

			(void)ld; /* TODO: keep track of it? */
		}
	}
	return self;
}

@end

#ifndef KRX_DEV_DOSFS_VAR_H
#define KRX_DEV_DOSFS_VAR_H

#include "safe_endian.h"

struct __attribute__((packed)) dos_bpb {
	uint8_t BS_jmpBoot[3];
	char BS_OEMName[8];
	leu16_t BPB_BytsPerSec;
	uint8_t BPB_SecPerClus;
	leu16_t BPB_RsvdSecCnt;
	uint8_t BPB_NumFATs;
	leu16_t BPB_RootEntCnt;
	leu16_t BPB_TotSec16;
	uint8_t BPB_Media;
	leu16_t BPB_FATSz16;
	leu16_t BPB_SecPerTrk;
	leu16_t BPB_NumHeads;
	leu32_t BPB_HiddSec;
	leu32_t BPB_TotSec32;
	union __attribute__((packed)) {
		struct __attribute__((packed)) dos_bpb_16 {
			uint8_t BS_DrvNum;
			uint8_t BS_Reserved1;
			uint8_t BS_BootSig;
			leu32_t BS_VolID;
			char BS_VolLab[11];
			char BS_FilSysType[8];
		} bpb_16;
		struct __attribute__((packed)) dos_bpb_32 {
			leu32_t BPB_FATSz32;
			leu16_t BPB_ExtFlags;
			leu16_t BPB_FSVer;
			leu32_t BPB_RootClus;
			leu16_t BPB_FSInfo;
			leu16_t BPB_BkBootSec;
			uint8_t BPB_Reserved[12];

			uint8_t BS_DrvNum;
			uint8_t BS_Reserved1;
			uint8_t BS_BootSig;
			leu32_t BS_VolID;
			uint8_t BS_VolLab[11];
			uint8_t BS_FilSysType[8];
		} bpb_32;
	};
};

struct __attribute__((packed)) Dir {
	char DIR_Name[11];
	uint8_t DIR_Attr;
	uint8_t DIR_NTRes;
	uint8_t DIR_CrtTimeTenth;
	leu16_t DIR_CrtTime;
	leu16_t DIR_CrtDate;
	leu16_t DIR_LstAccDate;
	leu16_t DIR_FstClusHI;
	leu16_t DIR_WrtTime;
	leu16_t DIR_WrtDate;
	leu16_t DIR_FstClusLO;
	leu32_t DIR_FileSize;
};

struct __attribute__((packed)) LongDir {
	uint8_t LDIR_Ord;
	leu16_t LDIR_Name1[5];
	uint8_t LDIR_Attr;
	uint8_t LDIR_Type;
	uint8_t LDIR_CHksum;
	leu16_t LDIR_Name2[6];
	leu16_t LDIR_FstClusLO;
	leu16_t LDIR_Name3[2];
};

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LONG_NAME \
	(ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)
#define ATTR_LONG_NAME_MASK                                            \
	(ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID | \
	    ATTR_DIRECTORY | ATTR_ARCHIVE)

#endif /* KRX_DEV_DOSFS_VAR_H */

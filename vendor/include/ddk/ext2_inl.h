#ifndef KRX_DDK_EXT2_INL_H
#define KRX_DDK_EXT2_INL_H

#include "ext2_fs.h"

static inline int
ext2fs_dirent_name_len(const struct ext2_dir_entry *entry)
{
	return le16_to_native(entry->name_len) & 0xff;
}

static inline int
ext2fs_dirent_file_type(const struct ext2_dir_entry *entry)
{
	return le16_to_native(entry->name_len) >> 8;
}

#endif /* KRX_DDK_EXT2_INL_H */

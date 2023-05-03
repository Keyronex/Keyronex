/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Apr 02 2023.
 */
/*!
 * @file 9pfs/9pfs_reg.h
 * @brief Protocol description for 9p2000.L
 *
 * Only 9p2000.L is supported; its protocol varies a little from 9p2000.U.
 */

#ifndef KRX_VIOFAM_9PFS_REG_H
#define KRX_VIOFAM_9PFS_REG_H

#include <sys/types.h>

#include "kdk/devmgr.h"

#ifdef __cplusplus
extern "C" {
#endif

enum ninep_kind {
	k9pLerror = 6,
	k9pLopen = 12,
	k9pLcreate = 14,
	k9pReadlink = 22,
	k9pGetattr = 24,
	k9pReaddir = 40,
	k9pLink = 70,
	k9pMkDir = 72,
	k9pUnlinkAt = 76,

	k9pVersion = 100,
	k9pAuth = 102,
	k9pAttach = 104,
	k9pError = 106,
	k9pFlush = 108,
	k9pWalk = 110,

	k9pOpen = 112,
	k9pCreate = 114,
	k9pRead = 116,
	k9pWrite = 118,
	k9pClunk = 120,
	k9pRemove = 122,
	k9pStat = 124,
	k9pWstat = 126,
};

enum ninep_getattr_mask {
	k9pGetattrMode = 0x1ULL,
	k9pGetattrNlink = 0x2ULL,
	k9pGetattrUid = 0x4ULL,
	k9pGetattrGid = 0x8ULL,
	k9pGetattrRdev = 0x10ULL,
	k9pGetattrAtime = 0x20ULL,
	k9pGetattrMtime = 0x40ULL,
	k9pGetattrCtime = 0x80ULL,
	k9pGetattrIno = 0x100ULL,
	k9pGetattrSize = 0x200ULL,
	k9pGetattrBlocks = 0x400ULL,

	k9pGetattrBtime = 0x800ULL,
	k9pGetattrGen = 0x1000ULL,
	k9pGetattrDataVersion = 0x2000ULL,

	k9pGetattrBasic = 0x7ffULL, /* all up to blocks */
	k9pGetattrAll = 0x3fffULL,  /* all */
};

struct ninep_hdr {
	uint32_t size;
	uint8_t kind;
	uint16_t tag;
	uint8_t data[0];
} __attribute__((packed));

struct ninep_str {
	uint16_t len;
	char str[0];
};

struct ninep_qid {
	uint8_t type;
	uint32_t version;
	uint64_t path;
} __attribute__((packed));

struct ninep_buf {
	size_t bufsize;
	io_off_t offset;
	struct ninep_hdr *data;
};

typedef uint32_t ninep_fid_t;

#define k9pVersion2000L "9P2000.L"

/*! @brief Allocate a 9p buffer for a format string. */
struct ninep_buf *ninep_buf_alloc(const char *fmt);
/*! @brief Allocate a 9p buffer for a given byte size.*/
struct ninep_buf *ninep_buf_alloc_bytes(size_t size);
/*! @brief Free a 9p buffer. */
void ninep_buf_free(struct ninep_buf *buf);

#define ninep_buf_addfid(BUF, NUM) ninep_buf_addu32(BUF, NUM)
void ninep_buf_addu16(struct ninep_buf *buf, uint16_t num);
void ninep_buf_addu32(struct ninep_buf *buf, uint32_t num);
void ninep_buf_addu64(struct ninep_buf *buf, uint64_t num);
void ninep_buf_addstr(struct ninep_buf *buf, const char *str);
void ninep_buf_close(struct ninep_buf *buf);

#define ninep_buf_getfid(BUF, PNUM) ninep_buf_getu32(BUF, PNUM)
int ninep_buf_getu8(struct ninep_buf *buf, uint8_t *num);
int ninep_buf_getu16(struct ninep_buf *buf, uint16_t *num);
int ninep_buf_getu32(struct ninep_buf *buf, uint32_t *num);
int ninep_buf_getu64(struct ninep_buf *buf, uint64_t *num);
int ninep_buf_getstr(struct ninep_buf *buf, char **str);
int ninep_buf_getqid(struct ninep_buf *buf, struct ninep_qid *qid_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRX_VIOFAM_9PFS_REG_H */

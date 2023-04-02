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

#include "kdk/devmgr.h"

#ifdef __cplusplus
extern "C" {
#endif

enum ninep_kind {
	k9pLerror = 6,

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

#define k9pVersion2000L "9P2000.L"

/*! Allocate a ninep input buffer. */
struct ninep_buf *ninep_buf_alloc(const char *fmt);
/*! Allocate a ninep output buffer. */
struct ninep_buf *ninep_buf_allocout(const char *fmt);

void ninep_buf_addu32(struct ninep_buf *buf, uint32_t num);
void ninep_buf_addstr(struct ninep_buf *buf, const char *str);
void ninep_buf_close(struct ninep_buf *buf);

int ninep_buf_getu32(struct ninep_buf *buf, uint32_t *num);
int ninep_buf_getstr(struct ninep_buf *buf, char **str);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRX_VIOFAM_9PFS_REG_H */

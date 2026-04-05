#ifndef ECX_LINUX_IF_ADDR_H
#define ECX_LINUX_IF_ADDR_H

#include <stdint.h>

struct ifa_cacheinfo {
	uint32_t ifa_prefered;
	uint32_t ifa_valid;
	uint32_t cstamp;	/* created, 1/100th secs */
	uint32_t tstamp;	/* updated, 1/100th secs */
};

enum ifa_f_flags {
	IFA_F_TENTATIVE = 0x40,
};

#endif /* ECX_LINUX_IF_ADDR_H */

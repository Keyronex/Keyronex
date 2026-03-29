#ifndef ECX_LINUX_NEIGHBOUR_H
#define ECX_LINUX_NEIGHBOUR_H

#include <stdint.h>

struct ndmsg {
	uint8_t		ndm_family;
	uint8_t		ndm_pad1;
	uint16_t	ndm_pad2;
	int32_t		ndm_ifindex;
	uint16_t	ndm_state;
	uint8_t		ndm_flags;
	uint8_t		ndm_type;
};

enum nud_state {
	NUD_INCOMPLETE = 0x01,
	NUD_REACHABLE = 0x02,
	NUD_STALE = 0x04,
	NUD_DELAY = 0x08,
	NUD_FAILED = 0x20,
};

enum {
	NDA_UNSPEC,
	NDA_DST,	/* neighbour l3 address */
	__NDA_MAX
};
#define	NDA_MAX	(__NDA_MAX - 1)

#endif /* ECX_LINUX_NEIGHBOUR_H */

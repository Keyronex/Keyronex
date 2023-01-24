
#include "lwip/tcpip.h"

void kstart(void) {
	tcpip_init(NULL, NULL);
	#if 0

		int autoconf(void);
	autoconf();
	vm_pagedump();
	
	#endif

	for (;; )asm("pause");
}

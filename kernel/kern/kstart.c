
#include <vm/vm.h>

#include "lwip/tcpip.h"

void kstart(void) {
	tcpip_init(NULL, NULL);


	int autoconf(void);
	autoconf();
	vm_pagedump();


	for (;; )asm("pause");
}

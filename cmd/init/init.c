#include <keyronex/syscall.h>
#include <stddef.h>

int
main(int argc, char *argv[])
{
	syscall1(SYS_debug_message, (uintptr_t)"Hello from userland!\n", NULL);
	for (;;)
		;
}

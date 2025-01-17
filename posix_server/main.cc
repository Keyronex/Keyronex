
#include <sys/epoll.h>

#include <keyronex/syscall.h>

#include <cassert>
#include <iostream>
#include <map>
#include <thread>

class Process {
	int handle;
	pid_t pid;
};

std::map<pid_t, Process> process_by_pid;
std::map<int, Process> process_by_handle;
int ep_handle;

int
main(int argc, char *argv[])
{
	std::cout << "Hello from the POSIX service!" << std::endl;

	ep_handle = epoll_create(0);
	assert(ep_handle != -1);

	std::cout << "ep_handle: " << ep_handle << std::endl;

	for (;;) ;

	return 0;
}

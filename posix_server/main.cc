
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

int
main(int argc, char *argv[])
{
	std::cout << "Hello from the POSIX service!" << std::endl;

	for (;;) ;

	return 0;
}

#include <sys/mman.h>
#include <keyronex/syscall.h>

#include <cassert>
#include <iostream>
#include <thread>

thread_local std::string x = "42";

static constexpr int nPages = 16;
char *mapping;

void run_over(void){
	while (true) {
		for (int i = 0; i < nPages; i++) {
			mapping[4096 * i] = i;
		}
	}
}

int main(int argc, char *argv[])
{
	std::cout << "Hello, I'm the POSIX server!!\n";

	mapping = (char*)mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(mapping != NULL);


	std::thread t2(
	    [](std::string msg) {
		    syscall1(kKrxDebugMessage, (uintptr_t)msg.c_str(), NULL);
		    for (;;) ;
	    },
	    "Hello world from thread 2!");

	std::thread t3(
	    [](std::string msg) {
		    syscall1(kKrxDebugMessage, (uintptr_t)msg.c_str(), NULL);
		    for (;;) ;
	    },
	    "Hello world from thread 3!");

	for (;;) ;

#if 0
	std::thread t2(
	    [](std::string msg) {
		    syscall1(kKrxDebugMessage, (uintptr_t)msg.c_str(), NULL);
		    x = "73";
		    syscall1(kKrxDebugMessage, (uintptr_t)(std::string("Thread 2 saw this value in TLS: " + x + "\n")).c_str(), NULL);
		   run_over();
	    },
	    "Hello world from thread 2!");
#endif

	std::string str("Hello from the Keyronex POSIX server; that's Thread 1!");
	str += x;
	syscall1(kKrxDebugMessage, (uintptr_t)str.c_str(), NULL);

#if 0
		   run_over();

	for (;;)
		;
#endif
	return 0;
}

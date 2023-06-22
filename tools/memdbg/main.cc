#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "parser.tab.h"

#include "result.hh"
#include <iostream>

std::map<std::string, Allocation> allocs;

extern int yyparse();

int
main(int argc, char **argv)
{
	int input;
	if (argc != 2) {
		printf("%s <input file>\n", argv[0]);
		return 1;
	}
	input = open(argv[1], O_RDONLY);
	dup2(input, STDIN_FILENO);
	close(input);
	// yydebug = true;
	yyparse();

	for (auto &alloc : allocs) {
		std::cout << "By " << alloc.second.pid << " In "
			  << alloc.second.zone << ": " << alloc.first << "\n";

		for (int i = 5; i < 7; i++) {
			std::string invocation =
			    "addr2line -e ../../../build/system-root/boot/keyronex " +
			    (*alloc.second.addresses)[i];
			std::cout << "  ";
			system(invocation.c_str());
		}
	}
}

int
yywrap(void)
{
	return 0;
}

int
yyerror(const char *msg)
{
	printf("Parse error: %s\n", msg);
	exit(1);
}

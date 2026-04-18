#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

int
main(int argc, char *argv[])
{
	char username[256];

	printf("login: ");
	fflush(stdout);

	if (fgets(username, sizeof(username), stdin) == NULL)
		err(1, "fgets");

	username[strcspn(username, "\n")] = 0;

	if (strlen(username) == 0)
		return 0;

	execl("/bin/login", "mini_login", username, (char *)NULL);
	err(1, "execl");
}

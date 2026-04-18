
#define _DEFAULT_SOURCE
#include <crypt.h>
#include <errno.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

int
main(int argc, char *argv[])
{
	const char *user;
	struct passwd *pw;
	struct spwd *sp;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <username>\n", argv[0]);
		return 1;
	}

	user = argv[1];

	pw = getpwnam(user);
	if (!pw) {
		printf("Login incorrect\n");
		return 1;
	}

	sp = getspnam(user);
	if (!sp || !sp->sp_pwdp) {
		printf("Login incorrect\n");
		return 1;
	}

	if (sp->sp_pwdp[0] != '\0') {
		char *pass = getpass("Password: ");
		char *crypted;

		if (pass == NULL)
			err(1, "getpass");

		crypted = crypt(pass, sp->sp_pwdp);

		memset(pass, 0, strlen(pass));

		if (crypted == NULL || strcmp(crypted, sp->sp_pwdp) != 0) {
			printf("Login incorrect\n");
			return 1;
		}
	}

	if (chdir(pw->pw_dir) != 0)
		warn("chdir");

	setenv("USER", pw->pw_name, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("SHELL", pw->pw_shell, 1);

	if (setgid(pw->pw_gid) != 0)
		err(1, "setgid");
	if (setuid(pw->pw_uid) != 0)
		err(1, "setuid");

	execl(pw->pw_shell, "-sh", NULL);
	err(1, "execl");
}

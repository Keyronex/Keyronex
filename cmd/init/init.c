/*
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 * Created on Sat Dec 13 2025.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file init.c
 * @brief Init daemon.
 */

#include <sys/ioctl.h>
#include <sys/termios.h>
#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void
runrc(void)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	printf("init: running /etc/rc\n");

	if (pid == 0) {
		execl("/bin/bash", "/bin/bash", "/etc/rc", NULL);
		perror("init: failed to run /etc/rc");
		return;
	}

	if (waitpid(pid, &status, 0) < 0) {
		perror("failed to wait for /etc/rc");
	}

	if (WEXITSTATUS(status) != 0) {
		printf("init: /etc/rc exited with status %d\n",
		    WEXITSTATUS(status));
	}
}

int
main(int argc, char *argv[])
{
	pid_t pid;

	runrc();

	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	if (pid == 0) {
		setsid();
		ioctl(0, TIOCSCTTY, 1);
		printf("init: starting login\n");
		execl("/usr/bin/bash", "-bash", NULL);
		perror("execl");
		exit(EXIT_FAILURE);
	}

	while (1) {
		pid_t wpid;
		int status;

		wpid = waitpid(-1, &status, 0);
		if (wpid < 0) {
			perror("waitpid");
			break;
		}

		if (WIFEXITED(status)) {
			printf("Process %d exited with status %d.\n", wpid,
			    WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			printf("Process %d killed by signal %d.\n", wpid,
			    WTERMSIG(status));
		}
	}

	printf("init: This point should be unreachable.\n");

	return 0;
}

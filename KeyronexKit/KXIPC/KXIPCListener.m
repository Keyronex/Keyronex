#include <sys/epoll.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#import <KeyronexKit/KXIPCConnection.h>
#import <KeyronexKit/KXIPCListener.h>
#include <assert.h>
#include <dxf/dxf.h>
#include <err.h>
#include <stdlib.h>
#include <unistd.h>

@implementation KXIPCListener

- (instancetype)initWithUnixSocketPath:(const char *)path
			      protocol:(Protocol *)protocol
				object:(id)object
{
	struct sockaddr_un sun;
	int r;

	self = [super init];

	_fd = socket(PF_UNIX, SOCK_SEQPACKET, 0);
	assert(_fd >= 0);

	sun.sun_family = AF_UNIX;
	strcpy(sun.sun_path, path);

	r = unlink(path);
	if (r < 0 && errno != ENOENT) {
		err(EXIT_FAILURE, "unlink");
	}

	r = bind(_fd, (struct sockaddr *)&sun, sizeof(sun));
	if (r < 0) {
		err(EXIT_FAILURE, "bind");
	}

	r = listen(_fd, 4);
	if (r < 0) {
		err(EXIT_FAILURE, "listen");
	}

	_protocol = protocol;
	_object = object;

	return self;
}

- (id)remoteObjectProxy
{
	return nil;
}

- (void)run
{
	int ep, r;
	struct epoll_event ev;

	ep = epoll_create(2);
	if (ep < 0)
		err(EXIT_FAILURE, "epoll_Create");

	ev.data.fd = _fd;
	ev.events = EPOLLIN;

	r = epoll_ctl(ep, EPOLL_CTL_ADD, _fd, &ev);
	if (r < 0)
		err(EXIT_FAILURE, "epoll_ctl");

	while (true) {
		KXIPCConnection *conn;

		r = epoll_wait(ep, &ev, 1, -1);
		if (r != 1)
			err(EXIT_FAILURE, "epoll_wait returned non-1");

		if (ev.data.fd == _fd) {
			int newfd;

			newfd = accept(_fd, NULL, NULL);
			if (newfd < 0)
				err(EXIT_FAILURE, "accept");

			conn = [[KXIPCConnection alloc] initWithFD:newfd
							  protocol:_protocol
							    object:_object];

			ev.data.ptr = conn;
			ev.events = EPOLLIN;

			r = epoll_ctl(ep, EPOLL_CTL_ADD, newfd, &ev);
		} else {
			char *buf = NULL;
			size_t nread = 0;
			conn = ev.data.ptr;

			if (ev.events & EPOLLIN) {
				buf = malloc(8192);
				nread = read(conn->_fd, buf, 8192);
			}

			if (ev.events & EPOLLHUP) {
				printf("Terminate connection of FD %d\n",
				    conn->_fd);
				epoll_ctl(ep, EPOLL_CTL_DEL, conn->_fd, &ev);
			}

			if (nread) {
				dxf_t *dxf = dxf_unpack(buf, nread, NULL);
				assert(dxf != NULL);
				[conn handleMessage:dxf];
			}

			if (buf)
				free(buf);
		}
	}
}

@end

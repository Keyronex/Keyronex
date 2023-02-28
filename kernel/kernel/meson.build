kernel_sources += files('ke.c', 'msgqueue.c', 'mutex.c', 'sched.c',
	'semaphore.c', 'thread.c', 'time.c', 'timer.c', 'wait.c',)

if (arch == 'x86_64')
	kernel_sources += files('amd64/keamd64.c')
endif
#include "kdk/executive.h"

struct ex_work_queue {
	kspinlock_t lock;
	TAILQ_HEAD(, ex_work) work;
};

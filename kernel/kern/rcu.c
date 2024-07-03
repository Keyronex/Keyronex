/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Fri April 4 2024.
 */
/*!
 * @file rcu.c
 * @brief RCU: Classic Edition implementation
 *
 * This is an implementation of RCU: Classic Edition exactly as described in
 * the expired patents.
 */

#include "kdk/kern.h"

struct rcu_state {
	kspinlock_t lock;
	uint64_t quiesced;
	uintptr_t generation, highest_generation;
};

static struct rcu_state rcu = {
	.lock = KSPINLOCK_INITIALISER,
	.quiesced = 0,
	.generation = 0,
	.highest_generation = 0,
};

static void
start_generation()
{
	/*
	 * "A new generation is initiated by setting all CPU’s bits in the
	 * bitmask, by incrementing the global current generation, and by
	 * setting the global maximum generation to one greater than the current
	 * value."
	 */
	rcu.quiesced = (1ULL << ncpus) - 1;
	rcu.generation++;
	rcu.highest_generation = rcu.generation;
}

void
ki_rcu_quiet()
{
	struct ki_rcu_per_cpu_data *rcpu = &curcpu()->rcu_cpustate;
	int cpunum = curcpu()->num;

	/*
	 * "As each CPU notices that its bit is set, it copies its counters to
	 * corresponding “snapshot” counters. Later, when the CPU notices that
	 * any of the counters differs from the snapshot, it clears its bit.
	 */

	if ((rcu.quiesced & 1 << cpunum)) {
		ke_spinlock_acquire_nospl(&rcu.lock);

		rcu.quiesced &= ~((uint64_t)1 << cpunum);
		if (rcu.quiesced == 0) {
			/*
			 * "If its bit is the last one set, it increments the
			 * global current generation."
			 */
			rcu.generation++;
			/*
			 * "If the global current generation does not then
			 * exceed the global maximum generation, the CPU
			 * initiates a new generation."
			 */
			if (rcu.generation <= rcu.highest_generation)
				start_generation();
		}

		ke_spinlock_release_nospl(&rcu.lock);
	}

	/*
	 * "As each CPU notices that the global current generation has advanced
	 * past its generation number, it appends the contents of its current
	 * list to its intr list, and schedules a tasklet to process it."
	 */
	if (!TAILQ_EMPTY(&rcpu->current_callbacks) &&
	    rcu.generation > rcpu->generation) {
		TAILQ_CONCAT(&rcpu->past_callbacks, &rcpu->current_callbacks,
		    queue_entry);
		ke_dpc_enqueue(&rcpu->past_callbacks_dpc);
	}

	if (TAILQ_EMPTY(&rcpu->current_callbacks) &&
	    !TAILQ_EMPTY(&rcpu->next_callbacks)) {
		/*
		 * "If the CPU’s next list is nonempty, the CPU moves it to its
		 * current list..."
		 * */

		TAILQ_CONCAT(&rcpu->current_callbacks, &rcpu->next_callbacks,
		    queue_entry);

		ke_spinlock_acquire_nospl(&rcu.lock);
		/*
		 * "... and sets its per-CPU generation number to be one greater
		 * than the global generation number."
		 */
		rcpu->generation = rcu.generation + 1;

		if (rcu.quiesced != 0) {
			/*
			 * If a generation is already in progress, the CPU sets
			 * the global maximum generation number to be one
			 * greater than its per-CPU generation number...
			 */
			kassert(rcu.highest_generation <= rcpu->generation);
			rcu.highest_generation = rcpu->generation + 1;
		} else
			/* ... otherwise, it starts a new generation. */
			start_generation();

		ke_spinlock_release_nospl(&rcu.lock);
	}
}

void
process_past_callbacks(void *arg)
{
	struct ki_rcu_per_cpu_data *rcpu = arg;

	while (!TAILQ_EMPTY(&rcpu->past_callbacks)) {
		krcu_entry_t *head = TAILQ_FIRST(&rcpu->past_callbacks);
		TAILQ_REMOVE(&rcpu->past_callbacks, head, queue_entry);
		head->callback(head->arg);
	}
}

void
ki_rcu_per_cpu_init(struct ki_rcu_per_cpu_data *data)
{
	data->generation = 0;
	TAILQ_INIT(&data->past_callbacks);
	TAILQ_INIT(&data->current_callbacks);
	TAILQ_INIT(&data->next_callbacks);
	data->past_callbacks_dpc.arg = data;
	data->past_callbacks_dpc.callback = process_past_callbacks;
	data->past_callbacks_dpc.cpu = NULL;
}

void
ke_rcu_call(krcu_entry_t *head, krcu_callback_t callback, void *arg)
{
	ipl_t ipl = spldpc();
	head->callback = callback;
	head->arg = arg;
	TAILQ_INSERT_TAIL(&curcpu()->rcu_cpustate.next_callbacks, head,
	    queue_entry);
	splx(ipl);
}

static void set_event(void *arg)
{
	kevent_t *ev = arg;
	ke_event_signal(ev);
}

void ke_rcu_synchronise(void)
{
	kevent_t ev;
	krcu_entry_t head;
	ke_event_init(&ev, false);
	ke_rcu_call(&head, set_event, &ev);
	ke_wait(&ev, "ke_rcu_synchronise", false, false, -1);
}

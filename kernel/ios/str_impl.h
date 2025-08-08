/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Tue Feb 25 2025.
 */
/*!
 * @file str_impl.h
 * @brief STREAMS private definitions.
 */

#ifndef KRX_IOS_STR_IMPL_H
#define KRX_IOS_STR_IMPL_H

#include <kdk/iop.h>
#include <kdk/stream.h>

/*
 * Stream head.
 */
typedef struct stdata {
	kspinlock_t sd_lock;

	bool sd_readlocked;
	TAILQ_HEAD(, sd_reader_entry) sd_read_threads;
	iop_queue_t sd_read_iops;

	queue_t *sd_rq;
} stdata_t;

/*!
 * @brief Acquire a stream head's read-lock as a thread. This will block and
 * wait if the read-lock is currently held by another reader.
 *
 * @param[out] pipl Receives the IPL value at function entry, if the call was
 * successful.
 *
 * @retval 0
 * 	Successfully acquired the read-lock. The stream spinlock is also
 * 	held, and *pipl is set to IPL at entry.
 * @retval -EINTR
 * 	if the wait was interrupted by an alert. No locks are
 * 	held, and *pipl is not set.
 */
int str_readlock_thread(stdata_t *stp, ipl_t *pipl);

/*!
 * @brief Acquire a stream head's read lock as an IOP. If the read-lock is
 * already held, the IOP will be queued and will be continued when the read-lock
 * is available.
 *
 * @retval 0
 * 	Successfully acquired the read-lock. The stream spinlock is also held.
 * @retval -EAGAIN
 * 	if the read-lock was already held. The IOP is queued for continuation
 * 	when the read-lock is available.
 */
int str_readlock_iop(stdata_t *stp, iop_t *iop);

/*!
 * @brief Release a stream head's read-lock.
 *
 * @param[in] ipl IPL value at function entry.
 */
void str_readunlock(stdata_t *stp, ipl_t ipl);

#endif /* KRX_IOS_STRSUBR_H */

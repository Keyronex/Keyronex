/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Fri Apr 26 2024.
 */
/*!
 * @file writeback.c
 * @brief Dirty page write-back daemon.
 */

#include "vmp.h"

extern kevent_t vmp_writeback_event;

void
vmp_writeback(void *)
{
	while (true) {
		kwaitresult_t w = ke_wait(&vmp_writeback_event,
		    "vmp_writeback_event", false, false, NS_PER_S);
		(void)w;
	}
}

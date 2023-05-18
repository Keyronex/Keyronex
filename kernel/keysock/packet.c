/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu May 18 2023.
 */

#include "keysock/sockfs.h"

void
packet_queue_init(struct packet_stailq *queue)
{
	STAILQ_INIT(queue);
}

void packet_add_to_queue(struct packet_stailq *queue, struct packet *packet)
{
	STAILQ_INSERT_TAIL(queue, packet, stailq_entry);
}

/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Apr 27 2023.
 */

#ifndef KRX_DEV_MOUSE_H
#define KRX_DEV_MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mouse_packet {
	uint8_t flags;
	int32_t x_mov;
	int32_t y_mov;
};

struct mouse *mouse_init(void);
void mouse_dispatch(struct mouse_packet);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRX_DEV_MOUSE_H */

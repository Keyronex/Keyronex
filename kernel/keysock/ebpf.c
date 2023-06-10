/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Jun 04 2023.
 */

#include <stdint.h>

struct instruction {
	uint8_t code;
	uint8_t reg_dest: 4;
	uint8_t reg_src: 4;
	int16_t offset;
	int32_t imm;
};

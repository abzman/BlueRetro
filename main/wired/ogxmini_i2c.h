/*
 * Copyright (c) 2026, BlueRetro contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OGXMINI_I2C_H_
#define _OGXMINI_I2C_H_

#include <stdint.h>

void ogxmini_i2c_init(void);
/* An output's pad changed: send it now rather than on the next tick */
void ogxmini_i2c_kick(uint32_t index);

#endif /* _OGXMINI_I2C_H_ */

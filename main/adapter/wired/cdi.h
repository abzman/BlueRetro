/*
 * Copyright (c) 2019-2022, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _CDI_H_
#define _CDI_H_
#include "adapter/adapter.h"

/* CD-i button bits in first byte of pointing device packets */
#define CDI_BTN_1 0x20
#define CDI_BTN_2 0x10

/* Absolute coordinate device ('T'), 10 bit position, 4 x 7 bit bytes:
 *   b0: 1 B1 B2 X9 X8 X7 X6
 *   b1: 0 PD  0  0 Y9 Y8 Y7 Y6   (PD = pen down / in proximity)
 *   b2: 0  X5 X4 X3 X2 X1 X0
 *   b3: 0  Y5 Y4 Y3 Y2 Y1 Y0
 * Origin is top left. x & y here are centered signed values in
 * [CDI_ABS_MIN, CDI_ABS_MAX], as produced by the adapter axis scaling. */
#define CDI_ABS_MIN (-512)
#define CDI_ABS_MAX 511
#define CDI_ABS_ID 'T'
#define CDI_ABS_PKT_LEN 4

static inline void cdi_abs_encode(uint8_t btns, int32_t x, int32_t y, uint32_t pen_down, uint8_t *data) {
    uint32_t ux, uy;

    x = (x < CDI_ABS_MIN) ? CDI_ABS_MIN : ((x > CDI_ABS_MAX) ? CDI_ABS_MAX : x);
    y = (y < CDI_ABS_MIN) ? CDI_ABS_MIN : ((y > CDI_ABS_MAX) ? CDI_ABS_MAX : y);
    ux = (uint32_t)(x - CDI_ABS_MIN);
    uy = (uint32_t)(y - CDI_ABS_MIN);

    data[0] = 0x40 | (btns & (CDI_BTN_1 | CDI_BTN_2)) | ((ux >> 6) & 0x0F);
    data[1] = (pen_down ? 0x40 : 0x00) | ((uy >> 6) & 0x0F);
    data[2] = ux & 0x3F;
    data[3] = uy & 0x3F;
}

void cdi_meta_init(struct wired_ctrl *ctrl_data);
void cdi_init_buffer(int32_t dev_mode, struct wired_data *wired_data);
void cdi_from_generic(int32_t dev_mode, struct wired_ctrl *ctrl_data, struct wired_data *wired_data);
void cdi_kb_id_to_scancode(uint32_t dev_id, uint8_t type, uint8_t id);
void cdi_gen_turbo_mask(struct wired_data *wired_data);

#endif /* _CDI_H_ */

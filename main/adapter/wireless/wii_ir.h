/*
 * Copyright (c) 2026, abzman2000
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _WII_IR_H_
#define _WII_IR_H_

#include <stdint.h>

/* Absolute pointer range produced by the tracker, centered on 0.
 * X grows to the right, Y grows upward (BlueRetro generic convention). */
#define WII_IR_PTR_MIN (-512)
#define WII_IR_PTR_MAX 511

/* Largest per-report delta from wii_ir_ptr_delta(). Kept at a typical HID
 * mouse logical range so adapter scaling treats it like a real mouse. */
#define WII_IR_DELTA_MAX 127

/* Tracks the Wii sensor bar (2 IR sources) seen by the Wiimote camera
 * and turns it into a screen pointer, usable both as an absolute position
 * (light gun / tablet style outputs) and as relative motion (mouse outputs). */
struct wii_ir_ptr {
    int32_t mid_x;   /* Last sensor bar midpoint, camera coordinates */
    int32_t mid_y;
    int32_t half_dx; /* Half the vector from left dot to right dot */
    int32_t half_dy;
    uint32_t has_half;
    uint32_t visible; /* Dots used in last update: 0, 1 or 2 */
    int32_t last_x;   /* Pointer position at previous update, for deltas */
    int32_t last_y;
    int32_t dx;       /* Motion since previous update */
    int32_t dy;
};

void wii_ir_ptr_init(struct wii_ir_ptr *s);

/* ir: the 12 bytes of extended mode IR data from report 0x33.
 * Returns number of dots used (0 = nothing seen, position held). */
uint32_t wii_ir_ptr_update(struct wii_ir_ptr *s, const uint8_t *ir);

/* Pointer position in [WII_IR_PTR_MIN, WII_IR_PTR_MAX] on both axes */
void wii_ir_ptr_get(const struct wii_ir_ptr *s, int32_t *x, int32_t *y);

/* Pointer motion since previous update, same orientation as the position,
 * clamped to +/- WII_IR_DELTA_MAX. Zero while nothing is tracked and on the
 * first update after tracking is (re)acquired, so aiming back at the screen
 * doesn't fling a mouse cursor. */
void wii_ir_ptr_delta(const struct wii_ir_ptr *s, int32_t *dx, int32_t *dy);

#endif /* _WII_IR_H_ */

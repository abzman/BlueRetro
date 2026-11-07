/*
 * Copyright (c) 2026, abzman2000
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wii_ir.h"

#define CAM_X_MAX 1023
#define CAM_Y_MAX 767
#define CAM_DOTS 4

struct ir_dot {
    int32_t x;
    int32_t y;
    int32_t size;
};

static inline int32_t clamp(int32_t v, int32_t min, int32_t max) {
    return (v < min) ? min : ((v > max) ? max : v);
}

static inline int32_t dist2(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    int32_t dx = ax - bx;
    int32_t dy = ay - by;
    return dx * dx + dy * dy;
}

void wii_ir_ptr_init(struct wii_ir_ptr *s) {
    s->mid_x = (CAM_X_MAX + 1) / 2;
    s->mid_y = (CAM_Y_MAX + 1) / 2;
    s->half_dx = 0;
    s->half_dy = 0;
    s->has_half = 0;
    s->visible = 0;
    s->last_x = 0;
    s->last_y = 0;
    s->dx = 0;
    s->dy = 0;
}

/* Extended mode: 3 bytes per dot, all 0xFF when slot is empty */
static uint32_t decode_ext(const uint8_t *ir, struct ir_dot *dots) {
    uint32_t cnt = 0;

    for (uint32_t i = 0; i < CAM_DOTS; i++) {
        const uint8_t *b = &ir[i * 3];
        int32_t x = b[0] | ((b[2] & 0x30) << 4);
        int32_t y = b[1] | ((b[2] & 0xC0) << 2);

        if (x > CAM_X_MAX || y > CAM_Y_MAX) {
            continue;
        }
        dots[cnt].x = x;
        dots[cnt].y = y;
        dots[cnt].size = b[2] & 0x0F;
        cnt++;
    }
    return cnt;
}

static void update_delta(struct wii_ir_ptr *s, uint32_t was_visible) {
    int32_t x, y;

    wii_ir_ptr_get(s, &x, &y);
    if (s->visible && was_visible) {
        s->dx = clamp(x - s->last_x, -WII_IR_DELTA_MAX, WII_IR_DELTA_MAX);
        s->dy = clamp(y - s->last_y, -WII_IR_DELTA_MAX, WII_IR_DELTA_MAX);
    }
    else {
        s->dx = 0;
        s->dy = 0;
    }
    s->last_x = x;
    s->last_y = y;
}

uint32_t wii_ir_ptr_update(struct wii_ir_ptr *s, const uint8_t *ir) {
    struct ir_dot dots[CAM_DOTS];
    uint32_t cnt = decode_ext(ir, dots);
    uint32_t was_visible = s->visible;

    if (cnt >= 2) {
        /* Keep the two biggest blobs, the sensor bar LEDs are usually the
         * brightest things in view, small ones tend to be reflections. */
        uint32_t a = 0, b = 1;
        if (dots[b].size > dots[a].size) {
            a = 1; b = 0;
        }
        for (uint32_t i = 2; i < cnt; i++) {
            if (dots[i].size > dots[a].size) {
                b = a;
                a = i;
            }
            else if (dots[i].size > dots[b].size) {
                b = i;
            }
        }
        /* Order left to right so the half vector keeps a stable sign */
        struct ir_dot *l = &dots[a], *r = &dots[b];
        if (l->x > r->x) {
            l = &dots[b];
            r = &dots[a];
        }
        s->mid_x = (l->x + r->x) / 2;
        s->mid_y = (l->y + r->y) / 2;
        s->half_dx = (r->x - l->x) / 2;
        s->half_dy = (r->y - l->y) / 2;
        s->has_half = 1;
        s->visible = 2;
    }
    else if (cnt == 1) {
        if (s->has_half) {
            /* One LED left the camera view, rebuild the midpoint from the
             * remaining one and pick whichever side is closest to the last
             * known midpoint so the pointer doesn't jump. */
            int32_t ax = dots[0].x + s->half_dx, ay = dots[0].y + s->half_dy;
            int32_t bx = dots[0].x - s->half_dx, by = dots[0].y - s->half_dy;
            if (dist2(ax, ay, s->mid_x, s->mid_y) <= dist2(bx, by, s->mid_x, s->mid_y)) {
                s->mid_x = ax;
                s->mid_y = ay;
            }
            else {
                s->mid_x = bx;
                s->mid_y = by;
            }
        }
        else {
            s->mid_x = dots[0].x;
            s->mid_y = dots[0].y;
        }
        s->mid_x = clamp(s->mid_x, 0, CAM_X_MAX);
        s->mid_y = clamp(s->mid_y, 0, CAM_Y_MAX);
        s->visible = 1;
    }
    else {
        /* Nothing in view, hold last position */
        s->visible = 0;
    }
    update_delta(s, was_visible);
    return s->visible;
}

void wii_ir_ptr_get(const struct wii_ir_ptr *s, int32_t *x, int32_t *y) {
    /* Camera image is mirrored horizontally relative to where the remote points */
    int32_t sx = CAM_X_MAX - s->mid_x;
    /* Stretch 768 camera lines over the 1024 pointer range */
    int32_t sy = (s->mid_y * 1024) / (CAM_Y_MAX + 1);

    *x = clamp(sx - 512, WII_IR_PTR_MIN, WII_IR_PTR_MAX);
    *y = clamp(511 - sy, WII_IR_PTR_MIN, WII_IR_PTR_MAX);
}

void wii_ir_ptr_delta(const struct wii_ir_ptr *s, int32_t *dx, int32_t *dy) {
    *dx = s->dx;
    *dy = s->dy;
}

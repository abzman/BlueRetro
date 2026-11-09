/*
 * Copyright (c) 2026, BlueRetro contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "beam_timing.h"

/* In IRAM on the ESP32 (main/linker.lf): it runs in the latch interrupt */

/* n frames of the period, in cycles: n * (p_int + p_frac / 256) */
static inline uint32_t periods(const struct beam_timing *bt, uint32_t n) {
    return n * bt->p_int + ((n * bt->p_frac) >> 8);
}

static void restart(struct beam_timing *bt, uint32_t t) {
    uint32_t p_min = bt->p_min, p_max = bt->p_max, resets = bt->resets;

    memset(bt, 0, sizeof(*bt));
    bt->p_min = p_min;
    bt->p_max = p_max;
    bt->resets = resets;
    bt->last_t = t;
    bt->first_t = t;
    bt->cur_t = t;
    bt->cur_valid = 1;
    bt->ref = t;
    bt->polls = 1;
}

/* Least squares slope through the block minima, as a correction to the current period: their
 * times less the period's, against their frame numbers. Integers only: in the latch interrupt the
 * ESP32 may only call what is in IRAM or ROM, and its ROM has 64-bit integer helpers but not
 * float division. Slope and intercept in 1/256 cycles. */
static void fit(struct beam_timing *bt) {
    uint32_t o = (bt->head + BEAM_BLOCKS - bt->count) % BEAM_BLOCKS;
    uint32_t t0 = bt->bt[o], f0 = bt->bf[o];
    int64_t sx = 0, sy = 0, sxx = 0, sxy = 0, n = bt->count;

    for (uint32_t k = 0; k < bt->count; k++) {
        uint32_t i = (o + k) % BEAM_BLOCKS;
        int64_t x = bt->bf[i] - f0;
        int64_t y = (int32_t)(bt->bt[i] - t0 - periods(bt, bt->bf[i] - f0));
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    int64_t den = n * sxx - sx * sx;
    if (den <= 0) {
        return;
    }
    int64_t slope_q8 = ((n * sxy - sx * sy) * 256) / den;     /* cycles per frame to add */

    /* Through the lowest block rather than the middle of them: each block's earliest poll is up
     * to a few dots past the window start, by an amount that comes and goes; the lowest of 16 is
     * the closest and changes least */
    int64_t icpt_q8 = 0;
    for (uint32_t k = 0; k < bt->count; k++) {
        uint32_t i = (o + k) % BEAM_BLOCKS;
        int64_t x = bt->bf[i] - f0;
        int64_t y = (int32_t)(bt->bt[i] - t0 - periods(bt, bt->bf[i] - f0));
        int64_t r = y * 256 - slope_q8 * x;
        if (k == 0 || r < icpt_q8) {
            icpt_q8 = r;
        }
    }

    /* the line at the newest block, then carried with the new period */
    uint32_t fn = bt->bf[(bt->head + BEAM_BLOCKS - 1) % BEAM_BLOCKS];
    int64_t at_fn_q8 = icpt_q8 + slope_q8 * (int64_t)(fn - f0);
    int32_t p_q8 = (int32_t)((bt->p_int << 8) + bt->p_frac) + (int32_t)slope_q8;

    bt->line_t = t0 + periods(bt, fn - f0) + (int32_t)(at_fn_q8 >> 8);
    bt->line_f = fn;
    bt->p_int = (uint32_t)p_q8 >> 8;
    bt->p_frac = (uint32_t)p_q8 & 0xFF;
}

void beam_timing_init(struct beam_timing *bt, uint32_t cycles_per_us) {
    memset(bt, 0, sizeof(*bt));
    bt->p_min = 14000 * cycles_per_us;
    bt->p_max = 22000 * cycles_per_us;
}

void beam_timing_poll(struct beam_timing *bt, uint32_t t) {
    if (bt->polls == 0) {
        restart(bt, t);
        return;
    }

    uint32_t d = t - bt->last_t;
    uint32_t m;

    if (bt->p_int == 0) {
        /* Second poll: the first period, if it is one */
        if (d < bt->p_min || d > bt->p_max) {
            if (d < bt->p_min / 2) {
                return;     /* a second read in the same frame: keep the first */
            }
            bt->resets++;
            restart(bt, t);
            return;
        }
        bt->p_int = d;
        m = 1;
    }
    else {
        if (d < bt->p_int / 2) {
            return;         /* a second read in the same frame: keep the first */
        }
        m = (d + bt->p_int / 2) / bt->p_int;
        int32_t err = (int32_t)(d - periods(bt, m));
        /* Frames may be skipped (a game's lag frames), but a poll that lands outside its window
         * means the timing changed: start over */
        if (m > 8 || err > (int32_t)(bt->p_int / 16) || err < -(int32_t)(bt->p_int / 16)) {
            bt->resets++;
            restart(bt, t);
            return;
        }
    }

    uint32_t prev_block = bt->frame / BEAM_BLOCK;
    bt->frame += m;
    bt->last_t = t;
    bt->polls++;

    if (bt->frame / BEAM_BLOCK != prev_block) {
        /* Block done: keep its earliest poll, fit again */
        if (bt->cur_valid) {
            bt->bt[bt->head] = bt->cur_t;
            bt->bf[bt->head] = bt->cur_f;
            bt->head = (bt->head + 1) % BEAM_BLOCKS;
            if (bt->count < BEAM_BLOCKS) {
                bt->count++;
            }
        }
        bt->cur_t = t;
        bt->cur_f = bt->frame;
        bt->cur_valid = 1;
        if (bt->count >= 2) {
            fit(bt);
        }
    }
    else if ((int32_t)((t - bt->cur_t) - periods(bt, bt->frame - bt->cur_f)) < 0) {
        bt->cur_t = t;
        bt->cur_f = bt->frame;
    }

    if (bt->count < 2) {
        /* Until there is a line: the period from all the polls, the reference from this one */
        uint32_t span = t - bt->first_t;
        bt->p_int = span / bt->frame;
        bt->p_frac = ((span % bt->frame) << 8) / bt->frame;
        bt->ref = t;
    }
    else {
        bt->ref = bt->line_t + periods(bt, bt->frame - bt->line_f);
    }
}

uint32_t beam_timing_locked(const struct beam_timing *bt) {
    return bt->p_int && bt->count >= BEAM_LOCK_BLOCKS;
}

uint32_t beam_timing_period(const struct beam_timing *bt) {
    return bt->p_int;
}

uint32_t beam_timing_target(const struct beam_timing *bt, uint32_t lines, int32_t dots,
                            uint32_t lines_per_frame, uint32_t dots_per_line) {
    /* The period over the frame's dots, unrounded: a cycles-per-dot rounded to 1/256 was off by
     * 5 dots at the bottom of the screen */
    int64_t p_q8 = ((int64_t)bt->p_int << 8) + bt->p_frac;
    int64_t total = (int64_t)(lines * dots_per_line) + dots;

    return bt->ref + (uint32_t)(int32_t)((total * p_q8) / ((int64_t)lines_per_frame * dots_per_line * 256));
}

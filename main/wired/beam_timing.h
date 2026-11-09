/*
 * Copyright (c) 2026, BlueRetro contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _BEAM_TIMING_H_
#define _BEAM_TIMING_H_

#include <stdint.h>

/* Where the console's beam is, from the only per-frame event a controller port sees: the poll.
 *
 * A light gun tells the console where it points by pulling a line the instant the beam passes
 * under it. To fake that from an absolute pointer (a Wiimote), the adapter needs the time each
 * frame starts. The polls give it, but not exactly: the SNES auto-read starts at a different
 * point of the first V-Blank line every frame (a 256 master-clock grid walked through a ~63-dot
 * window), and a game reading the pad itself does it after a variable amount of code. Both stay
 * inside a window whose start is a fixed point of the frame, and every few frames a poll lands
 * close to it. So: take the earliest poll of each block of frames (each carried back by the
 * frame period to compare), and fit a line through those of the last 512 frames. Its slope is
 * the frame period, and where it falls in a frame is the window start: `ref`.
 *
 * Times are CPU cycle counts (wrapping 32-bit). Pure arithmetic, no hardware: tested on the host
 * against a model of the SNES (tests/pytest_beam_timing.py). */

#define BEAM_BLOCK 32           /* frames a block: each holds a poll near the window start */
#define BEAM_BLOCKS 16          /* blocks the line is fitted over */
#define BEAM_LOCK_BLOCKS 6      /* blocks before targets are given (~3 s) */

struct beam_timing {
    uint32_t bt[BEAM_BLOCKS];   /* earliest poll of each finished block */
    uint32_t bf[BEAM_BLOCKS];   /* and its frame number */
    uint32_t head;
    uint32_t count;
    uint32_t cur_t;             /* earliest poll of the block being filled */
    uint32_t cur_f;
    uint32_t cur_valid;
    uint32_t frame;             /* frame number of the last poll kept */
    uint32_t last_t;
    uint32_t first_t;           /* first poll since a restart */
    uint32_t polls;
    uint32_t p_int;             /* frame period, cycles */
    uint32_t p_frac;            /* and 1/256ths */
    uint32_t line_t;            /* the fitted line: window start at frame line_f */
    uint32_t line_f;
    uint32_t ref;               /* the window start in the last poll's frame */
    uint32_t p_min;             /* plausible period range, cycles */
    uint32_t p_max;
    uint32_t resets;            /* times the polls stopped fitting the frame (console reset, game
                                 * changing how it reads); for the tests */
};

/* cycles_per_us: CPU clock; the period accepted is 14 ms to 22 ms (any NTSC or PAL console) */
void beam_timing_init(struct beam_timing *bt, uint32_t cycles_per_us);

/* At each poll, with the cycle count when it began */
void beam_timing_poll(struct beam_timing *bt, uint32_t t);

/* Whether targets can be trusted yet */
uint32_t beam_timing_locked(const struct beam_timing *bt);

/* Frame period in cycles (0 if unknown) */
uint32_t beam_timing_period(const struct beam_timing *bt);

/* Cycle count at which the beam is `lines` lines and `dots` dots past `ref` (`dots` may be
 * negative), the frame being lines_per_frame lines of dots_per_line dots */
uint32_t beam_timing_target(const struct beam_timing *bt, uint32_t lines, int32_t dots,
                            uint32_t lines_per_frame, uint32_t dots_per_line);

#endif /* _BEAM_TIMING_H_ */

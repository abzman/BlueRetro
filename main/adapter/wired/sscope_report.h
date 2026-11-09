/*
 * Copyright (c) 2026, BlueRetro contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SSCOPE_REPORT_H_
#define _SSCOPE_REPORT_H_

#include <stdint.h>

/* The SNES Super Scope's report byte from what the gun holds, poll by poll. Bits as the scope
 * reports them (FCTP 00nN): Fire, Cursor, Turbo, Pause, null (no screen in sight), noise. As
 * MAME's snes_sscope: Pause and the turbo switch act on a press, Cursor while held, Fire on a
 * press or, with turbo on, while held. Pure, so the host tests run it (tests/beam_timing_sim.c). */
#define SSCOPE_FIRE 0x80
#define SSCOPE_CURSOR 0x40
#define SSCOPE_TURBO 0x20       /* in held: the button flipping the switch; in the report: the switch */
#define SSCOPE_PAUSE 0x10
#define SSCOPE_OFFSCREEN 0x02

struct sscope_state {
    uint8_t report;     /* last report: its turbo bit is the switch */
    uint8_t locks;      /* edge-triggered buttons held at the last poll */
};

static inline uint8_t sscope_report(struct sscope_state *s, uint8_t held) {
    uint8_t report = s->report & SSCOPE_TURBO;

    report |= held & SSCOPE_OFFSCREEN;
    if ((held & SSCOPE_PAUSE) && !(s->locks & SSCOPE_PAUSE)) {
        report |= SSCOPE_PAUSE;
    }
    if ((held & SSCOPE_TURBO) && !(s->locks & SSCOPE_TURBO)) {
        report ^= SSCOPE_TURBO;
    }
    report |= held & SSCOPE_CURSOR;
    if ((held & SSCOPE_FIRE) && ((report & SSCOPE_TURBO) || !(s->locks & SSCOPE_FIRE))) {
        report |= SSCOPE_FIRE;
    }
    s->locks = held & (SSCOPE_PAUSE | SSCOPE_TURBO | SSCOPE_FIRE);
    s->report = report;
    return report;
}

#endif /* _SSCOPE_REPORT_H_ */

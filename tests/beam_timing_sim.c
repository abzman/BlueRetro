/* Host model of a SNES's polls feeding main/wired/beam_timing.c, built and run by
 * pytest_beam_timing.py. For each scenario it prints, once the tracker is locked, how far its
 * aim lands from where the beam really is: the mean (a fixed offset, which a game's calibration
 * takes out) and the spread around it, in dots, over the whole run.
 *
 * The model, from fullsnes / anomie's timing notes:
 * - master clock 21.477 MHz NTSC (21.281 MHz PAL), 1364 master clocks a line, 4 a dot;
 * - NTSC: 262 lines, frames alternately 357368 and 357364 master clocks (the short line);
 *   PAL: 312 lines, 425568;
 * - auto joypad read starts on a 256 master-clock grid counted from the previous read, at
 *   the first grid point past dot 32.5 of the first V-Blank line (it stays inside 32.5..95.5);
 * - a game reading the pad itself instead: anywhere in a window of its own, at random.
 * The adapter's side: a 240 MHz CPU, both crystals off by up to 50 ppm, the poll's time stamped
 * with up to 40 cycles of interrupt jitter, and a game skipping polls in lag frames. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "beam_timing.h"
#include "sscope_report.h"

#define CPU_HZ 240e6

struct scenario {
    const char *name;
    int pal;
    int manual;         /* window in dots for a game's own reads; 0: auto-read */
    double snes_ppm;
    double cpu_ppm;
    int jitter;         /* cycles */
    double lag;         /* probability a frame has no poll */
    int reset_at;       /* frame the console resets at (phase jumps); 0: never */
    int frames;
};

static double frand(void) {
    return rand() / (double)RAND_MAX;
}

static void run(const struct scenario *s) {
    struct beam_timing bt;
    double mc_hz = (s->pal ? 21281370.0 : 21477272.7) * (1 + s->snes_ppm * 1e-6);
    double cpu_hz = CPU_HZ * (1 + s->cpu_ppm * 1e-6);
    uint32_t lines_frame = s->pal ? 312 : 262;
    double frame_mc = 0, start_mc = 0;          /* this frame's start; last auto-read start */
    double sum = 0, mn = 1e9, mx = -1e9;
    double ssum = 0, smn = 1e9, smx = -1e9, prev[2] = {0, 0}, jump = 0;
    long n = 0, sn = 0;
    int locked_at = -1, relock_at = -1;
    uint32_t resets_before = 0;
    /* where to aim: two corners of the picture, as lines/dots past the window start */
    const uint32_t tl[2] = {38, 0}, br[2] = {38 + 223, 255};

    srand(1234);
    beam_timing_init(&bt, 240);
    start_mc = 298;     /* first read at dot 74.5 */

    for (int f = 0; f < s->frames; f++) {
        double len = s->pal ? 425568 : ((f & 1) ? 357364 : 357368);

        if (s->reset_at && f == s->reset_at) {
            frame_mc += 123457;     /* a reset: the frame restarts somewhere else */
            start_mc = frame_mc + 298;
            resets_before = bt.resets;
        }

        double poll_mc;
        if (s->manual) {
            poll_mc = frame_mc + 130 + frand() * s->manual * 4;
        }
        else {
            /* first point of the 256 grid at or past dot 32.5 */
            double lo = frame_mc + 130;
            double k = ceil((lo - start_mc) / 256);
            if (f > 0 || s->reset_at == f) {
                start_mc += k * 256;
            }
            poll_mc = start_mc;
        }

        if (frand() >= s->lag) {
            double t = poll_mc / mc_hz * cpu_hz + (rand() % (2 * s->jitter + 1) - s->jitter);
            uint32_t tc = (uint32_t)(uint64_t)fmod(t, 4294967296.0);
            beam_timing_poll(&bt, tc);

            if (beam_timing_locked(&bt) && bt.frame) {
                if (locked_at < 0) {
                    locked_at = f;
                }
                if (s->reset_at && f > s->reset_at && relock_at < 0 && bt.resets > resets_before) {
                    relock_at = f;
                }
                if (s->reset_at && f >= s->reset_at && f < s->reset_at + 200) {
                    goto next;  /* settling after the reset is reported apart */
                }
                for (int c = 0; c < 2; c++) {
                    const uint32_t *pos = c ? br : tl;
                    uint32_t aim = beam_timing_target(&bt, pos[0], pos[1], lines_frame, 341);
                    /* the beam's real time there: the window start of this frame */
                    double true_mc = frame_mc + 130 + pos[0] * 1364.0 + pos[1] * 4.0;
                    double true_t = true_mc / mc_hz * cpu_hz;
                    double diff = (double)(int32_t)(aim - (uint32_t)(uint64_t)fmod(true_t, 4294967296.0));
                    double dots = diff / (cpu_hz / mc_hz * 4);
                    sum += dots;
                    n++;
                    if (dots < mn) mn = dots;
                    if (dots > mx) mx = dots;
                    /* settled: 10 s after lock */
                    if (f >= locked_at + 600) {
                        ssum += dots;
                        sn++;
                        if (dots < smn) smn = dots;
                        if (dots > smx) smx = dots;
                        if (fabs(dots - prev[c]) > jump) jump = fabs(dots - prev[c]);
                    }
                    prev[c] = dots;
                }
            }
        }
next:
        frame_mc += len;
    }

    double mean = n ? sum / n : 0;
    if (s->reset_at) printf("%s relock_at=%d\n", s->name, relock_at);
    double smean = sn ? ssum / sn : 0;
    printf("%s locked_at=%d mean=%.2f spread=%.2f settled_spread=%.2f settled_below=%.2f settled_above=%.2f "
           "max_jump=%.2f samples=%ld resets=%u\n",
           s->name, locked_at, mean, mx - mn, smx - smn, smean - smn, smx - smean, jump, n, bt.resets);
}

/* The scope's report, poll by poll, against MAME's snes_sscope */
static int report_tests(void) {
    const struct {
        const char *what;
        uint8_t held[6];
        uint8_t want[6];
        int n;
    } cases[] = {
        {"fire single shot", {SSCOPE_FIRE, SSCOPE_FIRE, SSCOPE_FIRE, 0, SSCOPE_FIRE},
                             {SSCOPE_FIRE, 0, 0, 0, SSCOPE_FIRE}, 5},
        {"cursor while held", {SSCOPE_CURSOR, SSCOPE_CURSOR, 0}, {SSCOPE_CURSOR, SSCOPE_CURSOR, 0}, 3},
        {"pause on press", {SSCOPE_PAUSE, SSCOPE_PAUSE, 0, SSCOPE_PAUSE}, {SSCOPE_PAUSE, 0, 0, SSCOPE_PAUSE}, 4},
        {"turbo switch flips on press, fire repeats",
            {SSCOPE_TURBO, SSCOPE_TURBO, SSCOPE_FIRE, SSCOPE_FIRE, SSCOPE_TURBO, SSCOPE_FIRE},
            {SSCOPE_TURBO, SSCOPE_TURBO, SSCOPE_TURBO | SSCOPE_FIRE, SSCOPE_TURBO | SSCOPE_FIRE, 0, SSCOPE_FIRE}, 6},
        {"off the screen", {SSCOPE_OFFSCREEN | SSCOPE_FIRE, 0}, {SSCOPE_OFFSCREEN | SSCOPE_FIRE, 0}, 2},
    };
    int bad = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        struct sscope_state st = {0};
        for (int i = 0; i < cases[c].n; i++) {
            uint8_t got = sscope_report(&st, cases[c].held[i]);
            if (got != cases[c].want[i]) {
                printf("report %s: poll %d held %02X got %02X want %02X\n", cases[c].what, i,
                       cases[c].held[i], got, cases[c].want[i]);
                bad = 1;
            }
        }
    }
    printf("report %s\n", bad ? "FAIL" : "ok");
    return bad;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "report")) {
        return report_tests();
    }
    const struct scenario sc[] = {
        {"ntsc_auto",           0, 0,   0,   0,  0,   0,    0, 20000},
        {"ntsc_auto_real",      0, 0,  50, -50, 40, 0.02,   0, 20000},
        {"ntsc_auto_slow_cpu",  0, 0, -50,  50, 40, 0.02,   0, 20000},
        {"pal_auto_real",       1, 0,  50, -50, 40, 0.02,   0, 20000},
        {"ntsc_manual_10",      0, 10, 50, -50, 40, 0.02,   0, 20000},
        {"ntsc_reset",          0, 0,  50, -50, 40, 0.02, 5000, 10000},
    };
    for (size_t i = 0; i < sizeof(sc) / sizeof(sc[0]); i++) {
        if (argc > 1 && strcmp(argv[1], sc[i].name)) {
            continue;
        }
        run(&sc[i]);
    }
    return 0;
}

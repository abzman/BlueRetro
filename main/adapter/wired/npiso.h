/*
 * Copyright (c) 2019-2025, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _NPISO_H_
#define _NPISO_H_
#include "adapter/adapter.h"
#include "adapter/config.h"
#include "sscope_report.h"

/* SNES Super Scope: Gamepad Alt on the SNES's 2nd port, the only one whose pin 6 (IOBit) reaches
 * the PPU's H/V counter latch. The adapter keeps what the gun holds and where it points; the
 * driver (snes_spi.c) turns that into the scope's report at each poll and pulls IOBit low when
 * the beam passes the aim. */
#define NPISO_SCOPE_PORT 1
#define NPISO_SCOPE_FIRE SSCOPE_FIRE           /* bits as in the scope's report */
#define NPISO_SCOPE_CURSOR SSCOPE_CURSOR
#define NPISO_SCOPE_TURBO SSCOPE_TURBO         /* here: the button that flips the turbo switch */
#define NPISO_SCOPE_PAUSE SSCOPE_PAUSE
#define NPISO_SCOPE_OFFSCREEN SSCOPE_OFFSCREEN /* the report's null flag: no screen in sight */
#define NPISO_SCOPE_W 256           /* aim range: the picture, in dots and lines */
#define NPISO_SCOPE_H 224
#define NPISO_SCOPE_PTR_MIN (-512)  /* LX/LY range the adapter scales a pointer to */
#define NPISO_SCOPE_PTR_MAX 511

struct npiso_scope_map {
    uint8_t held;       /* NPISO_SCOPE_* held now */
    uint8_t reserved[3];
    uint32_t xy;        /* x in bits 0-15, y in 16-31: one load for the driver */
} __packed;

static inline uint32_t npiso_is_scope(uint32_t index) {
    return wired_adapter.system_id == SNES && index == NPISO_SCOPE_PORT
        && config.out_cfg[NPISO_SCOPE_PORT].dev_mode == DEV_PAD_ALT;
}

void npiso_meta_init(struct wired_ctrl *ctrl_data);
void npiso_init_buffer(int32_t dev_mode, struct wired_data *wired_data);
void npiso_from_generic(int32_t dev_mode, struct wired_ctrl *ctrl_data, struct wired_data *wired_data);
void npiso_fb_to_generic(int32_t dev_mode, struct raw_fb *raw_fb_data, struct generic_fb *fb_data);
void npiso_gen_turbo_mask(struct wired_data *wired_data);

#endif /* _NPISO_H_ */

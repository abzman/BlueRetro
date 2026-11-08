/*
 * Copyright (c) 2019-2022, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _PS_H_
#define _PS_H_
#include "adapter/adapter.h"
#include "adapter/config.h"

/* Namco GunCon (NPC-103), ID 0x63: Mouse mode with the rumble accessory (the rumble does nothing
 * on a mouse), and with both accessories it keeps the memory card. It reports the trigger and
 * A/B buttons in the pad's bit positions, then X as 8 MHz clocks since HSYNC and Y as scanlines
 * since VSYNC: the gun times the beam itself, so the console only reads numbers. */
#define PS_GUNCON_ID 0x63
#define PS_GUNCON_X_MIN 0x4D        /* left edge */
#define PS_GUNCON_X_MAX 0x1CD       /* right edge */
#define PS_GUNCON_Y_MIN 0x19        /* top edge, NTSC */
#define PS_GUNCON_Y_MAX 0xF8        /* bottom edge, NTSC */
#define PS_GUNCON_X_OFFSCREEN 0x01  /* no light seen */
#define PS_GUNCON_Y_OFFSCREEN 0x0A
#define PS_GUNCON_PTR_MIN (-512)    /* LX/LY range the adapter scales a pointer to */
#define PS_GUNCON_PTR_MAX 511

static inline uint32_t ps_is_guncon(uint32_t index) {
    return config.out_cfg[index].dev_mode == DEV_MOUSE && (config.out_cfg[index].acc_mode & ACC_RUMBLE);
}

/* The 6 bytes a GunCon answers a poll with, kept as wired_data output: buttons (active low), then
 * X and Y together so the SPI side reads a position in one load */
struct ps_guncon_map {
    uint16_t buttons;
    uint16_t reserved;
    uint32_t xy;    /* X in bits 0-15, Y in bits 16-31 */
} __packed;

void ps_meta_init(struct wired_ctrl *ctrl_data);
void ps_init_buffer(int32_t dev_mode, struct wired_data *wired_data);
void ps_from_generic(int32_t dev_mode, struct wired_ctrl *ctrl_data, struct wired_data *wired_data);
void ps_kb_id_to_scancode(uint32_t dev_id, uint8_t type, uint8_t id);
void ps_fb_to_generic(int32_t dev_mode, struct raw_fb *raw_fb_data, struct generic_fb *fb_data);
void ps_gen_turbo_mask(struct wired_data *wired_data);

#endif /* _PS_H_ */

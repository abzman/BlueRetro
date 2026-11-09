/*
 * Copyright (c) 2026, BlueRetro contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "ogxmini_pad.h"

#define NO_ANALOG -1
#define NO_TRIG -1

/* By generic id: the PAD_* order of adapter/adapter.h */
const struct ogxm_btn_map ogxm_btn_map[OGXM_GEN_BTN_CNT] = {
    /* PAD_LX_LEFT .. PAD_RY_UP: sticks, see ogxm_pad_turbo() */
    {0, 0, NO_ANALOG, NO_TRIG}, {0, 0, NO_ANALOG, NO_TRIG},
    {0, 0, NO_ANALOG, NO_TRIG}, {0, 0, NO_ANALOG, NO_TRIG},
    {0, 0, NO_ANALOG, NO_TRIG}, {0, 0, NO_ANALOG, NO_TRIG},
    {0, 0, NO_ANALOG, NO_TRIG}, {0, 0, NO_ANALOG, NO_TRIG},
    /* PAD_LD_LEFT, PAD_LD_RIGHT, PAD_LD_DOWN, PAD_LD_UP */
    {0, OGXM_DPAD_LEFT, OGXM_ANALOG_LEFT, NO_TRIG},
    {0, OGXM_DPAD_RIGHT, OGXM_ANALOG_RIGHT, NO_TRIG},
    {0, OGXM_DPAD_DOWN, OGXM_ANALOG_DOWN, NO_TRIG},
    {0, OGXM_DPAD_UP, OGXM_ANALOG_UP, NO_TRIG},
    /* PAD_RD_*: no right D-pad */
    {0, 0, NO_ANALOG, NO_TRIG}, {0, 0, NO_ANALOG, NO_TRIG},
    {0, 0, NO_ANALOG, NO_TRIG}, {0, 0, NO_ANALOG, NO_TRIG},
    /* PAD_RB_LEFT, PAD_RB_RIGHT, PAD_RB_DOWN, PAD_RB_UP */
    {OGXM_BTN_X, 0, OGXM_ANALOG_X, NO_TRIG},
    {OGXM_BTN_B, 0, OGXM_ANALOG_B, NO_TRIG},
    {OGXM_BTN_A, 0, OGXM_ANALOG_A, NO_TRIG},
    {OGXM_BTN_Y, 0, OGXM_ANALOG_Y, NO_TRIG},
    /* PAD_MM (Start), PAD_MS (Select), PAD_MT (Home), PAD_MQ (Capture) */
    {OGXM_BTN_START, 0, NO_ANALOG, NO_TRIG},
    {OGXM_BTN_BACK, 0, NO_ANALOG, NO_TRIG},
    {OGXM_BTN_SYS, 0, NO_ANALOG, NO_TRIG},
    {OGXM_BTN_MISC, 0, NO_ANALOG, NO_TRIG},
    /* PAD_LM, PAD_LS, PAD_LT, PAD_LJ */
    {0, 0, NO_ANALOG, 0},
    {OGXM_BTN_LB, 0, OGXM_ANALOG_LB, NO_TRIG},
    {0, 0, NO_ANALOG, NO_TRIG},
    {OGXM_BTN_L3, 0, NO_ANALOG, NO_TRIG},
    /* PAD_RM, PAD_RS, PAD_RT, PAD_RJ */
    {0, 0, NO_ANALOG, 1},
    {OGXM_BTN_RB, 0, OGXM_ANALOG_RB, NO_TRIG},
    {0, 0, NO_ANALOG, NO_TRIG},
    {OGXM_BTN_R3, 0, NO_ANALOG, NO_TRIG},
};

/* The turbo rule of every BlueRetro output (wired_gen_turbo_mask_*): cnt_mask is the period
 * mask shifted left once, with bit 0 picking which half of the period is the on half */
static int32_t turbo_off(uint8_t cnt_mask, uint32_t frame_cnt) {
    uint8_t mask = cnt_mask >> 1;

    if (!mask) {
        return 0;
    }
    if (cnt_mask & 1) {
        return !(mask & frame_cnt);
    }
    return (mask & frame_cnt) != mask;
}

void ogxm_pad_turbo(struct ogxm_pad *pad, const uint8_t *cnt_mask, uint32_t frame_cnt) {
    for (uint32_t i = 0; i < OGXM_GEN_BTN_CNT; i++) {
        if (!turbo_off(cnt_mask[i], frame_cnt)) {
            continue;
        }
        switch (i) {
            case OGXM_GEN_LX_LEFT:
                pad->joystick_lx = 0;
                continue;
            case OGXM_GEN_LY_DOWN:
                pad->joystick_ly = 0;
                continue;
            case OGXM_GEN_RX_LEFT:
                pad->joystick_rx = 0;
                continue;
            case OGXM_GEN_RY_DOWN:
                pad->joystick_ry = 0;
                continue;
        }
        const struct ogxm_btn_map *map = &ogxm_btn_map[i];

        pad->buttons &= ~map->buttons;
        pad->dpad &= ~map->dpad;
        if (map->analog >= 0) {
            pad->analog[map->analog] = 0;
        }
        if (map->trigger == 0) {
            pad->trigger_l = 0;
        }
        else if (map->trigger == 1) {
            pad->trigger_r = 0;
        }
    }
}

void ogxm_pad_fill_analog(struct ogxm_pad *pad) {
    for (uint32_t i = 0; i < OGXM_GEN_BTN_CNT; i++) {
        const struct ogxm_btn_map *map = &ogxm_btn_map[i];

        if (map->analog < 0) {
            continue;
        }
        if ((pad->buttons & map->buttons) || (pad->dpad & map->dpad)) {
            if (!pad->analog[map->analog]) {
                pad->analog[map->analog] = 0xFF;
            }
        }
        else {
            pad->analog[map->analog] = 0;
        }
    }
}

void ogxm_packet_set_pad(struct ogxm_packet_in *pkt, const struct ogxm_pad *pad) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->packet_len = sizeof(*pkt);
    pkt->packet_id = OGXM_PKT_SET_PAD;
    /* One gamepad per Pico: its I2C build has MAX_GAMEPADS 1 */
    pkt->index = 0;
    memcpy(&pkt->pad, pad, sizeof(pkt->pad));
}

void ogxm_packet_set_driver(struct ogxm_packet_in *pkt, uint8_t device_type) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->packet_len = sizeof(*pkt);
    pkt->packet_id = OGXM_PKT_SET_DRIVER;
    pkt->device_type = device_type;
}

int32_t ogxm_packet_out_parse(const uint8_t *buf, uint32_t len, uint8_t *rumble_l, uint8_t *rumble_r) {
    const struct ogxm_packet_out *out = (const struct ogxm_packet_out *)buf;

    if (len < sizeof(*out) || out->packet_len != sizeof(*out) || out->packet_id != OGXM_PKT_GET_PAD) {
        return -1;
    }
    *rumble_l = out->rumble_l;
    *rumble_r = out->rumble_r;
    return 0;
}

uint8_t ogxm_driver_from_dev_mode(int32_t dev_mode) {
    /* DEV_PAD, DEV_PAD_ALT, DEV_KB, DEV_MOUSE: the four choices of an output's mode */
    static const uint8_t drv[] = {
        OGXM_DRV_XBOXOG, OGXM_DRV_XINPUT, OGXM_DRV_PS3, OGXM_DRV_SWITCH,
    };

    if (dev_mode < 0 || dev_mode >= (int32_t)(sizeof(drv) / sizeof(drv[0]))) {
        return OGXM_DRV_XBOXOG;
    }
    return drv[dev_mode];
}

/*
 * Copyright (c) 2026, BlueRetro contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/* Gamepad output for an OGX-Mini's Pico (wired/ogxmini_i2c.c): one Pico per output, which shows
 * up on its USB port as the controller its mode says (original Xbox, XInput, PS3, Switch). */

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "zephyr/types.h"
#include "tools/util.h"
#include "adapter/config.h"
#include "adapter/wired/wired.h"
#include "wired/ogxmini_i2c.h"
#include "tests/cmds.h"
#include "bluetooth/mon.h"
#include "ogxmini.h"

#define OGXM_STICK_CNT 4

_Static_assert(OGXM_GEN_LX_LEFT == PAD_LX_LEFT && OGXM_GEN_LY_DOWN == PAD_LY_DOWN
    && OGXM_GEN_RX_LEFT == PAD_RX_LEFT && OGXM_GEN_RY_DOWN == PAD_RY_DOWN, "generic stick ids");
_Static_assert(PAD_LD_LEFT == 8 && PAD_RB_LEFT == 16 && PAD_MM == 20 && PAD_LM == 24
    && PAD_RJ == 31, "generic button ids, see ogxm_btn_map");

/* Where each generic axis goes: sticks and triggers have their own fields, the other ten are
 * the pressure of a digital button */
enum {
    OGXM_DST_STICK = 0,
    OGXM_DST_TRIG,
    OGXM_DST_ANALOG,
};

static const struct {
    uint8_t type;
    uint8_t idx;
} ogxm_axes_dst[ADAPTER_PS2_MAX_AXES] = {
    /* AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY */
    {OGXM_DST_STICK, 0}, {OGXM_DST_STICK, 1}, {OGXM_DST_STICK, 2}, {OGXM_DST_STICK, 3},
    /* TRIG_L, TRIG_R */
    {OGXM_DST_TRIG, 0}, {OGXM_DST_TRIG, 1},
    /* TRIG_LS, TRIG_RS: LB, RB */
    {OGXM_DST_ANALOG, OGXM_ANALOG_LB}, {OGXM_DST_ANALOG, OGXM_ANALOG_RB},
    /* DPAD_L, DPAD_R, DPAD_D, DPAD_U */
    {OGXM_DST_ANALOG, OGXM_ANALOG_LEFT}, {OGXM_DST_ANALOG, OGXM_ANALOG_RIGHT},
    {OGXM_DST_ANALOG, OGXM_ANALOG_DOWN}, {OGXM_DST_ANALOG, OGXM_ANALOG_UP},
    /* BTN_L, BTN_R, BTN_D, BTN_U: X, B, A, Y */
    {OGXM_DST_ANALOG, OGXM_ANALOG_X}, {OGXM_DST_ANALOG, OGXM_ANALOG_B},
    {OGXM_DST_ANALOG, OGXM_ANALOG_A}, {OGXM_DST_ANALOG, OGXM_ANALOG_Y},
};

/* Sticks are 16 bits with Y down positive (polarity), as the Pico takes them */
static DRAM_ATTR const struct ctrl_meta ogxm_axes_meta[ADAPTER_PS2_MAX_AXES] =
{
    {.size_min = -32768, .size_max = 32767, .neutral = 0, .abs_max = 32767, .abs_min = 32768},
    {.size_min = -32768, .size_max = 32767, .neutral = 0, .abs_max = 32767, .abs_min = 32768, .polarity = 1},
    {.size_min = -32768, .size_max = 32767, .neutral = 0, .abs_max = 32767, .abs_min = 32768},
    {.size_min = -32768, .size_max = 32767, .neutral = 0, .abs_max = 32767, .abs_min = 32768, .polarity = 1},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
    {.size_min = 0, .size_max = 255, .neutral = 0x00, .abs_max = 0x100, .abs_min = 0x00},
};

/* Every button of an Xbox pad, Capture as MISC (the Series' Share). The D-pad, face buttons,
 * bumpers and triggers are axes, so a pressure source (a DS3) keeps its pressure. */
static const uint32_t ogxm_mask[4] = {0xBBFF0FFF, 0x00000000, 0x00000000, BR_COMBO_MASK};
static const uint32_t ogxm_desc[4] = {0x330F0FFF, 0x00000000, 0x00000000, 0x00000000};

/* The driver task reads the pad while the Bluetooth task writes it */
static portMUX_TYPE ogxm_mux = portMUX_INITIALIZER_UNLOCKED;

void ogxmini_init_buffer(int32_t dev_mode, struct wired_data *wired_data) {
    struct ogxm_pad *map = (struct ogxm_pad *)wired_data->output;

    taskENTER_CRITICAL(&ogxm_mux);
    memset((void *)map, 0, sizeof(*map));
    taskEXIT_CRITICAL(&ogxm_mux);
    memset(wired_data->output_mask, 0xFF, sizeof(struct ogxm_pad));
}

void ogxmini_meta_init(struct wired_ctrl *ctrl_data) {
    memset((void *)ctrl_data, 0, sizeof(*ctrl_data)*WIRED_MAX_DEV);

    /* Any output mode is a gamepad: the mode only picks what the Pico shows up as */
    for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
        ctrl_data[i].mask = ogxm_mask;
        ctrl_data[i].desc = ogxm_desc;
        for (uint32_t j = 0; j < ADAPTER_PS2_MAX_AXES; j++) {
            ctrl_data[i].axes[j].meta = &ogxm_axes_meta[j];
        }
    }
}

static int32_t ogxm_axis_value(struct ctrl_axis_wired *axis) {
    if (axis->value > axis->meta->size_max) {
        return axis->meta->size_max;
    }
    if (axis->value < axis->meta->size_min) {
        return axis->meta->size_min;
    }
    return axis->value + axis->meta->neutral;
}

void ogxmini_from_generic(int32_t dev_mode, struct wired_ctrl *ctrl_data, struct wired_data *wired_data) {
    struct ogxm_pad map_tmp;
    int16_t *sticks = &map_tmp.joystick_lx;

    if (ctrl_data->index >= OGXM_MAX_PICO) {
        return;
    }

    memcpy((void *)&map_tmp, wired_data->output, sizeof(map_tmp));

    for (uint32_t i = 0; i < ARRAY_SIZE(generic_btns_mask); i++) {
        if (ctrl_data->map_mask[0] & BIT(i)) {
            const struct ogxm_btn_map *dst = &ogxm_btn_map[i];

            if (ctrl_data->btns[0].value & generic_btns_mask[i]) {
                map_tmp.buttons |= dst->buttons;
                map_tmp.dpad |= dst->dpad;
                wired_data->cnt_mask[i] = ctrl_data->btns[0].cnt_mask[i];
            }
            else {
                map_tmp.buttons &= ~dst->buttons;
                map_tmp.dpad &= ~dst->dpad;
                wired_data->cnt_mask[i] = 0;
            }
        }
    }

    for (uint32_t i = 0; i < ADAPTER_PS2_MAX_AXES; i++) {
        uint32_t btn_id = axis_to_btn_id(i);
        uint32_t btn_mask = axis_to_btn_mask(i);

        if (ctrl_data->map_mask[0] & (btn_mask & ogxm_desc[0])) {
            int32_t value = ogxm_axis_value(&ctrl_data->axes[i]);

            switch (ogxm_axes_dst[i].type) {
                case OGXM_DST_STICK:
                    sticks[ogxm_axes_dst[i].idx] = (int16_t)value;
                    break;
                case OGXM_DST_TRIG:
                    if (ogxm_axes_dst[i].idx) {
                        map_tmp.trigger_r = (uint8_t)value;
                    }
                    else {
                        map_tmp.trigger_l = (uint8_t)value;
                    }
                    break;
                default:
                {
                    const struct ogxm_btn_map *dst = &ogxm_btn_map[btn_id];

                    /* The pressure, and the button it belongs to */
                    map_tmp.analog[ogxm_axes_dst[i].idx] = (uint8_t)value;
                    if (value) {
                        map_tmp.buttons |= dst->buttons;
                        map_tmp.dpad |= dst->dpad;
                    }
                    else {
                        map_tmp.buttons &= ~dst->buttons;
                        map_tmp.dpad &= ~dst->dpad;
                    }
                    break;
                }
            }
        }
        wired_data->cnt_mask[btn_id] = ctrl_data->axes[i].cnt_mask;
    }

    ogxm_pad_fill_analog(&map_tmp);

    taskENTER_CRITICAL(&ogxm_mux);
    memcpy(wired_data->output, (void *)&map_tmp, sizeof(map_tmp));
    taskEXIT_CRITICAL(&ogxm_mux);

    ogxmini_i2c_kick(ctrl_data->index);

    TESTS_CMDS_LOG("\"wired_output\": {\"dpad\": %d, \"btns\": %d, \"triggers\": [%d, %d], "
        "\"axes\": [%d, %d, %d, %d], \"analog\": [%d, %d, %d, %d, %d, %d, %d, %d, %d, %d]},\n",
        map_tmp.dpad, map_tmp.buttons, map_tmp.trigger_l, map_tmp.trigger_r,
        map_tmp.joystick_lx, map_tmp.joystick_ly, map_tmp.joystick_rx, map_tmp.joystick_ry,
        map_tmp.analog[0], map_tmp.analog[1], map_tmp.analog[2], map_tmp.analog[3], map_tmp.analog[4],
        map_tmp.analog[5], map_tmp.analog[6], map_tmp.analog[7], map_tmp.analog[8], map_tmp.analog[9]);
    BT_MON_LOG("\"wired_output\": {\"dpad\": %02X, \"btns\": %04X, \"triggers\": [%02X, %02X], "
        "\"axes\": [%04X, %04X, %04X, %04X]},\n",
        map_tmp.dpad, map_tmp.buttons, map_tmp.trigger_l, map_tmp.trigger_r,
        (uint16_t)map_tmp.joystick_lx, (uint16_t)map_tmp.joystick_ly,
        (uint16_t)map_tmp.joystick_rx, (uint16_t)map_tmp.joystick_ry);
}

/* The pad as the next packet carries it: turbo applied at the driver's frame count */
void ogxmini_get_pad(struct wired_data *wired_data, struct ogxm_pad *pad) {
    taskENTER_CRITICAL(&ogxm_mux);
    memcpy(pad, wired_data->output, sizeof(*pad));
    taskEXIT_CRITICAL(&ogxm_mux);

    ogxm_pad_turbo(pad, wired_data->cnt_mask, wired_data->frame_cnt);
}

void ogxmini_fb_to_generic(int32_t dev_mode, struct raw_fb *raw_fb_data, struct generic_fb *fb_data) {
    uint32_t wired_id = raw_fb_data->header.wired_id;

    fb_data->wired_id = wired_id;
    fb_data->type = raw_fb_data->header.type;

    switch (fb_data->type) {
        case FB_TYPE_RUMBLE:
            /* data_len 0: BlueRetro's own stop (its rumble timeout). Otherwise the Pico's two
             * motors, any strength, passed on to the controller with the Rumble accessory set. */
            if (raw_fb_data->header.data_len == 0 || wired_id >= WIRED_MAX_DEV
                    || !(config.out_cfg[wired_id].acc_mode & ACC_RUMBLE)) {
                fb_data->state = 0;
                fb_data->lf_pwr = fb_data->hf_pwr = 0;
            }
            else {
                fb_data->state = (raw_fb_data->data[0] || raw_fb_data->data[1]) ? 1 : 0;
                fb_data->lf_pwr = raw_fb_data->data[0];
                fb_data->hf_pwr = raw_fb_data->data[1];
            }
            break;
    }
}

/*
 * Copyright (c) 2026, BlueRetro contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/* The OGX-Mini I2C protocol, as the Pico of an OGX-Mini "ESP32 Bluepad32 I2C" build speaks it
 * (Firmware/RP2040/src/OGXMini/Board/ESP32_Bluepad32_I2C.cpp): the ESP32 is the master, the Pico a
 * slave at 0x01. Every write is a 32-byte PacketIn; a read after a write returns the 8-byte
 * PacketOut (rumble) of that write's pad index.
 *
 * Plain C with no ESP-IDF headers, so the host test (tests/ogxmini_pad_test.c) builds it as is. */

#ifndef _OGXMINI_PAD_H_
#define _OGXMINI_PAD_H_

#include <stdint.h>

#define OGXM_I2C_ADDR_BASE 0x01
#define OGXM_MAX_PICO 4

/* PacketID */
enum {
    OGXM_PKT_UNKNOWN = 0,
    OGXM_PKT_SET_PAD,
    OGXM_PKT_GET_PAD,
    OGXM_PKT_SET_DRIVER,
};

/* DeviceDriverType: the Pico's USB output mode */
enum {
    OGXM_DRV_NONE = 0,
    OGXM_DRV_XBOXOG,
    OGXM_DRV_XBOXOG_SB,
    OGXM_DRV_XBOXOG_XR,
    OGXM_DRV_XINPUT,
    OGXM_DRV_PS3,
    OGXM_DRV_DINPUT,
    OGXM_DRV_PSCLASSIC,
    OGXM_DRV_SWITCH,
};

/* Gamepad::PadIn */
#define OGXM_DPAD_UP    0x01
#define OGXM_DPAD_DOWN  0x02
#define OGXM_DPAD_LEFT  0x04
#define OGXM_DPAD_RIGHT 0x08

#define OGXM_BTN_A      0x0001
#define OGXM_BTN_B      0x0002
#define OGXM_BTN_X      0x0004
#define OGXM_BTN_Y      0x0008
#define OGXM_BTN_L3     0x0010
#define OGXM_BTN_R3     0x0020
#define OGXM_BTN_BACK   0x0040
#define OGXM_BTN_START  0x0080
#define OGXM_BTN_LB     0x0100
#define OGXM_BTN_RB     0x0200
#define OGXM_BTN_SYS    0x0400
#define OGXM_BTN_MISC   0x0800

/* Pressure of the digital buttons, in Gamepad::PadIn.analog */
enum {
    OGXM_ANALOG_UP = 0,
    OGXM_ANALOG_DOWN,
    OGXM_ANALOG_LEFT,
    OGXM_ANALOG_RIGHT,
    OGXM_ANALOG_A,
    OGXM_ANALOG_B,
    OGXM_ANALOG_X,
    OGXM_ANALOG_Y,
    OGXM_ANALOG_LB,
    OGXM_ANALOG_RB,
    OGXM_ANALOG_MAX,
};

struct ogxm_pad {
    uint8_t dpad;
    uint16_t buttons;
    uint8_t trigger_l;
    uint8_t trigger_r;
    /* Y is down positive, as Bluepad32 has it: the Pico inverts it for the Xbox */
    int16_t joystick_lx;
    int16_t joystick_ly;
    int16_t joystick_rx;
    int16_t joystick_ry;
    uint8_t analog[OGXM_ANALOG_MAX];
} __attribute__((packed));

struct ogxm_packet_in {
    uint8_t packet_len;
    uint8_t packet_id;
    uint8_t index;
    uint8_t device_type;
    struct ogxm_pad pad;
    uint8_t reserved[5];
} __attribute__((packed));

struct ogxm_packet_out {
    uint8_t packet_len;
    uint8_t packet_id;
    uint8_t index;
    uint8_t rumble_l;
    uint8_t rumble_r;
    uint8_t reserved[3];
} __attribute__((packed));

_Static_assert(sizeof(struct ogxm_pad) == 23, "PadIn size");
_Static_assert(sizeof(struct ogxm_packet_in) == 32, "PacketIn size");
_Static_assert(sizeof(struct ogxm_packet_out) == 8, "PacketOut size");

/* The BlueRetro generic button ids (PAD_* in adapter/adapter.h) this file needs; ogxmini.c
 * checks they still match. */
#define OGXM_GEN_LX_LEFT  0
#define OGXM_GEN_LY_DOWN  2
#define OGXM_GEN_RX_LEFT  4
#define OGXM_GEN_RY_DOWN  6
#define OGXM_GEN_BTN_CNT  32

/* What one generic button drives on the pad. Turbo releases all of it on its off frames. */
struct ogxm_btn_map {
    uint16_t buttons;
    uint8_t dpad;
    int8_t analog;  /* index in analog[], -1 if none */
    int8_t trigger; /* 0 left, 1 right, -1 if none */
};

extern const struct ogxm_btn_map ogxm_btn_map[OGXM_GEN_BTN_CNT];

/* Releases, on frame frame_cnt, every button whose turbo (cnt_mask, by generic id, as
 * wired_data->cnt_mask holds it) is in its off phase: its bits and pressure go to 0, a trigger to
 * 0 and a stick to center. */
void ogxm_pad_turbo(struct ogxm_pad *pad, const uint8_t *cnt_mask, uint32_t frame_cnt);

/* Pressure of the buttons that have none: a pressed button with a 0 byte reads 255, a released
 * one 0. A pressure source (a DS3) keeps its own value. */
void ogxm_pad_fill_analog(struct ogxm_pad *pad);

void ogxm_packet_set_pad(struct ogxm_packet_in *pkt, const struct ogxm_pad *pad);
void ogxm_packet_set_driver(struct ogxm_packet_in *pkt, uint8_t device_type);

/* Returns 0 and the rumble if buf is a PacketOut, -1 otherwise (a Pico that is busy or rebooting
 * answers garbage or 0xFF). */
int32_t ogxm_packet_out_parse(const uint8_t *buf, uint32_t len, uint8_t *rumble_l, uint8_t *rumble_r);

/* The Pico mode for a BlueRetro output mode (DEV_PAD, DEV_PAD_ALT, DEV_KB, DEV_MOUSE) */
uint8_t ogxm_driver_from_dev_mode(int32_t dev_mode);

#endif /* _OGXMINI_PAD_H_ */

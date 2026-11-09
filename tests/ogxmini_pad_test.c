/* Host test of main/adapter/wired/ogxmini_pad.c, built and run by pytest_ogxmini.py: the packets
 * against the layout of OGX-Mini's own structs, the turbo, the 0/255 pressure fill and the
 * PacketOut check. Prints "ok" or what failed. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ogxmini_pad.h"

/* OGX-Mini's structs as declared (Firmware/RP2040/src/Gamepad/Gamepad.h and
 * Firmware/RP2040/src/OGXMini/Board/ESP32_Bluepad32_I2C.cpp, #pragma pack(1)), in C */
#pragma pack(push, 1)
struct PadIn {
    uint8_t dpad;
    uint16_t buttons;
    uint8_t trigger_l;
    uint8_t trigger_r;
    int16_t joystick_lx;
    int16_t joystick_ly;
    int16_t joystick_rx;
    int16_t joystick_ry;
    uint8_t analog[10];
};
struct PacketIn {
    uint8_t packet_len;
    uint8_t packet_id;
    uint8_t index;
    uint8_t device_type;
    struct PadIn pad_in;
    uint8_t reserved[5];
};
struct PadOut {
    uint8_t rumble_l;
    uint8_t rumble_r;
};
struct PacketOut {
    uint8_t packet_len;
    uint8_t packet_id;
    uint8_t index;
    struct PadOut pad_out;
    uint8_t reserved[3];
};
#pragma pack(pop)

static int fails;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)
#define SAME_OFFSET(a, fa, b, fb) CHECK(offsetof(a, fa) == offsetof(b, fb))

/* Generic ids, from PAD_* in adapter/adapter.h */
enum {
    LD_LEFT = 8, LD_RIGHT, LD_DOWN, LD_UP,
    RB_LEFT = 16, RB_RIGHT, RB_DOWN, RB_UP,
    MM, MS, MT, MQ,
    LM, LS, LT, LJ,
    RM, RS, RT, RJ,
};

static void test_layout(void) {
    CHECK(sizeof(struct ogxm_packet_in) == sizeof(struct PacketIn));
    CHECK(sizeof(struct ogxm_packet_out) == sizeof(struct PacketOut));
    CHECK(sizeof(struct ogxm_pad) == sizeof(struct PadIn));
    SAME_OFFSET(struct ogxm_packet_in, packet_id, struct PacketIn, packet_id);
    SAME_OFFSET(struct ogxm_packet_in, index, struct PacketIn, index);
    SAME_OFFSET(struct ogxm_packet_in, device_type, struct PacketIn, device_type);
    SAME_OFFSET(struct ogxm_packet_in, pad, struct PacketIn, pad_in);
    SAME_OFFSET(struct ogxm_packet_in, reserved, struct PacketIn, reserved);
    SAME_OFFSET(struct ogxm_pad, buttons, struct PadIn, buttons);
    SAME_OFFSET(struct ogxm_pad, trigger_l, struct PadIn, trigger_l);
    SAME_OFFSET(struct ogxm_pad, trigger_r, struct PadIn, trigger_r);
    SAME_OFFSET(struct ogxm_pad, joystick_lx, struct PadIn, joystick_lx);
    SAME_OFFSET(struct ogxm_pad, joystick_ly, struct PadIn, joystick_ly);
    SAME_OFFSET(struct ogxm_pad, joystick_rx, struct PadIn, joystick_rx);
    SAME_OFFSET(struct ogxm_pad, joystick_ry, struct PadIn, joystick_ry);
    SAME_OFFSET(struct ogxm_pad, analog, struct PadIn, analog);
    SAME_OFFSET(struct ogxm_packet_out, rumble_l, struct PacketOut, pad_out.rumble_l);
    SAME_OFFSET(struct ogxm_packet_out, rumble_r, struct PacketOut, pad_out.rumble_r);
}

static void test_packets(void) {
    struct ogxm_packet_in pkt;
    struct ogxm_pad pad = {0};

    memset(&pkt, 0xAA, sizeof(pkt));
    pad.buttons = OGXM_BTN_A;
    pad.joystick_ly = -1234;
    ogxm_packet_set_pad(&pkt, &pad);
    CHECK(pkt.packet_len == 32 && pkt.packet_id == OGXM_PKT_SET_PAD && pkt.index == 0);
    CHECK(pkt.device_type == OGXM_DRV_NONE);
    CHECK(pkt.pad.buttons == OGXM_BTN_A && pkt.pad.joystick_ly == -1234);
    CHECK(pkt.reserved[0] == 0 && pkt.reserved[4] == 0);

    ogxm_packet_set_driver(&pkt, OGXM_DRV_XINPUT);
    CHECK(pkt.packet_len == 32 && pkt.packet_id == OGXM_PKT_SET_DRIVER && pkt.device_type == 4);
    CHECK(pkt.pad.buttons == 0);

    /* Driver ids as in DeviceDriverTypes.h */
    CHECK(ogxm_driver_from_dev_mode(0) == 1);   /* DEV_PAD: XBOXOG */
    CHECK(ogxm_driver_from_dev_mode(1) == 4);   /* DEV_PAD_ALT: XINPUT */
    CHECK(ogxm_driver_from_dev_mode(2) == 5);   /* DEV_KB: PS3 */
    CHECK(ogxm_driver_from_dev_mode(3) == 8);   /* DEV_MOUSE: SWITCH */
    CHECK(ogxm_driver_from_dev_mode(9) == 1);
}

static void test_packet_out(void) {
    uint8_t buf[8] = {8, OGXM_PKT_GET_PAD, 0, 0x40, 0xC0, 0, 0, 0};
    uint8_t l = 0, r = 0;

    CHECK(ogxm_packet_out_parse(buf, sizeof(buf), &l, &r) == 0 && l == 0x40 && r == 0xC0);
    /* A Pico not ready: nothing, or all ones on an idle bus */
    memset(buf, 0xFF, sizeof(buf));
    CHECK(ogxm_packet_out_parse(buf, sizeof(buf), &l, &r) == -1);
    memset(buf, 0, sizeof(buf));
    CHECK(ogxm_packet_out_parse(buf, sizeof(buf), &l, &r) == -1);
    CHECK(ogxm_packet_out_parse(buf, 4, &l, &r) == -1);
}

static void test_fill_analog(void) {
    struct ogxm_pad pad = {0};

    /* Digital press: 255; pressure source: its own value; released: 0 */
    pad.buttons = OGXM_BTN_A | OGXM_BTN_LB | OGXM_BTN_START;
    pad.dpad = OGXM_DPAD_UP;
    pad.analog[OGXM_ANALOG_LB] = 0x30;
    pad.analog[OGXM_ANALOG_B] = 0x55;
    ogxm_pad_fill_analog(&pad);
    CHECK(pad.analog[OGXM_ANALOG_A] == 0xFF);
    CHECK(pad.analog[OGXM_ANALOG_UP] == 0xFF);
    CHECK(pad.analog[OGXM_ANALOG_LB] == 0x30);
    CHECK(pad.analog[OGXM_ANALOG_B] == 0);
    CHECK(pad.analog[OGXM_ANALOG_X] == 0 && pad.analog[OGXM_ANALOG_DOWN] == 0);
}

static struct ogxm_pad full_pad(void) {
    struct ogxm_pad pad;

    memset(&pad, 0, sizeof(pad));
    pad.buttons = 0x0FFF;
    pad.dpad = 0x0F;
    pad.trigger_l = pad.trigger_r = 200;
    pad.joystick_lx = 1000;
    pad.joystick_ly = -2000;
    pad.joystick_rx = 3000;
    pad.joystick_ry = -4000;
    memset(pad.analog, 0x80, sizeof(pad.analog));
    return pad;
}

static void test_turbo(void) {
    uint8_t cnt_mask[OGXM_GEN_BTN_CNT] = {0};
    struct ogxm_pad pad, ref = full_pad();

    /* No turbo: untouched */
    pad = ref;
    ogxm_pad_turbo(&pad, cnt_mask, 1);
    CHECK(!memcmp(&pad, &ref, sizeof(pad)));

    /* A at period 2 (mask 1), on half when frame & 1: released on even frames */
    cnt_mask[RB_DOWN] = (1 << 1) | 1;
    pad = ref;
    ogxm_pad_turbo(&pad, cnt_mask, 1);
    CHECK(!memcmp(&pad, &ref, sizeof(pad)));
    ogxm_pad_turbo(&pad, cnt_mask, 2);
    CHECK(pad.buttons == (ref.buttons & ~OGXM_BTN_A) && pad.analog[OGXM_ANALOG_A] == 0);
    CHECK(pad.analog[OGXM_ANALOG_B] == 0x80 && pad.dpad == 0x0F);

    /* Every button, trigger, D-pad direction and stick on its off frame: a pad at rest */
    for (uint32_t i = 0; i < OGXM_GEN_BTN_CNT; i++) {
        cnt_mask[i] = (1 << 1) | 1;
    }
    pad = ref;
    ogxm_pad_turbo(&pad, cnt_mask, 2);
    CHECK(pad.dpad == 0 && pad.trigger_l == 0 && pad.trigger_r == 0);
    CHECK(pad.joystick_lx == 0 && pad.joystick_ly == 0 && pad.joystick_rx == 0 && pad.joystick_ry == 0);
    for (uint32_t i = 0; i < OGXM_ANALOG_MAX; i++) {
        CHECK(pad.analog[i] == 0);
    }
    CHECK(pad.buttons == 0);

    /* The other phase (bit 0 clear): on only when every mask bit is set in the frame count */
    memset(cnt_mask, 0, sizeof(cnt_mask));
    cnt_mask[LM] = 3 << 1;
    pad = ref;
    ogxm_pad_turbo(&pad, cnt_mask, 3);
    CHECK(pad.trigger_l == 200);
    ogxm_pad_turbo(&pad, cnt_mask, 2);
    CHECK(pad.trigger_l == 0 && pad.trigger_r == 200);
}

static void test_btn_map(void) {
    /* Each pad button comes from exactly one generic button */
    uint16_t seen = 0;
    uint8_t dpad = 0;

    for (uint32_t i = 0; i < OGXM_GEN_BTN_CNT; i++) {
        CHECK(!(seen & ogxm_btn_map[i].buttons));
        CHECK(!(dpad & ogxm_btn_map[i].dpad));
        seen |= ogxm_btn_map[i].buttons;
        dpad |= ogxm_btn_map[i].dpad;
    }
    CHECK(seen == 0x0FFF && dpad == 0x0F);
    CHECK(ogxm_btn_map[RB_DOWN].buttons == OGXM_BTN_A && ogxm_btn_map[RB_RIGHT].buttons == OGXM_BTN_B);
    CHECK(ogxm_btn_map[RB_LEFT].buttons == OGXM_BTN_X && ogxm_btn_map[RB_UP].buttons == OGXM_BTN_Y);
    CHECK(ogxm_btn_map[MM].buttons == OGXM_BTN_START && ogxm_btn_map[MS].buttons == OGXM_BTN_BACK);
    CHECK(ogxm_btn_map[LS].buttons == OGXM_BTN_LB && ogxm_btn_map[RS].buttons == OGXM_BTN_RB);
    CHECK(ogxm_btn_map[LJ].buttons == OGXM_BTN_L3 && ogxm_btn_map[RJ].buttons == OGXM_BTN_R3);
    CHECK(ogxm_btn_map[LM].trigger == 0 && ogxm_btn_map[RM].trigger == 1);
}

int main(void) {
    test_layout();
    test_packets();
    test_packet_out();
    test_fill_analog();
    test_turbo();
    test_btn_map();
    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}

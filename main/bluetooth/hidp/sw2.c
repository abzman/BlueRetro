/*
 * Copyright (c) 2025, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdbool.h>
#include <esp_timer.h>
#include "bluetooth/host.h"
#include "bluetooth/hci.h"
#include "bluetooth/mon.h"
#include "bluetooth/att.h"
#include "zephyr/att.h"
#include "zephyr/gatt.h"
#include "tools/util.h"
#include "tests/cmds.h"
#include "adapter/config.h"
#include "sw2.h"

#define SW2_INIT_STATE_RETRY_MAX 10

/* Magic prefix for SW2 user calibration in SPI flash (0xA1B2 LE) */
#define SW2_USER_CALIB_MAGIC 0xA1B2

/* Drop input reports until calibration is loaded, with a bounded fallback so
 * the controller still works if a SPI read fails. 180 reports ~= 3s at 60Hz,
 * generous enough to absorb a slow-but-eventually-successful read. */
#define SW2_PRE_CALIB_REPORT_LIMIT 180

enum {
    SW2_INIT_STATE_READ_INFO = 0,
    SW2_INIT_STATE_READ_LTK,
    SW2_INIT_STATE_SET_BDADDR,
    SW2_INIT_STATE_READ_NEW_LTK,
    SW2_INIT_STATE_READ_LEFT_FACTORY_CALIB,
    SW2_INIT_STATE_READ_RIGHT_FACTORY_CALIB,
    SW2_INIT_STATE_READ_USER_CALIB,
    SW2_INIT_STATE_SET_LED,
    SW2_INIT_STATE_EN_REPORT,
};

static struct bt_hid_sw2_ctrl_calib calib[BT_MAX_DEV] = {0};

/* Our half of the 0x15/0x04 key exchange (A1). A build-time constant, as it is
 * upstream, so it can be both sent and folded with the controller's reply. */
static const uint8_t sw2_pairing_a1[16] = {
    0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42, 0xc6,
    0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31,
};

/* Offset of the device key (B1) inside a 0x15/0x04 ack's value[].
 *
 * The command header is 8 bytes and struct bt_hidp_sw2_ack declares only its
 * first 4, so value[] starts at header byte 4 and value[k] is command-data
 * offset k - 4. commands.md puts the 16-byte device key at data offset 0x1,
 * behind a constant 0x01 response marker, hence 5. */
#define BT_HIDP_SW2_ACK_DEVICE_KEY_OFFSET 5
static uint8_t pre_calib_report_cnt[BT_MAX_DEV] = {0};

static bool bt_hid_sw2_calib_data_is_plausible(const uint8_t *data) {
    /* Reject all-0xFF (erased flash) and all-zero (uninitialised). A valid
     * stick calibration must have a non-zero, non-saturated X-centre LSB. */
    uint8_t all_ff = 0xFF, all_00 = 0x00;
    for (uint32_t i = 0; i < 9; i++) {
        all_ff &= data[i];
        all_00 |= data[i];
    }
    return all_ff != 0xFF && all_00 != 0x00;
}

/* SPI flash region each READ_SPI init state requests. The controller echoes the
 * address & length back in its ack, so we use this both to build the request and
 * to confirm an ack actually answers the request the current state made. Without
 * that check a duplicated or out-of-order ack shifts every subsequent read by one
 * state, which silently stores device-info bytes (an ASCII serial) as the LTK.
 */
struct bt_hid_sw2_spi_read {
    uint32_t addr;
    uint32_t len;
};

static const struct bt_hid_sw2_spi_read sw2_spi_read[] = {
    [SW2_INIT_STATE_READ_INFO] = {0x00013000, 0x40},
    [SW2_INIT_STATE_READ_LTK] = {0x001FA01A, 0x10},
    [SW2_INIT_STATE_READ_NEW_LTK] = {0x001FA01A, 0x10},
    [SW2_INIT_STATE_READ_LEFT_FACTORY_CALIB] = {0x00013080, 0x40},
    [SW2_INIT_STATE_READ_RIGHT_FACTORY_CALIB] = {0x000130C0, 0x40},
    [SW2_INIT_STATE_READ_USER_CALIB] = {0x001FC040, 0x40},
};

/* Ack value layout: [0..3] hdr, [4..7] len (LE32), [8..11] addr (LE32), [12..] payload. */
#define BT_HIDP_SW2_ACK_LEN_OFFSET 4
#define BT_HIDP_SW2_ACK_ADDR_OFFSET 8
#define BT_HIDP_SW2_ACK_DATA_OFFSET 12

static uint32_t bt_hid_sw2_le32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool bt_hid_sw2_spi_ack_is_expected(struct bt_dev *device, struct bt_hidp_sw2_ack *ack,
        uint32_t len) {
    const struct bt_hid_sw2_spi_read *expected;
    uint32_t ack_addr, ack_len;

    if (device->hid_state >= ARRAY_SIZE(sw2_spi_read)) {
        return false;
    }

    expected = &sw2_spi_read[device->hid_state];
    if (expected->len == 0) {
        /* Current state does not issue a SPI read. */
        return false;
    }

    /* Ack must carry the echoed header plus the payload it claims. Note the caller
     * derives len from att_len without subtracting the ATT opcode byte, so len runs
     * one over the true value length; this bound is conservative either way.
     */
    if (len < sizeof(*ack) + BT_HIDP_SW2_ACK_DATA_OFFSET + expected->len) {
        printf("# %s: short SPI ack: %ld\n", __FUNCTION__, len);
        bt_mon_log(true, "%s: short SPI ack: %ld\n", __FUNCTION__, len);
        return false;
    }

    /* Byte-wise to stay clear of unaligned 32-bit loads on Xtensa. */
    ack_len = bt_hid_sw2_le32(&ack->value[BT_HIDP_SW2_ACK_LEN_OFFSET]);
    ack_addr = bt_hid_sw2_le32(&ack->value[BT_HIDP_SW2_ACK_ADDR_OFFSET]);

    if (ack_addr != expected->addr || ack_len != expected->len) {
        printf("# %s: SPI ack mismatch: got %08lX/%02lX want %08lX/%02lX\n", __FUNCTION__,
            ack_addr, ack_len, expected->addr, expected->len);
        bt_mon_log(true, "%s: SPI ack mismatch: got %08lX/%02lX want %08lX/%02lX\n", __FUNCTION__,
            ack_addr, ack_len, expected->addr, expected->len);
        return false;
    }

    return true;
}

static void bt_hid_sw2_set_calib(struct bt_hid_sw2_ctrl_calib *calib, uint8_t *data, uint8_t stick) {
    calib->sticks[stick].axes[0].neutral = ((data[1] << 8) & 0xF00) | data[0];
    calib->sticks[stick].axes[1].neutral = (data[2] << 4) | (data[1] >> 4);
    calib->sticks[stick].axes[0].rel_max = ((data[4] << 8) & 0xF00) | data[3];
    calib->sticks[stick].axes[1].rel_max = (data[5] << 4) | (data[4] >> 4);
    calib->sticks[stick].axes[0].rel_min = ((data[7] << 8) & 0xF00) | data[6];
    calib->sticks[stick].axes[1].rel_min = (data[8] << 4) | (data[7] >> 4);
}

static void bt_hid_sw2_print_calib(struct bt_hid_sw2_ctrl_calib *calib) {
    TESTS_CMDS_LOG("\"calib_data\": {");
    TESTS_CMDS_LOG("\"rel_min\": [%u, %u, %u, %u], ", calib->sticks[0].axes[0].rel_min, calib->sticks[0].axes[1].rel_min,
        calib->sticks[1].axes[0].rel_min, calib->sticks[1].axes[1].rel_min);
    TESTS_CMDS_LOG("\"rel_max\": [%u, %u, %u, %u], ", calib->sticks[0].axes[0].rel_max, calib->sticks[0].axes[1].rel_max,
        calib->sticks[1].axes[0].rel_max, calib->sticks[1].axes[1].rel_max);
    TESTS_CMDS_LOG("\"neutral\": [%u, %u, %u, %u], ", calib->sticks[0].axes[0].neutral, calib->sticks[0].axes[1].neutral,
        calib->sticks[1].axes[0].neutral, calib->sticks[1].axes[1].neutral);
    TESTS_CMDS_LOG("\"deadzone\": [%u, %u, %u, %u]},\n", calib->sticks[0].deadzone, calib->sticks[0].deadzone,
        calib->sticks[1].deadzone, calib->sticks[1].deadzone);
    printf("rel_min LX %04d RX %04d\n", calib->sticks[0].axes[0].rel_min, calib->sticks[1].axes[0].rel_min);
    printf("neutral LX %04d RX %04d\n", calib->sticks[0].axes[0].neutral, calib->sticks[1].axes[0].neutral);
    printf("rel_max LX %04d RX %04d\n", calib->sticks[0].axes[0].rel_max, calib->sticks[1].axes[0].rel_max);
    printf("rel_min LY %04d RY %04d\n", calib->sticks[0].axes[1].rel_min, calib->sticks[1].axes[1].rel_min);
    printf("neutral LY %04d RY %04d\n", calib->sticks[0].axes[1].neutral, calib->sticks[1].axes[1].neutral);
    printf("rel_max LY %04d RY %04d\n", calib->sticks[0].axes[1].rel_max, calib->sticks[1].axes[1].rel_max);
    printf("        LD %04d RD %04d\n", calib->sticks[0].deadzone, calib->sticks[1].deadzone);
    bt_mon_log(true, "rel_min LX %04d RX %04d\n", calib->sticks[0].axes[0].rel_min, calib->sticks[1].axes[0].rel_min);
    bt_mon_log(true, "neutral LX %04d RX %04d\n", calib->sticks[0].axes[0].neutral, calib->sticks[1].axes[0].neutral);
    bt_mon_log(true, "rel_max LX %04d RX %04d\n", calib->sticks[0].axes[0].rel_max, calib->sticks[1].axes[0].rel_max);
    bt_mon_log(true, "rel_min LY %04d RY %04d\n", calib->sticks[0].axes[1].rel_min, calib->sticks[1].axes[1].rel_min);
    bt_mon_log(true, "neutral LY %04d RY %04d\n", calib->sticks[0].axes[1].neutral, calib->sticks[1].axes[1].neutral);
    bt_mon_log(true, "rel_max LY %04d RY %04d\n", calib->sticks[0].axes[1].rel_max, calib->sticks[1].axes[1].rel_max);
    bt_mon_log(true, "        LD %04d RD %04d\n", calib->sticks[0].deadzone, calib->sticks[1].deadzone);
}

void bt_hid_sw2_init_rumble_gc(struct bt_data *bt_data) {
    bt_data->base.output[1] = 0x50;

    bt_data->base.output[5] = BT_HIDP_SW2_CMD_SET_LED;
    bt_data->base.output[6] = BT_HIDP_SW2_REQ_TYPE_REQ;
    bt_data->base.output[7] = BT_HIDP_SW2_REQ_INT_BLE;
    bt_data->base.output[8] = BT_HIDP_SW2_SUBCMD_SET_LED;
    bt_data->base.output[10] = 0x08;
    bt_data->base.output[13] = bt_hid_led_dev_id_map[bt_data->base.pids->out_idx];
}

bool bt_hid_sw2_pid_is_jc(uint16_t pid) {
    return pid == SW2_LJC_PID || pid == SW2_RJC_PID;
}

/* Joy-Con 2, either half. Same idle LRA template as the Pro 2 but with one
 * block instead of two, so the command header lands 16 bytes earlier. Using the
 * Pro 2 template here - which is what upstream did for these PIDs - puts the
 * header inside what the controller reads as rumble data. */
void bt_hid_sw2_init_rumble_jc(struct bt_data *bt_data) {
    bt_data->base.output[1] = 0x50;
    bt_data->base.output[2] = 0xe1;
    bt_data->base.output[4] = 0x10;
    bt_data->base.output[5] = 0x1e;

    bt_data->base.output[BT_HIDP_SW2_JC_CMD_OFFSET + 0] = BT_HIDP_SW2_CMD_SET_LED;
    bt_data->base.output[BT_HIDP_SW2_JC_CMD_OFFSET + 1] = BT_HIDP_SW2_REQ_TYPE_REQ;
    bt_data->base.output[BT_HIDP_SW2_JC_CMD_OFFSET + 2] = BT_HIDP_SW2_REQ_INT_BLE;
    bt_data->base.output[BT_HIDP_SW2_JC_CMD_OFFSET + 3] = BT_HIDP_SW2_SUBCMD_SET_LED;
    bt_data->base.output[BT_HIDP_SW2_JC_CMD_OFFSET + 6] = 0x08;
    bt_data->base.output[BT_HIDP_SW2_JC_CMD_OFFSET + BT_HIDP_SW2_CMD_HDR_LEN] =
        bt_hid_led_dev_id_map[bt_data->base.pids->out_idx];
}

void bt_hid_sw2_init_rumble_pro2(struct bt_data *bt_data) {
    bt_data->base.output[1] = 0x50;
    bt_data->base.output[2] = 0xe1;
    bt_data->base.output[4] = 0x10;
    bt_data->base.output[5] = 0x1e;

    bt_data->base.output[17] = 0x50;
    bt_data->base.output[18] = 0xe1;
    bt_data->base.output[20] = 0x10;
    bt_data->base.output[21] = 0x1e;

    bt_data->base.output[33] = BT_HIDP_SW2_CMD_SET_LED;
    bt_data->base.output[34] = BT_HIDP_SW2_REQ_TYPE_REQ;
    bt_data->base.output[35] = BT_HIDP_SW2_REQ_INT_BLE;
    bt_data->base.output[36] = BT_HIDP_SW2_SUBCMD_SET_LED;
    bt_data->base.output[38] = 0x08;
    bt_data->base.output[41] = bt_hid_led_dev_id_map[bt_data->base.pids->out_idx];
}

void bt_hid_cmd_sw2_out(struct bt_dev *device, void *report) {
    struct bt_data *bt_data = &bt_adapter.data[device->ids.id];
    if (bt_data) {
        /* pid is zero until the READ_INFO ack lands. Nothing below can be shaped
         * correctly before then, and the default case would otherwise send a
         * zeroed Pro 2 frame for a controller whose type is not yet known. */
        if (bt_data->base.pid == 0) {
            return;
        }
        switch (bt_data->base.pid) {
            case SW2_PRO2_PID:
            default:
                /* Unknown SW2 PIDs use the Pro 2 frame, matching the templates set
                 * up in the READ_INFO ack and the mapping in sw2_to_generic(). */
                bt_att_cmd_write_cmd(device->acl_handle, BT_HIDP_SW2_OUT_CMD_ATT_HDL, (uint8_t *)report,
                    BT_HIDP_SW2_PRO2_CMD_OFFSET + BT_HIDP_SW2_CMD_HDR_LEN);
                bt_data->base.output[17] &= 0xF0;
                bt_data->base.output[17] |= device->tid & 0xF;
                break;
            case SW2_LJC_PID:
            case SW2_RJC_PID:
                /* 17 + 8: the single-LRA prefix plus the command header, the same
                 * shape the Pro 2 case above uses (33 + 8). Upstream sends nothing
                 * at all for these PIDs, which is why a Joy-Con 2 drops the link
                 * without rumble traffic.
                 *
                 * If a Joy-Con 2 still times out on hardware, the reference
                 * implementation to fall back to is bluepad32's: a bare 17-byte
                 * vibration report on BT_HIDP_SW2_OUT_ATT_HDL (0x0012) with no
                 * command header at all, 33 bytes for a Pro 2. */
                bt_att_cmd_write_cmd(device->acl_handle, BT_HIDP_SW2_OUT_CMD_ATT_HDL, (uint8_t *)report,
                    BT_HIDP_SW2_JC_CMD_OFFSET + BT_HIDP_SW2_CMD_HDR_LEN);
                break;
            case SW2_GC_PID:
                bt_att_cmd_write_cmd(device->acl_handle, BT_HIDP_SW2_OUT_CMD_ATT_HDL, (uint8_t *)report, 21);
                break;
        }

        bt_data->base.output[1] &= 0xF0;
        bt_data->base.output[1] |= device->tid++ & 0xF;
    }
}

void bt_hid_sw2_get_calib(int32_t dev_id, struct bt_hid_sw2_ctrl_calib **cal) {
    struct bt_dev *device = NULL;
    bt_host_get_dev_from_id(dev_id, &device);

    if (device && atomic_test_bit(&device->flags, BT_DEV_CALIB_SET)) {
        *cal = &calib[dev_id];
    }
}

/* Issue the SPI read the given state is defined to make, per sw2_spi_read[]. */
static void bt_hid_sw2_read_spi(struct bt_dev *device, uint32_t state) {
    const struct bt_hid_sw2_spi_read *req = &sw2_spi_read[state];
    uint8_t read_spi[] = {
        BT_HIDP_SW2_CMD_READ_SPI,
        BT_HIDP_SW2_REQ_TYPE_REQ,
        BT_HIDP_SW2_REQ_INT_BLE,
        BT_HIDP_SW2_SUBCMD_READ_SPI,
        0x00, 0x08, 0x00, 0x00,
        (uint8_t)req->len, /* Read len */
        0x7e, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, /* Read addr, filled below */
    };

    /* Byte-wise to stay clear of unaligned 32-bit stores on Xtensa. */
    read_spi[12] = (uint8_t)req->addr;
    read_spi[13] = (uint8_t)(req->addr >> 8);
    read_spi[14] = (uint8_t)(req->addr >> 16);
    read_spi[15] = (uint8_t)(req->addr >> 24);
    bt_att_cmd_write_cmd(device->acl_handle, BT_HIDP_SW2_CMD_ATT_HDL, read_spi, sizeof(read_spi));
}

static void bt_hid_sw2_exec_next_state(struct bt_dev *device) {
    struct bt_data *bt_data = &bt_adapter.data[device->ids.id];

    /* A Joy-Con 2 has one stick, and the official console init reads only the
     * primary factory calibration block (0x13080) for either half - never the
     * secondary at 0x130C0 (ndeadly, bluetooth_interface.md, "JoyCon 2"
     * sequence). Reading it anyway returns whatever is in flash for a slot the
     * half does not have, and the plausibility check is not guaranteed to
     * reject it. Skip the state outright for these PIDs.
     *
     * The right half is not an exception: its calibration lives in the primary
     * block too, even though its stick data arrives in the right-hand slot of
     * input report 0x05. sw2_jc_axes_idx routes it accordingly. */
    if (device->hid_state == SW2_INIT_STATE_READ_RIGHT_FACTORY_CALIB
            && bt_data && bt_hid_sw2_pid_is_jc(bt_data->base.pid)) {
        device->hid_state++;
    }

    switch(device->hid_state) {
        case SW2_INIT_STATE_READ_INFO:
            bt_hid_sw2_read_spi(device, device->hid_state);
            break;
        case SW2_INIT_STATE_SET_BDADDR:
        {
            uint8_t set_bdaddr[] = {
                BT_HIDP_SW2_CMD_PAIRING,
                BT_HIDP_SW2_REQ_TYPE_REQ,
                BT_HIDP_SW2_REQ_INT_BLE,
                BT_HIDP_SW2_SUBCMD_PAIRING_STEP1,
                0x00, 0x0e, 0x00, 0x00, 0x00, 0x02,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            };
            bt_addr_le_t bdaddr;
            bt_hci_get_le_local_addr(&bdaddr);
            memcpy(&set_bdaddr[10], bdaddr.a.val, sizeof(bdaddr.a.val));
            memcpy(&set_bdaddr[16], bdaddr.a.val, sizeof(bdaddr.a.val));
            set_bdaddr[16]--;
            bt_att_cmd_write_cmd(device->acl_handle, BT_HIDP_SW2_CMD_ATT_HDL, set_bdaddr, sizeof(set_bdaddr));
            break;
        }
        case SW2_INIT_STATE_READ_LTK:
        case SW2_INIT_STATE_READ_NEW_LTK:
        case SW2_INIT_STATE_READ_LEFT_FACTORY_CALIB:
        case SW2_INIT_STATE_READ_RIGHT_FACTORY_CALIB:
        case SW2_INIT_STATE_READ_USER_CALIB:
            bt_hid_sw2_read_spi(device, device->hid_state);
            break;
        case SW2_INIT_STATE_SET_LED:
        {
            uint8_t led[] = {
                BT_HIDP_SW2_CMD_SET_LED,
                BT_HIDP_SW2_REQ_TYPE_REQ,
                BT_HIDP_SW2_REQ_INT_BLE,
                BT_HIDP_SW2_SUBCMD_SET_LED,
                0x00, 0x08, 0x00, 0x00,
                bt_hid_led_dev_id_map[device->ids.out_idx],
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            };
            bt_att_cmd_write_cmd(device->acl_handle, BT_HIDP_SW2_CMD_ATT_HDL, led, sizeof(led));
            break;
        }
        case SW2_INIT_STATE_EN_REPORT:
        default:
        {
            uint16_t data = BT_GATT_CCC_NOTIFY;
            bt_att_cmd_write_req(device->acl_handle, 0x000b, (uint8_t *)&data, sizeof(data));
            data = 0;
            bt_att_cmd_write_req(device->acl_handle, 0x001b, (uint8_t *)&data, sizeof(data));
            break;
        }
    }
}

void bt_hid_sw2_init(struct bt_dev *device) {
    /* Reset per-device state so a stale calib from a previous controller on
     * the same dev_id cannot bleed through if a SPI read later fails. */
    memset(&calib[device->ids.id], 0, sizeof(calib[0]));
    pre_calib_report_cnt[device->ids.id] = 0;

    /* enable cmds rsp */
    uint16_t data = BT_GATT_CCC_NOTIFY;
    bt_att_cmd_write_req(device->acl_handle, 0x001b, (uint8_t *)&data, sizeof(data));

    bt_hid_sw2_exec_next_state(device);

    atomic_set_bit(&device->flags, BT_DEV_HID_INIT_DONE);
}

/* Drop input reports while the init state machine is still loading
 * calibration. Without this, the first ~3 reports get bridged with the
 * default 0x800 stick centre (vs the calibrated ~0x7B0), and worse, if a
 * SPI read aborts with an error response the stale/garbage calib that
 * gets parsed produces a stuck stick offset. After a bounded number of
 * reports we let them through anyway with default meta so a controller
 * with a misbehaving SPI link still works. */
static bool bt_hid_sw2_gate_report(struct bt_dev *device) {
    if (atomic_test_bit(&device->flags, BT_DEV_CALIB_SET)) {
        return true;
    }
    if (pre_calib_report_cnt[device->ids.id] < SW2_PRE_CALIB_REPORT_LIMIT) {
        pre_calib_report_cnt[device->ids.id]++;
        return false;
    }
    /* Timeout: proceed with whatever (possibly zeroed) calib we have. The
     * adapter falls back to default meta when calib->neutral == 0. */
    printf("# %s: dev %ld calib read timed out, using defaults\n",
        __FUNCTION__, device->ids.id);
    atomic_set_bit(&device->flags, BT_DEV_CALIB_SET);
    bt_type_update(device->ids.id, BT_SW2, device->ids.subtype);
    return true;
}

void bt_hid_sw2_hdlr(struct bt_dev *device, uint16_t att_handle, uint8_t *data, uint32_t len) {
    switch (att_handle) {
        case BT_HIDP_SW2_REPORT_TYPE1_ATT_HDL:
        {
            struct bt_data *bt_data = &bt_adapter.data[device->ids.id];

            /* HD-rumble keepalive. A Pro 2 or Joy-Con 2 expects a steady stream of
             * output frames and drops the link (HCI 0x08, supervision timeout) if
             * it stops; bluepad32 sends an idle LRA packet every 5 ms and warns
             * explicitly that the link times out without it. Upstream only sent
             * this when the user had rumble enabled, so turning rumble off took the
             * pad down with it. The idle frame is what the buffer already holds
             * when nothing is rumbling, so sending it unconditionally costs one
             * ATT write per input report and nothing else.
             *
             * The NSO GameCube pad is excluded: HD-rumble writes power its motor
             * off, and bluepad32 likewise excludes PID 0x2073 from its keepalive.
             *
             * Sent before the calibration gate below, deliberately. The gate exists
             * to suppress *input*, and dropping an input report must not also drop
             * the link maintenance that keeps the controller connected. */
            if (bt_data && bt_data->base.pid != SW2_GC_PID) {
                bt_hid_cmd_sw2_out(device, bt_data->base.output);
            }

            if (!bt_hid_sw2_gate_report(device)) {
                break;
            }
            bt_host_bridge(device, 1, data, len);
            break;
        }
        case BT_HIDP_SW2_REPORT_TYPE2_ATT_HDL:
            if (!bt_hid_sw2_gate_report(device)) {
                break;
            }
            bt_host_bridge(device, 2, data, len);
            break;
        case BT_HIDP_SW2_ACK_ATT_HDL:
        {
            struct bt_hidp_sw2_ack *ack = (struct bt_hidp_sw2_ack *)data;
            /* Only accept successful responses. An ERR (0x00) ack carries no
             * meaningful payload — parsing it advances the state machine
             * with garbage data (notably for SPI reads, this is the root of
             * the "phantom left-stick held down" symptom seen on the NSO
             * GameCube controller). A failed SPI read is retried below instead. */
            if (ack->type != BT_HIDP_SW2_REQ_TYPE_RSP && ack->cmd != BT_HIDP_SW2_CMD_READ_SPI) {
                printf("# %s: dev %ld skip non-RSP ack type=0x%02X cmd=0x%02X subcmd=0x%02X state=%ld\n",
                    __FUNCTION__, device->ids.id, ack->type, ack->cmd, ack->subcmd, device->hid_state);
                break;
            }
            switch (ack->cmd) {
                case BT_HIDP_SW2_CMD_READ_SPI:
                    /* Only consume a successful SPI read that answers the request this state
                     * made. Re-issue on an error or a mismatch rather than advancing, so a
                     * failed or stray ack cannot shift the state machine and misfile the payload.
                     */
                    if (ack->type != BT_HIDP_SW2_REQ_TYPE_RSP) {
                        printf("# %s: dev %ld SPI read failed (ack type=0x%02X) state=%ld, retrying\n",
                            __FUNCTION__, device->ids.id, ack->type, device->hid_state);
                    }
                    if (ack->type != BT_HIDP_SW2_REQ_TYPE_RSP || !bt_hid_sw2_spi_ack_is_expected(device, ack, len)) {
                        if (++device->hid_retry_cnt > SW2_INIT_STATE_RETRY_MAX) {
                            printf("# %s: SPI read retry limit, disconnecting dev: %ld\n",
                                __FUNCTION__, device->ids.id);
                            bt_mon_log(true, "%s: SPI read retry limit, disconnecting dev: %ld\n",
                                __FUNCTION__, device->ids.id);
                            bt_hci_disconnect(device);
                        }
                        else {
                            bt_hid_sw2_exec_next_state(device);
                        }
                        break;
                    }
                    device->hid_retry_cnt = 0;
                    switch (device->hid_state) {
                        case SW2_INIT_STATE_READ_INFO:
                        {
                            struct bt_data *bt_data = &bt_adapter.data[device->ids.id];
                            if (bt_data) {
                                bt_data->base.vid = *(uint16_t *)&ack->value[30];
                                bt_data->base.pid = *(uint16_t *)&ack->value[32];

                                printf("%s: VID: 0x%04X PID: 0x%04X\n", __FUNCTION__, bt_data->base.vid, bt_data->base.pid);
                                bt_mon_log(true, "%s: VID: 0x%04X PID: 0x%04X\n", __FUNCTION__, bt_data->base.vid, bt_data->base.pid);

                                /* The advertisement carries the product ID too, and it
                                 * arrives before any of this. Every downstream decision
                                 * - output template, mapping, keepalive - keys off the
                                 * PID, so a corrupt READ_INFO payload silently picks the
                                 * wrong everything. The two sources disagreeing is worth
                                 * saying out loud; the correlation check above should
                                 * already have caught it, and this says whether it did.
                                 *
                                 * Advisory only: the SPI value still wins, because the
                                 * advertisement is absent on an inbound reconnect. */
                                if (device->le_adv_pid && device->le_adv_pid != bt_data->base.pid) {
                                    printf("# %s: PID mismatch: adv 0x%04X vs SPI 0x%04X\n",
                                        __FUNCTION__, device->le_adv_pid, bt_data->base.pid);
                                    bt_mon_log(true, "%s: PID mismatch: adv 0x%04X vs SPI 0x%04X\n",
                                        __FUNCTION__, device->le_adv_pid, bt_data->base.pid);
                                }

                                /* Init output data for Rumble/LED feedback */
                                switch (bt_data->base.pid) {
                                    case SW2_LJC_PID:
                                    case SW2_RJC_PID:
                                        bt_hid_sw2_init_rumble_jc(bt_data);
                                        break;
                                    case SW2_PRO2_PID:
                                        bt_hid_sw2_init_rumble_pro2(bt_data);
                                        break;
                                    case SW2_GC_PID:
                                        bt_hid_sw2_init_rumble_gc(bt_data);
                                        break;
                                    default:
                                        /* Pro 2 shaped, matching the mapping fallback in
                                         * sw2_to_generic(). Leaving the buffer zeroed meant an
                                         * unrecognised SW2 controller got no feedback frame at
                                         * all, and with the keepalive that now also means no
                                         * link maintenance. */
                                        printf("# %s: unknown SW2 pid %04X, using the Pro 2 output template\n",
                                            __FUNCTION__, bt_data->base.pid);
                                        bt_mon_log(true, "%s: unknown SW2 pid %04X, using the Pro 2 output template\n",
                                            __FUNCTION__, bt_data->base.pid);
                                        bt_hid_sw2_init_rumble_pro2(bt_data);
                                        break;
                                }
                            }
                            break;
                        }
                        case SW2_INIT_STATE_READ_LTK:
                        {
                            struct bt_smp_encrypt_info encrypt_info = {0};

                            printf("%s: LTK: ", __FUNCTION__);
                            for (uint32_t i = 0; i < 16; i++) {
                                printf("%02X ", ack->value[12 + i]);
                            }
                            printf("\n");
                            if (bt_host_load_le_ltk(&device->le_remote_bdaddr, &encrypt_info, NULL) == 0) {
                                if (memcmp(encrypt_info.ltk, &ack->value[12], sizeof(encrypt_info.ltk)) == 0) {
                                    /* LTK match, skip pairing */
                                    device->hid_state += 2;
                                }
                            }
                            break;
                        }
                        case SW2_INIT_STATE_READ_NEW_LTK:
                            printf("%s: NEW LTK: ", __FUNCTION__);
                            for (uint32_t i = 0; i < 16; i++) {
                                printf("%02X ", ack->value[12 + i]);
                            }
                            printf("\n");
                            bt_host_store_le_ltk(&device->le_remote_bdaddr, (struct bt_smp_encrypt_info *)&ack->value[12]);
                            break;
                        case SW2_INIT_STATE_READ_LEFT_FACTORY_CALIB:
                        {
                            struct bt_hid_sw2_ctrl_calib *dev_calib = &calib[device->ids.id];
                            uint8_t *data = &ack->value[52];
                            if (bt_hid_sw2_calib_data_is_plausible(data)) {
                                bt_hid_sw2_set_calib(dev_calib, data, 0);
                            }
                            break;
                        }
                        case SW2_INIT_STATE_READ_RIGHT_FACTORY_CALIB:
                        {
                            struct bt_hid_sw2_ctrl_calib *dev_calib = &calib[device->ids.id];
                            uint8_t *data = &ack->value[52];
                            if (bt_hid_sw2_calib_data_is_plausible(data)) {
                                bt_hid_sw2_set_calib(dev_calib, data, 1);
                            }
                            bt_hid_sw2_print_calib(dev_calib);
                            break;
                        }
                        case SW2_INIT_STATE_READ_USER_CALIB:
                        {
                            struct bt_hid_sw2_ctrl_calib *dev_calib = &calib[device->ids.id];
                            /* User-cal layout in SPI flash:
                             *   0x1FC040  magic (uint16 LE, 0xA1B2)  -> ack->value[12..13]
                             *   0x1FC042  L stick 9-byte calib       -> ack->value[14..22]
                             *   0x1FC060  magic                       -> ack->value[44..45]
                             *   0x1FC062  R stick 9-byte calib       -> ack->value[46..54]
                             * Only trust each stick when its magic is present. */
                            uint16_t l_magic = (ack->value[13] << 8) | ack->value[12];
                            uint16_t r_magic = (ack->value[45] << 8) | ack->value[44];
                            if (l_magic == SW2_USER_CALIB_MAGIC
                                    && bt_hid_sw2_calib_data_is_plausible(&ack->value[14])) {
                                bt_hid_sw2_set_calib(dev_calib, &ack->value[14], 0);
                            }
                            if (r_magic == SW2_USER_CALIB_MAGIC
                                    && bt_hid_sw2_calib_data_is_plausible(&ack->value[46])) {
                                bt_hid_sw2_set_calib(dev_calib, &ack->value[46], 1);
                            }
                            bt_hid_sw2_print_calib(dev_calib);
                            atomic_set_bit(&device->flags, BT_DEV_CALIB_SET);
                            /* Force reinit once calib available */
                            bt_type_update(device->ids.id, BT_SW2, device->ids.subtype);
                            break;
                        }
                    }
                    device->hid_state++;
                    bt_hid_sw2_exec_next_state(device);
                    break;
                case BT_HIDP_SW2_CMD_SET_LED:
                    /* Seen on hardware: an ack tagged SET_LED arriving in the middle of the
                     * pairing steps. Only the one we asked for moves the bring-up along. */
                    if (device->hid_state != SW2_INIT_STATE_SET_LED) {
                        printf("# %s: dev %ld SET_LED ack in state %ld, ignored\n", __FUNCTION__, device->ids.id, device->hid_state);
                        break;
                    }
                    printf("# BT_HIDP_SW2_CMD_SET_LED\n");
                    /* Controller slot is confirmed. Stop scanning so the next
                     * controller can only pair via an explicit button press,
                     * preventing simultaneous SW2 init races.
                     *
                     * Not the first such call: att_hid.c already stops inquiry on
                     * MTU_RSP, long before SW2 init starts. This one re-asserts it
                     * after the LE_CONN_COMPLETE handler has had its say about
                     * advertising and scanning, which is the window a second pad
                     * can slip into. */
                    bt_hci_stop_inquiry();
                    device->hid_state++;
                    bt_hid_sw2_exec_next_state(device);
                    break;
                case BT_HIDP_SW2_CMD_PAIRING:
                    if (device->hid_state != SW2_INIT_STATE_SET_BDADDR) {
                        printf("# %s: dev %ld pairing ack in state %ld, ignored\n", __FUNCTION__, device->ids.id, device->hid_state);
                        break;
                    }
                    switch(ack->subcmd) {
                        case BT_HIDP_SW2_SUBCMD_PAIRING_STEP1:
                        {
                            uint8_t pair2[] = {
                                BT_HIDP_SW2_CMD_PAIRING,
                                BT_HIDP_SW2_REQ_TYPE_REQ,
                                BT_HIDP_SW2_REQ_INT_BLE,
                                BT_HIDP_SW2_SUBCMD_PAIRING_STEP2,
                                0x00, 0x11, 0x00, 0x00, 0x00,
                            };
                            uint8_t pair2_full[sizeof(pair2) + sizeof(sw2_pairing_a1)];

                            memcpy(pair2_full, pair2, sizeof(pair2));
                            memcpy(pair2_full + sizeof(pair2), sw2_pairing_a1, sizeof(sw2_pairing_a1));
                            bt_att_cmd_write_cmd(device->acl_handle, BT_HIDP_SW2_CMD_ATT_HDL,
                                pair2_full, sizeof(pair2_full));
                            break;
                        }
                        case BT_HIDP_SW2_SUBCMD_PAIRING_STEP2:
                        {
                            /* Instrumentation only, for the derived-LTK experiment.
                             *
                             * This ack carries the controller's key (B1), and the
                             * shared LTK is A1 XOR B1 - which is how the real console
                             * obtains it. This firmware instead reads the key back out
                             * of SPI flash at 0x1FA01A afterwards, which is the source
                             * of the stale-key and bricked-pad failures.
                             *
                             * B1 is documented as the fixed constant
                             * 5CF6EE792CDF05E1BA2B6325C41A5F10, and with A1 also
                             * constant the derived key would be the same every time.
                             * Printing both here, next to the existing "NEW LTK" dump
                             * of the flash read, turns the comparison into one serial
                             * capture: if the derived value matches the flash value in
                             * either byte order, the SPI reads can go.
                             *
                             * Deliberately does not change what is stored. */
                            const uint8_t *b1 = &ack->value[BT_HIDP_SW2_ACK_DEVICE_KEY_OFFSET];
                            uint8_t derived[16];

                            printf("%s: B1: ", __FUNCTION__);
                            for (uint32_t i = 0; i < sizeof(derived); i++) {
                                derived[i] = sw2_pairing_a1[i] ^ b1[i];
                                printf("%02X ", b1[i]);
                            }
                            printf("\n%s: derived LTK (A1^B1): ", __FUNCTION__);
                            for (uint32_t i = 0; i < sizeof(derived); i++) {
                                printf("%02X ", derived[i]);
                            }
                            printf("\n%s: derived LTK reversed: ", __FUNCTION__);
                            for (uint32_t i = sizeof(derived); i > 0; i--) {
                                printf("%02X ", derived[i - 1]);
                            }
                            printf("\n");

                            uint8_t pair3[] = {
                                BT_HIDP_SW2_CMD_PAIRING,
                                BT_HIDP_SW2_REQ_TYPE_REQ,
                                BT_HIDP_SW2_REQ_INT_BLE,
                                BT_HIDP_SW2_SUBCMD_PAIRING_STEP3,
                                0x00, 0x11, 0x00, 0x00, 0x00, 0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b, 0x41, 0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73
                            };
                            bt_att_cmd_write_cmd(device->acl_handle, BT_HIDP_SW2_CMD_ATT_HDL, pair3, sizeof(pair3));
                            break;
                        }
                        case BT_HIDP_SW2_SUBCMD_PAIRING_STEP3:
                        {
                            uint8_t pair4[] = {
                                BT_HIDP_SW2_CMD_PAIRING,
                                BT_HIDP_SW2_REQ_TYPE_REQ,
                                BT_HIDP_SW2_REQ_INT_BLE,
                                BT_HIDP_SW2_SUBCMD_PAIRING_STEP4,
                                0x00, 0x01, 0x00, 0x00, 0x00
                            };
                            bt_att_cmd_write_cmd(device->acl_handle, BT_HIDP_SW2_CMD_ATT_HDL, pair4, sizeof(pair4));
                            break;
                        }
                        case BT_HIDP_SW2_SUBCMD_PAIRING_STEP4:
                        {
                            device->hid_state++;
                            bt_hid_sw2_exec_next_state(device);
                            break;
                        }
                    }
                    break;
            }
            break;
        }
        default:
            printf("%s: unknown handle %04X\n", __FUNCTION__, att_handle);
            break;
    }
}

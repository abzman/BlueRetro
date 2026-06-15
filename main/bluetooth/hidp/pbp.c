#include <stdint.h>
#include <string.h>

#include "bluetooth/host.h"
#include "adapter/adapter.h"

void bt_hid_pbp_hdlr(struct bt_dev *device, uint16_t handle, uint8_t *data, uint32_t len) {
    struct bt_data *bt_data = &bt_adapter.data[device->ids.id];

    /*
     * Pass the real notification data through.
     * If the notification is shorter than 5 bytes, send a neutral/released report
     * instead of ignoring it. This prevents stale held buttons.
     */
    static uint8_t report[5];

    memset(report, 0, sizeof(report));

    if (data && len >= 5) {
        memcpy(report, data, sizeof(report));
    }
    else {
        /*
         * Neutral joystick / no buttons.
         * Prevents old button state from sticking.
         */
        report[1] = 0x00;
        report[2] = 0x00;
        report[3] = 0x07;
        report[4] = 0x6C;
    }

    bt_data->base.report_id = 0;
    bt_data->base.report_type = PAD;
    bt_data->base.input = report;
    bt_data->base.input_len = sizeof(report);

    adapter_bridge(bt_data);
}
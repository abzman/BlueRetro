#include <stdlib.h>
#include <string.h>
#include "adapter/adapter.h"
#include "pbp.h"

#define PBP_X_MIN 32
#define PBP_X_MAX 192
#define PBP_Y_MIN 36
#define PBP_Y_MAX 180
#define PBP_BIT(n) (1UL << (n))

/* 7.5% deadzone from pokeball-plus-4-windows */
#define PBP_DEADZONE_PERMILLE 75

static const uint32_t pbp_mask[4] = {
    PBP_BIT(PAD_LX_LEFT)  |
    PBP_BIT(PAD_LX_RIGHT) |
    PBP_BIT(PAD_LY_DOWN)  |
    PBP_BIT(PAD_LY_UP)    |
    PBP_BIT(PAD_RB_DOWN)  | /* A */
    PBP_BIT(PAD_MM),        /* Start */
};

static const uint32_t pbp_desc[4] = {
    PBP_BIT(PAD_LX_LEFT)  |
    PBP_BIT(PAD_LX_RIGHT) |
    PBP_BIT(PAD_LY_DOWN)  |
    PBP_BIT(PAD_LY_UP),
};

static struct ctrl_meta pbp_meta[ADAPTER_PS2_MAX_AXES] = {
    [AXIS_LX] = {
        .neutral = 0,
        .deadzone = ((PBP_X_MAX - PBP_X_MIN) * PBP_DEADZONE_PERMILLE) / 1000,
        .abs_max = (PBP_X_MAX - PBP_X_MIN) / 2,
        .abs_min = (PBP_X_MAX - PBP_X_MIN) / 2,
        .polarity = 0,
    },
    [AXIS_LY] = {
        .neutral = 0,
        .deadzone = ((PBP_Y_MAX - PBP_Y_MIN) * PBP_DEADZONE_PERMILLE) / 1000,
        .abs_max = (PBP_Y_MAX - PBP_Y_MIN) / 2,
        .abs_min = (PBP_Y_MAX - PBP_Y_MIN) / 2,
        .polarity = 0,
    },
};

static int32_t pbp_axis(int32_t raw, int32_t min, int32_t max, int invert) {
    if (raw < min) {
        raw = min;
    }
    else if (raw > max) {
        raw = max;
    }

    const int32_t center = (min + max) / 2;
    const int32_t deadzone = ((max - min) * PBP_DEADZONE_PERMILLE) / 1000;

    int32_t value = raw - center;

    if (invert) {
        value = -value;
    }

    if (abs(value) <= deadzone) {
        return 0;
    }

    return value;
}

int32_t pbp_to_generic(struct bt_data *bt_data, struct wireless_ctrl *ctrl_data) {
    const uint8_t *data = bt_data->base.input;

    if (!data || bt_data->base.input_len < 5) {
        return -1;
    }

    memset(ctrl_data, 0, sizeof(*ctrl_data));

    ctrl_data->mask = pbp_mask;
    ctrl_data->desc = pbp_desc;

    const uint8_t btn = data[1];

    /* top button -> A */
    if (btn == 1 || btn == 3) {
        ctrl_data->btns[0].value |= PBP_BIT(PAD_RB_DOWN);
    }

    /* joystick click -> Start */
    if (btn == 2 || btn == 3) {
        ctrl_data->btns[0].value |= PBP_BIT(PAD_MM);
    }

    const int32_t raw_x = ((data[3] & 0x0F) << 4) | ((data[2] >> 4) & 0x0F);
    const int32_t raw_y = data[4];

    ctrl_data->axes[AXIS_LX].meta = &pbp_meta[AXIS_LX];
    ctrl_data->axes[AXIS_LX].value = pbp_axis(raw_x, PBP_X_MIN, PBP_X_MAX, 0);

    ctrl_data->axes[AXIS_LY].meta = &pbp_meta[AXIS_LY];
    ctrl_data->axes[AXIS_LY].value = pbp_axis(raw_y, PBP_Y_MIN, PBP_Y_MAX, 1);

    return 0;
}

bool pbp_fb_from_generic(struct generic_fb *fb_data, struct bt_data *bt_data) {
    return false;
}
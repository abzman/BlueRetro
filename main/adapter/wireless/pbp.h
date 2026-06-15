#ifndef _PBP_H_
#define _PBP_H_

#include <stdbool.h>
#include "adapter/adapter.h"

int32_t pbp_to_generic(struct bt_data *bt_data, struct wireless_ctrl *ctrl_data);
bool pbp_fb_from_generic(struct generic_fb *fb_data, struct bt_data *bt_data);

#endif
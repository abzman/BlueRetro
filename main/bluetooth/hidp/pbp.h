#ifndef _HIDP_PBP_H_
#define _HIDP_PBP_H_

#include <stdint.h>
#include "bluetooth/host.h"

void bt_hid_pbp_hdlr(struct bt_dev *device, uint16_t handle, uint8_t *data, uint32_t len);

#endif
/*
 * Copyright (c) 2026, BlueRetro contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OGXMINI_H_
#define _OGXMINI_H_
#include "adapter/adapter.h"
#include "ogxmini_pad.h"

void ogxmini_meta_init(struct wired_ctrl *ctrl_data);
void ogxmini_init_buffer(int32_t dev_mode, struct wired_data *wired_data);
void ogxmini_from_generic(int32_t dev_mode, struct wired_ctrl *ctrl_data, struct wired_data *wired_data);
void ogxmini_fb_to_generic(int32_t dev_mode, struct raw_fb *raw_fb_data, struct generic_fb *fb_data);
void ogxmini_get_pad(struct wired_data *wired_data, struct ogxm_pad *pad);

#endif /* _OGXMINI_H_ */

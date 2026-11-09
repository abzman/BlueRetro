/*
 * Copyright (c) 2026, BlueRetro contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/* I2C master for OGX-Mini Picos (ESP32 Bluepad32 I2C builds), one per output: output 1 is the
 * Pico at 0x01, which is where every unmodified Pico answers; outputs 2 to 4 are at 0x02 to 0x04
 * on Picos whose firmware reads an address strap.
 *
 * Not a console bus: the Pico takes a pad whenever it comes, so this runs on the FreeRTOS core
 * with ESP-IDF's I2C driver, not on the bare metal core. Each output's pad goes out as soon as it
 * changes, and every tick (60 Hz) when it differs from the last one sent (turbo) or the last one
 * is 100 ms old (a Pico that rebooted). Every tick also reads each Pico's rumble back.
 *
 * Wiring, as OGX-Mini's own ESP32 firmware has it: SDA GPIO21 to the Pico's GPIO18, SCL GPIO22 to
 * its GPIO19, and ground. With more than one Pico, add 2.2 to 4.7 kOhm pull-ups to 3.3 V. */

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"
#include "adapter/adapter.h"
#include "adapter/config.h"
#include "adapter/wired/ogxmini.h"
#include "ogxmini_i2c.h"

#define OGXM_SDA_PIN 21
#define OGXM_SCL_PIN 22
#define OGXM_I2C_HZ 400000
#define OGXM_XFER_MS 5

#define OGXM_TICK_US 16667
#define OGXM_RESEND_TICKS 6     /* 100 ms */
#define OGXM_PROBE_TICKS 60     /* 1 s */

#define OGXM_NOTIFY_TICK BIT(16)

struct ogxm_pico {
    i2c_master_dev_handle_t dev;
    uint8_t present;
    uint8_t driver;     /* mode last sent, OGXM_DRV_NONE once it needs sending */
    uint8_t rumble[2];
    uint32_t sent_tick;
    struct ogxm_pad sent;
};

static TaskHandle_t ogxm_task_hdl = NULL;
static i2c_master_bus_handle_t ogxm_bus = NULL;
static struct ogxm_pico picos[OGXM_MAX_PICO] = {0};

static void ogxm_rumble(uint32_t index, uint8_t lf, uint8_t hf) {
    struct ogxm_pico *pico = &picos[index];
    struct raw_fb fb_data = {0};

    if (pico->rumble[0] == lf && pico->rumble[1] == hf) {
        return;
    }
    pico->rumble[0] = lf;
    pico->rumble[1] = hf;

    fb_data.header.wired_id = index;
    fb_data.header.type = FB_TYPE_RUMBLE;
    fb_data.header.data_len = 2;
    fb_data.data[0] = lf;
    fb_data.data[1] = hf;
    adapter_q_fb(&fb_data);
}

static void ogxm_lost(uint32_t index) {
    picos[index].present = 0;
    ogxm_rumble(index, 0, 0);
    printf("# %s: Pico 0x%02lX gone\n", __FUNCTION__, OGXM_I2C_ADDR_BASE + index);
}

static int32_t ogxm_write(uint32_t index, const struct ogxm_packet_in *pkt) {
    if (i2c_master_transmit(picos[index].dev, (const uint8_t *)pkt, sizeof(*pkt), OGXM_XFER_MS) != ESP_OK) {
        ogxm_lost(index);
        return -1;
    }
    return 0;
}

static void ogxm_probe(void) {
    for (uint32_t i = 0; i < OGXM_MAX_PICO; i++) {
        if (!picos[i].present && i2c_master_probe(ogxm_bus, OGXM_I2C_ADDR_BASE + i, OGXM_XFER_MS) == ESP_OK) {
            picos[i].present = 1;
            picos[i].driver = OGXM_DRV_NONE;
            picos[i].sent_tick = 0;
            printf("# %s: Pico 0x%02lX found\n", __FUNCTION__, OGXM_I2C_ADDR_BASE + i);
        }
    }
}

static void ogxm_service(uint32_t index, uint32_t notify, uint32_t tick) {
    struct ogxm_pico *pico = &picos[index];
    struct ogxm_packet_in pkt;
    struct ogxm_pad pad;

    /* The mode from the output's config. The Pico stores it and reboots only when it differs
     * from its own, so it is sent each time a Pico shows up and when the config changes. */
    uint8_t driver = ogxm_driver_from_dev_mode(config.out_cfg[index].dev_mode);
    if (pico->driver != driver) {
        ogxm_packet_set_driver(&pkt, driver);
        if (ogxm_write(index, &pkt)) {
            return;
        }
        pico->driver = driver;
        pico->sent_tick = tick - OGXM_RESEND_TICKS;
    }

    if (notify & (BIT(index) | OGXM_NOTIFY_TICK)) {
        ogxmini_get_pad(&wired_adapter.data[index], &pad);
        if (memcmp(&pad, &pico->sent, sizeof(pad)) || (tick - pico->sent_tick) >= OGXM_RESEND_TICKS) {
            ogxm_packet_set_pad(&pkt, &pad);
            if (ogxm_write(index, &pkt)) {
                return;
            }
            memcpy(&pico->sent, &pad, sizeof(pad));
            pico->sent_tick = tick;
        }
    }

    if (notify & OGXM_NOTIFY_TICK) {
        uint8_t buf[sizeof(struct ogxm_packet_out)];
        uint8_t lf, hf;

        if (i2c_master_receive(pico->dev, buf, sizeof(buf), OGXM_XFER_MS) != ESP_OK) {
            ogxm_lost(index);
            return;
        }
        if (ogxm_packet_out_parse(buf, sizeof(buf), &lf, &hf) == 0) {
            ogxm_rumble(index, lf, hf);
        }
    }
}

static void ogxm_task(void *arg) {
    uint32_t tick = 0;

    ogxm_probe();

    while (1) {
        uint32_t notify = 0;

        xTaskNotifyWait(0, UINT32_MAX, &notify, portMAX_DELAY);

        if (notify & OGXM_NOTIFY_TICK) {
            ++tick;
            for (uint32_t i = 0; i < OGXM_MAX_PICO; i++) {
                ++wired_adapter.data[i].frame_cnt;
            }
            if ((tick % OGXM_PROBE_TICKS) == 0) {
                ogxm_probe();
            }
        }

        for (uint32_t i = 0; i < OGXM_MAX_PICO; i++) {
            if (picos[i].present) {
                ogxm_service(i, notify, tick);
            }
        }
    }
}

static void ogxm_tick_cb(void *arg) {
    xTaskNotify(ogxm_task_hdl, OGXM_NOTIFY_TICK, eSetBits);
}

void ogxmini_i2c_kick(uint32_t index) {
    if (ogxm_task_hdl && index < OGXM_MAX_PICO) {
        xTaskNotify(ogxm_task_hdl, BIT(index), eSetBits);
    }
}

void ogxmini_i2c_init(void) {
#ifndef CONFIG_BLUERETRO_QEMU
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = OGXM_SDA_PIN,
        .scl_io_num = OGXM_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    const esp_timer_create_args_t tick_args = {
        .callback = &ogxm_tick_cb,
        .name = "ogxm_tick",
    };
    esp_timer_handle_t tick_hdl;

    if (i2c_new_master_bus(&bus_cfg, &ogxm_bus) != ESP_OK) {
        printf("# %s: I2C bus init fail\n", __FUNCTION__);
        return;
    }

    for (uint32_t i = 0; i < OGXM_MAX_PICO; i++) {
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = OGXM_I2C_ADDR_BASE + i,
            .scl_speed_hz = OGXM_I2C_HZ,
        };

        if (i2c_master_bus_add_device(ogxm_bus, &dev_cfg, &picos[i].dev) != ESP_OK) {
            printf("# %s: I2C device 0x%02lX init fail\n", __FUNCTION__, OGXM_I2C_ADDR_BASE + i);
            return;
        }
    }

    if (xTaskCreatePinnedToCore(&ogxm_task, "ogxm_task", 3072, NULL, 6, &ogxm_task_hdl, 0) != pdPASS) {
        printf("# %s: task create fail\n", __FUNCTION__);
        return;
    }

    if (esp_timer_create(&tick_args, &tick_hdl) == ESP_OK) {
        esp_timer_start_periodic(tick_hdl, OGXM_TICK_US);
    }
#else
    /* No I2C in QEMU: the tests check the pads the adapter builds */
    (void)ogxm_task;
    (void)ogxm_tick_cb;
#endif
}

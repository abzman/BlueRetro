/*
 * Copyright (c) 2019-2024, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "system/ui.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <soc/efuse_reg.h>
#include "driver/gpio.h"
#include "hal/ledc_periph.h"
#include "hal/gpio_hal.h"
#include "driver/ledc.h"
#include "esp_rom_gpio.h"
#include "driver/rtc_io.h"
#include "adapter/adapter.h"
#include "adapter/config.h"
#include "adapter/memory_card.h"
#include "bluetooth/host.h"
#include "bluetooth/hci.h"
#include "wired/wired_bare.h"
#include "tools/util.h"
#include "system/fs.h"
#include "system/led.h"
#include "system/port_led.h"
#include "bare_metal_app_cpu.h"
#include "serial_cmd.h"
#include "console_cmd.h"
#include "tests/cmds.h"
#include "manager.h"

/* GPIO matrix signal index for one LEDC channel.
 *
 * ESP-IDF v6.1 reshaped ledc_signal_conn_t: the outer index is the LEDC group
 * rather than the speed mode, and the per-channel output signal is an array
 * rather than a base index the channel number is added to. ESP32 has a single
 * group, and the values are the same LEDC_{HS,LS}_SIG_OUTn_IDX either way, so
 * only the spelling changed.
 *
 * ledc_set_pin() is the supported way to do this and would not have needed
 * touching here, but it validates arguments and registers the pin in the
 * driver's per-channel bookkeeping - a behavioural change, not a rename, and
 * these paths deliberately poke the GPIO matrix directly. Worth revisiting
 * separately. */
static inline int ledc_sig_out_idx(ledc_mode_t mode, ledc_channel_t chan) {
    return ledc_periph_signal[0].speed_mode[mode].sig_out_idx[chan];
}

#if defined(CONFIG_BLUERETRO_HW_IBLUECONTROL) || defined(CONFIG_BLUERETRO_HW2_IBLUECONTROLMOD)
/* iBlueControl boards: console reset button ("IN" connector), needs the internal pull-up */
#define BOOT_BTN_PIN 16
#else
#define BOOT_BTN_PIN 0
#endif

#define RESET_PIN 14

#define POWER_ON_PIN 13
#ifdef CONFIG_BLUERETRO_HW2_IBLUECONTROLMOD
#define POWER_OFF_PIN 17
#else
#define POWER_OFF_PIN 16
#endif
#define POWER_SENSE_PIN 39

#define POWER_OFF_ALT_PIN 12

#ifdef CONFIG_BLUERETRO_HW_IBLUECONTROL
#define SENSE_P1_PIN 36
#define SENSE_P2_PIN 35
#define LED_P1_PIN 15
#define LED_P2_PIN 4
#define LED_P3_PIN 15
#define LED_P4_PIN 4
#define ESP_OE_1_PIN 2
#define ESP_OE_2_PIN 27
#else
#define SENSE_P1_PIN 35
#define SENSE_P2_PIN 36
#ifdef CONFIG_BLUERETRO_HW2_IBLUECONTROLMOD
#define LED_P1_PIN 15
#define LED_P2_PIN 4
#define LED_P3_PIN 2
#define LED_P4_PIN 12
#else
#define LED_P1_PIN 2
#define LED_P2_PIN 4
#define LED_P3_PIN 12
#define LED_P4_PIN 15
#endif
#endif

#define SENSE_P3_PIN 32
#define SENSE_P4_PIN 33

#define SENSE_P1_ALT_PIN 15
#define SENSE_P2_ALT_PIN 34

#define INHIBIT_CNT 200

typedef void (*sys_mgr_cmd_t)(void);

enum {
    SYS_MGR_BTN_STATE0 = 0,
    SYS_MGR_BTN_STATE1,
    SYS_MGR_BTN_STATE2,
    SYS_MGR_BTN_STATE3,
};

#ifdef CONFIG_BLUERETRO_HW2
static uint8_t sense_list[] = {
    SENSE_P1_PIN, SENSE_P2_PIN, SENSE_P3_PIN, SENSE_P4_PIN
};
#endif
static uint8_t led_list[] = {
    LED_P1_PIN, LED_P2_PIN, LED_P3_PIN, LED_P4_PIN
};
static uint8_t current_pulse_led = LED_P1_PIN;
static uint8_t err_led_pin;
static uint8_t err_led_inv;
static uint8_t power_off_pin = POWER_OFF_PIN;
static uint8_t led_init_cnt = 1;
static uint16_t port_state = 0;
static RingbufHandle_t cmd_q_hdl = NULL;
static uint32_t chip_package = EFUSE_RD_CHIP_VER_PKG_ESP32D0WDQ6;
static bool factory_reset = false;

static int32_t sys_mgr_get_power(void);
static int32_t sys_mgr_get_boot_btn(void);
static void sys_mgr_reset(void);
static void sys_mgr_power_on(void);
static void sys_mgr_power_off(void);
static void sys_mgr_inquiry_toggle(void);
static void sys_mgr_factory_reset(void);
static void sys_mgr_deep_sleep(void);
static void sys_mgr_esp_restart(void);
static void sys_mgr_wired_reset(void);

static const sys_mgr_cmd_t sys_mgr_cmds[] = {
    sys_mgr_reset,
    sys_mgr_power_on,
    sys_mgr_power_off,
    sys_mgr_inquiry_toggle,
    sys_mgr_factory_reset,
    sys_mgr_deep_sleep,
    sys_mgr_esp_restart,
    sys_mgr_wired_reset,
};

static inline uint32_t sense_port_is_empty(uint32_t index) {
#ifdef CONFIG_BLUERETRO_HW2
    if (hw_config.ports_sense_input_polarity) {
        return !gpio_get_level(sense_list[index]);
    }
    else {
        return gpio_get_level(sense_list[index]);
    }
#else
    return 1;
#endif
}

static inline void set_power_on(uint32_t state) {
    if (hw_config.power_pin_polarity) {
        gpio_set_level(POWER_ON_PIN, !state);
    }
    else {
        gpio_set_level(POWER_ON_PIN, state);
    }
}

#ifdef CONFIG_BLUERETRO_HW_IBLUECONTROL
static void ibluecontrol_set_oe(uint16_t port_mask) {
    gpio_set_level(ESP_OE_1_PIN, (port_mask & BIT(0)) ? 0 : 1);
    gpio_set_level(ESP_OE_2_PIN, (port_mask & BIT(1)) ? 0 : 1);
}

static void ibluecontrol_enter_deep_sleep(void) {
    set_power_on(0);
    gpio_set_direction(LED_P1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_P1_PIN, 0);
    gpio_hold_en(POWER_ON_PIN);
    gpio_hold_en(LED_P1_PIN);
    gpio_deep_sleep_hold_en();
    rtc_gpio_init(BOOT_BTN_PIN);
    rtc_gpio_set_direction(BOOT_BTN_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(BOOT_BTN_PIN);
    rtc_gpio_pulldown_dis(BOOT_BTN_PIN);
    esp_sleep_enable_ext0_wakeup(BOOT_BTN_PIN, 0);
    esp_deep_sleep_start();
}
#endif

static inline void set_power_off(uint32_t state) {
    if (hw_config.power_pin_polarity) {
        gpio_set_level(power_off_pin, !state);
    }
    else {
        gpio_set_level(power_off_pin, state);
    }
}

static inline void set_reset(uint32_t state) {
    if (hw_config.reset_pin_polarity) {
        gpio_set_level(RESET_PIN, !state);
    }
    else {
        gpio_set_level(RESET_PIN, state);
    }
}

static inline void set_sense_out(uint32_t pin, uint32_t state) {
    if (hw_config.ports_sense_output_polarity) {
        gpio_set_level(pin, !state);
    }
    else {
        gpio_set_level(pin, state);
    }
}

static inline void set_port_led(uint32_t index, uint32_t state) {
    if (state) {
        esp_rom_gpio_connect_out_signal(led_list[index], ledc_sig_out_idx(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1), 0, 0);
    }
    else {
        esp_rom_gpio_connect_out_signal(led_list[index], ledc_sig_out_idx(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2), 0, 0);
    }
}

static inline uint32_t get_port_led_pin(uint32_t index) {
    return led_list[index];
}

static void internal_flag_init(void) {
#ifdef CONFIG_BLUERETRO_HW_IBLUECONTROL
    hw_config.external_adapter = 0;
#elif defined(CONFIG_BLUERETRO_HW2)
    if (hw_config.power_pin_polarity) {
        if (!gpio_get_level(POWER_ON_PIN) && gpio_get_level(RESET_PIN)) {
            hw_config.external_adapter = 1;
        }
    }
    else {
        if (gpio_get_level(POWER_ON_PIN) && gpio_get_level(RESET_PIN)) {
            hw_config.external_adapter = 1;
        }
    }
#else
    hw_config.external_adapter = 1;
#endif
    if (hw_config.external_adapter) {
        printf("# %s: External adapter\n", __FUNCTION__);
    }
    else {
        printf("# %s: Internal adapter\n", __FUNCTION__);
    }
}

static void port_led_pulse(uint32_t pin) {
    if (pin) {
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(pin, ledc_sig_out_idx(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0), 0, 0);
    }
}

static void set_leds_as_btn_status(uint8_t state) {
    ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, hw_config.led_flash_on_duty_cycle, 0);

    /* Use all port LEDs */
    for (uint32_t i = 0; i < hw_config.port_cnt; i++) {
        uint8_t pin = led_list[i];

        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        if (state) {
            esp_rom_gpio_connect_out_signal(pin, ledc_sig_out_idx(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1), 0, 0);
        }
    }

    /* Use error LED as well */
#ifndef CONFIG_BLUERETRO_HW2_METHYLENEX /* MethyleneX: this pin is the OLED SCL (see ui.c) */
    if (state) {
        esp_rom_gpio_connect_out_signal(err_led_pin, ledc_sig_out_idx(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1), err_led_inv, 0);
    }
    else {
        esp_rom_gpio_connect_out_signal(err_led_pin, ledc_sig_out_idx(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0), err_led_inv, 0);
    }
#endif
}

static void power_on_hdl(void) {
#ifdef CONFIG_BLUERETRO_HW2
    static int32_t curr = 0, prev = 0;
    struct bt_dev *not_used;

    prev = curr;
    curr = sys_mgr_get_power();
    if (curr) {
        /* System is power on */
        bt_hci_inquiry_override(0);

        if (bt_host_get_active_dev(&not_used) < 0) {
            /* No Bt device */
            uint32_t port_cnt_loc = 0;

            for (uint32_t i = 0; i < hw_config.port_cnt; i++) {
                if (sense_port_is_empty(i)) {
                    port_cnt_loc++;
                }
            }
            if (port_cnt_loc == hw_config.port_cnt) {
                /* No wired controller */
                if (!bt_hci_get_inquiry()) {
                    bt_hci_start_inquiry();
                }
            }
        }
    }
    else {
        /* System is off */
        if (curr != prev) {
            /* Kick BT controllers on off transition */
            bt_host_disconnect_all();
        }
        else if (bt_host_get_hid_init_dev(&not_used) > -1) {
            /* Power on BT connect */
            bt_hci_inquiry_override(0);
            sys_mgr_power_on();
            return;
        }
        /* Make sure no inquiry while power off */
        bt_hci_inquiry_override(1);
        if (bt_hci_get_inquiry()) {
            bt_hci_stop_inquiry();
        }
    }
#endif /* CONFIG_BLUERETRO_HW2 */
}

static void wired_port_hdl(void) {
    uint32_t update = 0;
    uint16_t port_mask = 0;
    uint8_t err_led_set = 0;
    uint8_t pads_ready = 0;
#ifdef CONFIG_BLUERETRO_HW2
    static uint32_t last_pad_color_generation;
    uint32_t pad_color_generation = ui_pad_color_generation();
    uint8_t pad_color_changed = (pad_color_generation != last_pad_color_generation);
    uint8_t pad_color_refresh_ok = 1;
#endif

    for (int32_t i = 0, j = 0, idx = 0; i < BT_MAX_DEV; i++) {
        struct bt_dev *device = NULL;
        uint8_t bt_ready = 0;
        uint8_t pairing = 0;

        bt_host_get_dev_from_id(i, &device);

        for (; j < hw_config.port_cnt; j++) {
            if (sense_port_is_empty(j)) {
                j++;
                break;
            }
            set_port_led(j, 0);
            port_led_set_port(j, PORT_LED_WIRED);
            idx++;
        }

        bt_ready = atomic_test_bit(&device->flags, BT_DEV_HID_INIT_DONE);
        if (bt_ready) {
            pads_ready++;
        }

#ifdef CONFIG_BLUERETRO_HW2
        int32_t prev_idx = device->ids.out_idx;
#endif
        device->ids.out_idx = idx;
        if ((hw_config.hotplug && bt_ready) || !hw_config.hotplug) {
            port_mask |= BIT(idx) | adapter_get_out_mask(idx);
        }
        idx++;


        if (device->ids.out_idx < hw_config.port_cnt) {
            if (bt_ready) {
                set_port_led(device->ids.out_idx, 1);
            }
            else if (get_port_led_pin(device->ids.out_idx) != current_pulse_led) {
                set_port_led(device->ids.out_idx, 0);
            }
        }

        if (!bt_ready && !err_led_set) {
            uint8_t new_led = (device->ids.out_idx < hw_config.port_cnt) ? get_port_led_pin(device->ids.out_idx) : 0;

            if (bt_hci_get_inquiry()) {
                pairing = 1;
                port_led_pulse(new_led);
                err_led_set = 1;
                current_pulse_led = new_led;
            }
            else {
                current_pulse_led = 0;
            }
        }

        if (device->ids.out_idx < hw_config.port_cnt) {
            if (bt_ready) {
                port_led_set_port(device->ids.out_idx, PORT_LED_BT);
            }
            else if (pairing) {
                port_led_set_port(device->ids.out_idx, PORT_LED_PAIRING);
            }
            else {
                port_led_set_port(device->ids.out_idx, PORT_LED_OFF);
            }
        }

#ifdef CONFIG_BLUERETRO_HW2
        uint8_t map_changed = (device->ids.out_idx != prev_idx);

        if (map_changed) {
            update++;
            printf("# %s: BTDEV %ld map to WIRED %ld\n", __FUNCTION__, i, device->ids.out_idx);
        }

        if (bt_ready && (map_changed || pad_color_changed)) {
            struct raw_fb fb_data = {0};
            int32_t fb_ret;

            fb_data.header.wired_id = device->ids.out_idx;
            fb_data.header.type = FB_TYPE_PLAYER_LED;
            fb_data.header.data_len = 0;
            fb_ret = adapter_q_fb(&fb_data);

            if (pad_color_changed && fb_ret != 0) {
                pad_color_refresh_ok = 0;
            }
        }
#endif
    }
#ifdef CONFIG_BLUERETRO_HW2
    if (pad_color_changed && pad_color_refresh_ok) {
        last_pad_color_generation = pad_color_generation;
    }
#endif
    {
        static uint8_t last_pads = 0xFF;

        if (pads_ready != last_pads) {
            last_pads = pads_ready;
            ui_set_pads(pads_ready);
        }
    }
    if (hw_config.hotplug) {
        if (port_state != port_mask) {
            update++;
        }
    }
    if (update && !mc_get_state()) {
        printf("# %s: Update ports state: %04X\n", __FUNCTION__, port_mask);
        wired_bare_port_cfg(port_mask);
#ifdef CONFIG_BLUERETRO_HW_IBLUECONTROL
        ibluecontrol_set_oe(port_mask);
#endif
        port_state = port_mask;
        if (hw_config.ports_sense_p3_p4_as_output) {
            /* Toggle Wii classic sense line to force ctrl reinit */
            set_sense_out(SENSE_P3_PIN, 0);
            set_sense_out(SENSE_P4_PIN, 0);
            vTaskDelay(hw_config.ports_sense_output_ms / portTICK_PERIOD_MS);
            set_sense_out(SENSE_P3_PIN, 1);
            set_sense_out(SENSE_P4_PIN, 1);
        }
    }
}

static void boot_btn_hdl(void) {
    static uint32_t check_qdp = 0;
    static uint32_t inhibit_cnt = 0;
    uint32_t hold_cnt = 0;
    uint32_t state = 0;

    /* Let inhibit_cnt reach 0 before handling button again */
    if (inhibit_cnt && inhibit_cnt--) {
        /* Power off on quick double press */
        if (check_qdp && sys_mgr_get_power() && sys_mgr_get_boot_btn()) {
            sys_mgr_power_off();
            check_qdp = 0;
        }
        return;
    }
    check_qdp = 0;

    if (sys_mgr_get_boot_btn()) {
        set_leds_as_btn_status(1);
        port_led_set_btn(1, 0);

        while (sys_mgr_get_boot_btn()) {
            hold_cnt++;
            if (hold_cnt > (hw_config.sw_io0_hold_thres_ms[state] / 10) && state < SYS_MGR_BTN_STATE3) {
                ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, hw_config.led_flash_duty_cycle, 0);
                ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, hw_config.led_flash_hz[state]);
                port_led_set_btn(1, hw_config.led_flash_hz[state]);
                state++;
            }
            if (hold_cnt == 3000) {
                printf("# FW will be factory reset\n");
                factory_reset = true;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }

#ifndef CONFIG_BLUERETRO_SYSTEM_UNIVERSAL
        if (hw_config.external_adapter)
#endif
        {
            state++;
        }

        if (sys_mgr_get_power()) {
            /* System is on */
            switch (state) {
                case SYS_MGR_BTN_STATE0:
                    sys_mgr_reset();
                    check_qdp = 1;
                    break;
                case SYS_MGR_BTN_STATE1:
                    if (bt_hci_get_inquiry()) {
                        bt_hci_stop_inquiry();
                    }
                    else {
                        bt_host_disconnect_all();
                    }
                    break;
                case SYS_MGR_BTN_STATE2:
                    bt_hci_start_inquiry();
                    break;
                default:
                    sys_mgr_factory_reset();
                    break;
            }
        }
        else {
            /* System is off */
            switch (state) {
                case SYS_MGR_BTN_STATE0:
                    sys_mgr_power_on();
                    break;
                default:
                    set_reset(0);
                    sys_mgr_power_on();
                    set_reset(1);
                    break;
            }
        }

        set_leds_as_btn_status(0);
        port_led_set_btn(0, 0);
        inhibit_cnt = INHIBIT_CNT;
    }
}

static void sys_mgr_task(void *arg) {
    uint32_t cnt = 0;
    uint8_t *cmd;
    size_t cmd_len;

    while (1) {
        boot_btn_hdl();
        serial_cmd_poll();
        console_cmd_poll();

        /* Fetch system cmd to execute */
        if (cmd_q_hdl) {
            cmd = (uint8_t *)xRingbufferReceive(cmd_q_hdl, &cmd_len, 0);
            if (cmd) {
                if (sys_mgr_cmds[*cmd]) {
                    sys_mgr_cmds[*cmd]();
                }
                vRingbufferReturnItem(cmd_q_hdl, (void *)cmd);
            }
        }

        /* Update those only 1/32 loop */
        if ((cnt & 0x1F) == 0x1F) {
            wired_port_hdl();
            if (!hw_config.external_adapter) {
                power_on_hdl();
            }
        }
        cnt++;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

static void sys_mgr_reset(void) {
    set_reset(0);
    vTaskDelay(hw_config.reset_pin_pulse_ms / portTICK_PERIOD_MS);
    set_reset(1);
}

static void sys_mgr_inquiry_toggle(void) {
    if (bt_hci_get_inquiry()) {
        bt_hci_stop_inquiry();
    }
    else {
        /* Only here, never in bt_hci_start_inquiry(): that also runs by itself
         * every time the last device drops, which would wipe the list moments
         * after anything landed on it. Asking to pair is a deliberate act, and
         * the only point at which reconsidering a failed device makes sense. */
        bt_host_le_pair_fail_clear();
        bt_hci_start_inquiry();
    }
}

static void sys_mgr_power_on(void) {
    set_power_on(1);
#ifdef CONFIG_BLUERETRO_HW2_IBLUECONTROLMOD
    /* Stock FW holds the power on line, no pulse */
    return;
#endif
    if (!hw_config.power_pin_is_hold) {
        vTaskDelay(hw_config.power_pin_pulse_ms / portTICK_PERIOD_MS);
        set_power_on(0);
    }
}

static void sys_mgr_power_off(void) {
    bt_host_disconnect_all();
#ifdef CONFIG_BLUERETRO_HW2
    if (hw_config.power_pin_is_hold) {
        set_power_on(0);
    }
    else {
        set_power_off(1);
        vTaskDelay(hw_config.power_pin_pulse_ms / portTICK_PERIOD_MS);
        set_power_off(0);
    }
#endif
}

static int32_t sys_mgr_get_power(void) {
#ifdef CONFIG_BLUERETRO_SYSTEM_UNIVERSAL
    return 1;
#else
    if (hw_config.external_adapter) {
        return 1;
    }
    else {
        return gpio_get_level(POWER_SENSE_PIN);
    }
#endif
}

static int32_t sys_mgr_get_boot_btn(void) {
    return !gpio_get_level(BOOT_BTN_PIN);
}

static void sys_mgr_factory_reset(void) {
    const esp_partition_t* partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
    if (factory_reset && partition) {
        esp_partition_erase_range(partition, 0, partition->size);
    }

    fs_reset();
    printf("BlueRetro factory reset\n");
    bt_host_disconnect_all();
    bt_host_disconnect_conf();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
}

/* Before a restart or a sleep, tell every peer the link is going, as factory reset does. Left
 * alone, the link just stops, and each peer times it out: a phone waited ~10 s to see the adapter
 * gone after a restart, and after every OTA update. The short wait first lets the reply to the
 * write that asked for this (the restart command, the OTA end) go out ahead of the disconnect,
 * so the asker hears yes before goodbye; the whole still takes the second it always has. */
static void sys_mgr_disconnect_before_going(void) {
    vTaskDelay(200 / portTICK_PERIOD_MS);
    bt_host_disconnect_all();
    bt_host_disconnect_conf();
    vTaskDelay(800 / portTICK_PERIOD_MS);
}

static void sys_mgr_esp_restart(void) {
    sys_mgr_disconnect_before_going();
    esp_restart();
}

static void sys_mgr_deep_sleep(void) {
    sys_mgr_disconnect_before_going();
#ifdef CONFIG_BLUERETRO_HW_IBLUECONTROL
    ibluecontrol_enter_deep_sleep();
#else
#ifdef CONFIG_BLUERETRO_HW2_IBLUECONTROLMOD
    /* Keep reset button pull-up and console control lines idle while sleeping */
    gpio_hold_en(BOOT_BTN_PIN);
    gpio_hold_en(RESET_PIN);
    gpio_hold_en(POWER_ON_PIN);
    gpio_hold_en(power_off_pin);
    gpio_deep_sleep_hold_en();
#endif
    esp_deep_sleep_start();
#endif
}

static void IRAM_ATTR sys_mgr_wired_reinit_task(void) {
    for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
        adapter_init_buffer(i);
    }

    if (wired_adapter.system_id < WIRED_MAX) {
        wired_bare_init(chip_package);
    }
}

static void sys_mgr_wired_reset(void) {
    if (init_app_cpu_baremetal() != ESP_OK) {
        /* Not recoverable here, but not fatal to the running system either:
         * start_app_cpu below refuses and reports when CPU1 never came up. */
        printf("# APP CPU restart timed out\n");
    }
    start_app_cpu(sys_mgr_wired_reinit_task);
    port_state = 0;
}

void sys_mgr_cmd(uint8_t cmd) {
#ifdef CONFIG_BLUERETRO_QEMU
    /* No wired driver nor second core to restart under QEMU, and no flash to reset: report the
     * command so the tests see a reset, power off or factory reset asked for, and do nothing */
    printf("# %s %d (QEMU: ignored)\n", __FUNCTION__, cmd);
    TESTS_CMDS_LOG("\"sys_mgr_cmd\": %d,\n", cmd);
    return;
#endif
    if (cmd_q_hdl) {
        UBaseType_t ret = xRingbufferSend(cmd_q_hdl, &cmd, sizeof(cmd), portMAX_DELAY);
        if (ret != pdTRUE) {
            printf("# %s cmd_q full!\n", __FUNCTION__);
        }
    }
    else {
        printf("# %s cmd_q_hdl NULL!\n", __FUNCTION__);
        if (cmd == SYS_MGR_CMD_WIRED_RST) {
            /* For gameid cfg we may need to reset bare core very early */
            sys_mgr_wired_reset();
        }
    }
}

#ifdef CONFIG_BLUERETRO_HW2_IBLUECONTROLMOD
/* Set the console control lines to idle as early as possible, like the stock FW */
void sys_mgr_early_init(void) {
    gpio_config_t io_conf = {0};

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pin_bit_mask = 1ULL << BOOT_BTN_PIN;
    gpio_config(&io_conf);

    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_set_level(POWER_ON_PIN, 0);
    io_conf.pin_bit_mask = 1ULL << POWER_ON_PIN;
    gpio_config(&io_conf);
    gpio_set_level(POWER_OFF_PIN, 0);
    io_conf.pin_bit_mask = 1ULL << POWER_OFF_PIN;
    gpio_config(&io_conf);

    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    gpio_set_level(RESET_PIN, 1);
    io_conf.pin_bit_mask = 1ULL << RESET_PIN;
    gpio_config(&io_conf);
}
#endif

void sys_mgr_init(uint32_t package) {
    gpio_config_t io_conf = {0};

#ifdef CONFIG_BLUERETRO_HW_IBLUECONTROL
    gpio_hold_dis(POWER_ON_PIN);
    gpio_hold_dis(LED_P1_PIN);
    gpio_deep_sleep_hold_dis();
#endif

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;

    io_conf.pin_bit_mask = 1ULL << BOOT_BTN_PIN;
    gpio_config(&io_conf);

    chip_package = package;
    err_led_pin = err_led_get_pin();
    err_led_inv = err_led_get_inv();

    ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_TIMER_20_BIT,
        .freq_hz = 2,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t ledc_channel = {
        .channel    = LEDC_CHANNEL_1,
        .duty       = hw_config.led_flash_on_duty_cycle,
        .gpio_num   = LED_P1_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint     = 0,
        .timer_sel  = LEDC_TIMER_1,
    };
    ledc_timer_config(&ledc_timer);
    ledc_channel_config(&ledc_channel);

    while (wired_adapter.system_id <= WIRED_AUTO) {
        boot_btn_hdl();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    cmd_q_hdl = xRingbufferCreate(32, RINGBUF_TYPE_NOSPLIT);
    if (cmd_q_hdl == NULL) {
        printf("# Failed to create cmd_q ring buffer\n");
    }

    switch (wired_adapter.system_id) {
        case GENESIS:
        case SATURN:
            hw_config.port_cnt = 2;
#ifdef CONFIG_BLUERETRO_HW2
            sense_list[0] = SENSE_P1_ALT_PIN;
            sense_list[1] = SENSE_P2_ALT_PIN;
            power_off_pin = POWER_OFF_ALT_PIN;
#endif
            break;
        case JAGUAR:
            hw_config.port_cnt = 1;
#ifdef CONFIG_BLUERETRO_HW2
            sense_list[0] = SENSE_P1_ALT_PIN;
            sense_list[1] = SENSE_P2_ALT_PIN;
#endif
            break;
        case WII_EXT:
            hw_config.port_cnt = 2;
            hw_config.power_pin_is_hold = 1;
            hw_config.ports_sense_input_polarity = 1;
            hw_config.power_pin_polarity = 1;
            hw_config.ports_sense_p3_p4_as_output = 1;
            break;
        case N64:
        case DC:
            hw_config.port_cnt = 4;
            break;
        case GC:
            hw_config.port_cnt = 4;
#ifdef CONFIG_BLUERETRO_HW2_IBLUECONTROLMOD
            /* Only drive a port once a BT ctrl is connected to it, like the stock FW. */
            /* Leaves ports free for undetected wired accessories (GBA link cable, etc). */
            hw_config.hotplug = 1;
            /* Button hold thresholds from the board manual: 3s / 6s / 9s */
            hw_config.sw_io0_hold_thres_ms[0] = 3000;
            hw_config.sw_io0_hold_thres_ms[1] = 6000;
            hw_config.sw_io0_hold_thres_ms[2] = 9000;
#endif
            break;
        case PARALLEL_1P:
        case PCE:
        case REAL_3DO:
        case JVS:
        case VBOY:
        case PARALLEL_1P_OD:
        case SEA_BOARD:
            hw_config.port_cnt = 1;
            break;
        default:
            hw_config.port_cnt = 2;
            break;
    }

#ifdef CONFIG_BLUERETRO_BT_H4_TRACE
    hw_config.port_cnt = 1;
#endif

    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
#ifdef CONFIG_BLUERETRO_HW2_IBLUECONTROLMOD
    /* Always internal, keep the console control lines driven since early init */
    hw_config.external_adapter = 0;
    (void)internal_flag_init;
#else
#ifdef CONFIG_BLUERETRO_HW2
    io_conf.pin_bit_mask = 1ULL << POWER_ON_PIN;
    gpio_config(&io_conf);
    io_conf.pin_bit_mask = 1ULL << RESET_PIN;
    gpio_config(&io_conf);
#endif

    internal_flag_init();
#endif

    hw_config_patch();

    err_led_cfg_update();

    led_init_cnt = hw_config.port_cnt;
    if (wired_adapter.system_id == PSX || wired_adapter.system_id == PS2) {
        led_init_cnt = 4;
    }

#ifdef CONFIG_BLUERETRO_HW2
    for (uint32_t i = 0; i < hw_config.port_cnt; i++) {
        io_conf.pin_bit_mask = 1ULL << sense_list[i];
        gpio_config(&io_conf);
    }
    io_conf.pin_bit_mask = 1ULL << POWER_SENSE_PIN;
    gpio_config(&io_conf);
#endif

#ifndef CONFIG_BLUERETRO_HW2
    for (uint32_t i = 0; i < sizeof(led_list); i++) {
        led_list[i] = hw_config.hw1_ports_led_pins[i];
    }
#endif

    io_conf.mode = GPIO_MODE_OUTPUT;
    for (uint32_t i = 0; i < led_init_cnt; i++) {
#ifdef CONFIG_BLUERETRO_HW2
        /* Skip pin 15 if alt sense pin are used */
        if (sense_list[0] == led_list[i]) {
            continue;
        }
#endif
        gpio_set_level(led_list[i], 0);
        io_conf.pin_bit_mask = 1ULL << led_list[i];
        gpio_config(&io_conf);

        if (i < hw_config.port_cnt) {
            /* Can't use GPIO mode on port LED as some wired driver overwrite whole GPIO port */
            /* Use unused LEDC channel 2 to force output low */
            esp_rom_gpio_connect_out_signal(led_list[i], ledc_sig_out_idx(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2), 0, 0);
        }
    }

#ifdef CONFIG_BLUERETRO_HW2
    if (hw_config.power_pin_od) {
        io_conf.mode = GPIO_MODE_OUTPUT_OD;
    }
    else {
        io_conf.mode = GPIO_MODE_OUTPUT;
    }
    set_power_on(0);
    io_conf.pin_bit_mask = 1ULL << POWER_ON_PIN;
    gpio_config(&io_conf);

#ifndef CONFIG_BLUERETRO_HW_IBLUECONTROL
    set_power_off(0);
    io_conf.pin_bit_mask = 1ULL << power_off_pin;
    gpio_config(&io_conf);
#endif

    gpio_set_level(RESET_PIN, 1);
    if (hw_config.reset_pin_od) {
        io_conf.mode = GPIO_MODE_OUTPUT_OD;
    }
    else {
        io_conf.mode = GPIO_MODE_OUTPUT;
    }
    io_conf.pin_bit_mask = 1ULL << RESET_PIN;
    gpio_config(&io_conf);

    if (hw_config.ports_sense_p3_p4_as_output) {
        /* Wii-ext got a sense line that we need to control */
        if (hw_config.ports_sense_output_od) {
            io_conf.mode = GPIO_MODE_OUTPUT_OD;
        }
        else {
            io_conf.mode = GPIO_MODE_OUTPUT;
        }
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.pin_bit_mask = 1ULL << SENSE_P4_PIN;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
        gpio_set_level(SENSE_P4_PIN, 1);
        io_conf.pin_bit_mask = 1ULL << SENSE_P3_PIN;
        gpio_config(&io_conf);
        gpio_set_level(SENSE_P3_PIN, 1);
    }

#ifdef CONFIG_BLUERETRO_HW_IBLUECONTROL
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pin_bit_mask = 1ULL << ESP_OE_1_PIN;
    gpio_config(&io_conf);
    gpio_set_level(ESP_OE_1_PIN, 1);
    io_conf.pin_bit_mask = 1ULL << ESP_OE_2_PIN;
    gpio_config(&io_conf);
    gpio_set_level(ESP_OE_2_PIN, 1);

    if (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT0)) {
        sys_mgr_power_on();
    }
    else if (sys_mgr_get_boot_btn()) {
        ibluecontrol_enter_deep_sleep();
    }
    else {
        sys_mgr_power_on();
    }
#elif defined(CONFIG_BLUERETRO_HW2_IBLUECONTROLMOD)
    /* Stock FW waits 1 s, then always raises IO13 and keeps it high. */
    /* The console stays black without it. */
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    sys_mgr_power_on();
    if (sys_mgr_get_boot_btn()) {
        sys_mgr_deep_sleep();
    }
#else
    /* If boot switch pressed at boot, trigger system on and goes to deep sleep */
    if (sys_mgr_get_boot_btn() && !sys_mgr_get_power()) {
        sys_mgr_power_on();
        sys_mgr_deep_sleep();
    }
#endif
#endif /* CONFIG_BLUERETRO_HW2 */

    port_led_init();

    xTaskCreatePinnedToCore(sys_mgr_task, "sys_mgr_task", 2048, NULL, 5, NULL, 0);
}

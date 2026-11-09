/*
 * Copyright (c) 2019-2026, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "soc/io_mux_reg.h"
#include "esp_private/periph_ctrl.h"
#include <soc/spi_periph.h>
#include <soc/pcnt_struct.h>
#include <hal/pcnt_ll.h>
#include <esp32/rom/ets_sys.h>
#include "hal/clk_gate_ll.h"
#include "driver/gpio.h"
#include "system/intr.h"
#include "system/gpio.h"
#include "system/delay.h"
#include "zephyr/atomic.h"
#include "zephyr/types.h"
#include "tools/util.h"
#include "adapter/adapter.h"
#include "adapter/config.h"
#include "adapter/wired/npiso.h"
#include "wired_bare.h"
#include "beam_timing.h"
#include "snes_spi.h"

#define GPIO_INTR_NUM 19

#define P1_LATCH_PIN 32
#define P1_CLK_PIN 5
#define P1_CIPO_PIN 19
#define P1_COPI_PIN 23
#define P2_LATCH_PIN 32
#define P2_CLK_PIN 18
#define P2_CIPO_PIN 22
#define P2_COPI_PIN 26

#define P1_LATCH_MASK (1 << (P1_LATCH_PIN - 32))
#define P1_CLK_MASK (1 << P1_CLK_PIN)
#define P1_CIPO_MASK (1 << P1_CIPO_PIN)
#define P1_COPI_MASK (1 << P1_COPI_PIN)
#define P2_LATCH_MASK (1 << (P2_LATCH_PIN - 32))
#define P2_CLK_MASK (1 << P2_CLK_PIN)
#define P2_CIPO_MASK (1 << P2_CIPO_PIN)
#define P2_COPI_MASK (1 << P2_COPI_PIN)

#define SNES_PORT_MAX 2

enum {
    SNES_PAD_FORMAT_DEFAULT = 0,
    SNES_PAD_FORMAT_BR_8BITS,
    SNES_PAD_FORMAT_BR_4BITS,
    SNES_PAD_FORMAT_CNT,
};

struct snes_ctrl_port {
    struct spi_cfg cfg;
    spi_dev_t *hw;
    uint32_t latch_pin;
    uint32_t clk_pin;
    uint32_t cipo_pin;
    uint32_t copi_pin;
    uint32_t latch_mask;
    uint32_t clk_mask;
    uint32_t cipo_mask;
    uint32_t copi_mask;
    uint8_t latch_sig;
    uint8_t clk_sig;
    uint8_t cipo_sig;
    uint8_t copi_sig;
    uint8_t pcnt_ctrl_sig;
    uint8_t pcnt_pulse_sig;
    uint8_t spi_mod;
    uint8_t rumble_data;
    uint8_t format;
    uint8_t pcnt_id;
};

static struct snes_ctrl_port snes_ctrl_ports[SNES_PORT_MAX] = {
    {
        .cfg = {
            .hw = &SPI2,
            .write_bit_order = 0,
            .read_bit_order = 0,
            .clk_idle_edge = 1,
            .clk_i_edge = 0,
            .miso_delay_mode = 0,
            .miso_delay_num = 2,
            .mosi_delay_mode = 0,
            .mosi_delay_num = 3,
            .write_bit_len = 33 - 1,
            .read_bit_len = 33 - 1,
            .inten = 0,
        },
        .hw = &SPI2,
        .latch_pin = P1_LATCH_PIN,
        .clk_pin = P1_CLK_PIN,
        .cipo_pin = P1_CIPO_PIN,
        .copi_pin = P1_COPI_PIN,
        .latch_mask = P1_LATCH_MASK,
        .clk_mask = P1_CLK_MASK,
        .cipo_mask = P1_CIPO_MASK,
        .copi_mask = P1_COPI_MASK,
        .latch_sig = HSPICS0_IN_IDX,
        .clk_sig = HSPICLK_IN_IDX,
        .cipo_sig = HSPIQ_OUT_IDX,
        .copi_sig = HSPID_IN_IDX,
        .pcnt_ctrl_sig = PCNT_CTRL_CH0_IN0_IDX,
        .pcnt_pulse_sig = PCNT_SIG_CH0_IN0_IDX,
        .spi_mod = PERIPH_HSPI_MODULE,
        .pcnt_id = 0,
    },
    {
        .cfg = {
            .hw = &SPI3,
            .write_bit_order = 0,
            .read_bit_order = 0,
            .clk_idle_edge = 1,
            .clk_i_edge = 0,
            .miso_delay_mode = 0,
            .miso_delay_num = 2,
            .mosi_delay_mode = 0,
            .mosi_delay_num = 3,
            .write_bit_len = 33 - 1,
            .read_bit_len = 33 - 1,
            .inten = 0,
        },
        .hw = &SPI3,
        .latch_pin = P2_LATCH_PIN,
        .clk_pin = P2_CLK_PIN,
        .cipo_pin = P2_CIPO_PIN,
        .copi_pin = P2_COPI_PIN,
        .latch_mask = P2_LATCH_MASK,
        .clk_mask = P2_CLK_MASK,
        .cipo_mask = P2_CIPO_MASK,
        .copi_mask = P2_COPI_MASK,
        .latch_sig = VSPICS0_IN_IDX,
        .clk_sig = VSPICLK_IN_IDX,
        .cipo_sig = VSPIQ_OUT_IDX,
        .copi_sig = VSPID_IN_IDX,
        .pcnt_ctrl_sig = PCNT_CTRL_CH0_IN1_IDX,
        .pcnt_pulse_sig = PCNT_SIG_CH0_IN1_IDX,
        .spi_mod = PERIPH_VSPI_MODULE,
        .pcnt_id = 1,
    },
};

/* Super Scope (Gamepad Alt on port 2). Each poll builds the scope's report and, while Fire or
 * Cursor is held on the screen, sets the time the beam will pass the aim in the next picture;
 * the loop the CPU idles in (scope_loop) pulls IOBit low then, latching the PPU's H/V
 * counters as the scope's light sensor would. */
#define SCOPE_PULSE_CYCLES (2 * CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ)  /* 2 us low */
#define SCOPE_REF_DOT 32        /* the poll window opens at dot 32.5 of the first V-Blank line */
#define SCOPE_PIC_DOT 22        /* the picture's first dot */
#define SCOPE_VBLANK_LINE 225   /* first V-Blank line, without overscan */
#define SCOPE_PAL_US 18000      /* frames longer than this: PAL */

static struct beam_timing scope_beam;
static uint32_t scope_enabled;
static volatile uint32_t scope_target;
static volatile uint32_t scope_armed;
static struct sscope_state scope_state;
static uint8_t scope_prev;      /* held at the last poll */

static void scope_poll(struct snes_ctrl_port *p, uint32_t now) {
    struct npiso_scope_map *map = (struct npiso_scope_map *)wired_adapter.data[NPISO_SCOPE_PORT].output;
    uint8_t held = map->held;
    uint32_t xy = map->xy;
    union {
        uint8_t tmp[4];
        uint32_t tmp32;
    } raw = {0};

    /* FCTP 00nN, then the 1111 1111 signature: a 1 is the line pulled low. The report is what
     * was held at the previous poll, so the H/V latch of the frame in between matches it. */
    raw.tmp[0] = ~sscope_report(&scope_state, scope_prev);
    raw.tmp[1] = 0x00;
    p->hw->data_buf[0] = raw.tmp32;
    p->hw->data_buf[1] = 0;
    scope_prev = held;

    beam_timing_poll(&scope_beam, now);
    if ((held & (NPISO_SCOPE_FIRE | NPISO_SCOPE_CURSOR)) && !(held & NPISO_SCOPE_OFFSCREEN)
            && beam_timing_locked(&scope_beam)) {
        uint32_t pal = beam_timing_period(&scope_beam) > SCOPE_PAL_US * CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ;
        uint32_t lines_frame = pal ? 312 : 262;
        /* the picture's first line is the first after the next V-Blank */
        uint32_t lines = lines_frame - SCOPE_VBLANK_LINE + 1 + (xy >> 16);
        int32_t dots = (int32_t)(xy & 0xFFFF) + SCOPE_PIC_DOT - SCOPE_REF_DOT;

        scope_target = beam_timing_target(&scope_beam, lines, dots, lines_frame, 341);
        __asm__ __volatile__("memw" ::: "memory");
        scope_armed = 1;
    }
}

/* Where the CPU waits once the driver is up: nothing else runs on it but the latch interrupt,
 * which never comes during the picture, so the pulse lands within a few cycles of its time */
static void scope_loop(void) {
    while (1) {
        if (scope_armed) {
            uint32_t target = scope_target;
            if ((int32_t)(xthal_get_ccount() - target) >= 0) {
                GPIO.out_w1tc = P2_COPI_MASK;
                uint32_t start = xthal_get_ccount();
                while (xthal_get_ccount() - start < SCOPE_PULSE_CYCLES);
                GPIO.out_w1ts = P2_COPI_MASK;
                scope_armed = 0;
            }
        }
    }
}

static inline void load_mouse_axes(uint8_t port, uint8_t *axes) {
    uint8_t *relative = (uint8_t *)(wired_adapter.data[port].output + 2);
    int32_t *raw_axes = (int32_t *)(wired_adapter.data[port].output + 4);
    int32_t val = 0;

    for (uint32_t i = 0; i < 2; i++) {
        if (relative[i]) {
            val = atomic_clear(&raw_axes[i]);
        }
        else {
            val = raw_axes[i];
        }

        if (val > 127) {
            axes[i] = 128;
        }
        else if (val < -128) {
            axes[i] = 0;
        }
        else {
            if (val < 0) {
                axes[i] = ~((uint8_t)abs(val) | 0x80);
            }
            else {
                axes[i] = ~((uint8_t)val);
            }
        }
    }
}

static inline void load_buffer(uint8_t port) {
    switch (config.out_cfg[port].dev_mode) {
        case DEV_PAD:
        case DEV_PAD_ALT:   /* port 1: a pad; port 2: the Super Scope, see scope_poll */
        {
            union {
                uint8_t tmp[8];
                uint16_t tmp16[4];
                uint32_t tmp32[2];
            } raw = {0};

            switch (snes_ctrl_ports[port].format) {
                case SNES_PAD_FORMAT_BR_8BITS:
                    raw.tmp16[0] = wired_adapter.data[port].output16[0] | wired_adapter.data[port].output_mask16[0];
                    raw.tmp16[1] = wired_adapter.data[port].output16[1];
                    raw.tmp32[1] = wired_adapter.data[port].output32[1];
                    raw.tmp[1] &= 0xF0;
                    raw.tmp[1] |= 0x09;
                    break;
                case SNES_PAD_FORMAT_BR_4BITS:
                    raw.tmp16[0] = wired_adapter.data[port].output16[0] | wired_adapter.data[port].output_mask16[0];
                    raw.tmp[2] = (wired_adapter.data[port].output[2] & 0xF0) | (wired_adapter.data[port].output[3] >> 4);
                    raw.tmp[3] = (wired_adapter.data[port].output[4] & 0xF0) | (wired_adapter.data[port].output[5] >> 4);
                    raw.tmp[4] = (wired_adapter.data[port].output[6] & 0xF0) | (wired_adapter.data[port].output[7] >> 4);
                    raw.tmp[1] &= 0xF0;
                    raw.tmp[1] |= 0x08;
                    break;
                default:
                    raw.tmp16[0] = wired_adapter.data[port].output16[0] | wired_adapter.data[port].output_mask16[0];
                    raw.tmp16[1] = 0;
                    raw.tmp32[1] = 0;
                    raw.tmp[1] |= 0x0F;
                    if ((uint8_t)~wired_adapter.data[port].output[6] > 16) {
                        raw.tmp16[0] &= ~(1 << 13);
                    }
                    if ((uint8_t)~wired_adapter.data[port].output[7] > 16) {
                        raw.tmp16[0] &= ~(1 << 12);
                    }
                    break;
            }

            snes_ctrl_ports[port].hw->data_buf[0] = raw.tmp32[0];
            snes_ctrl_ports[port].hw->data_buf[1] = raw.tmp32[1];
            break;
        }
        case DEV_MOUSE:
        {
            union {
                uint8_t tmp[4];
                uint32_t tmp32;
            } raw = {0};
            raw.tmp[0] = 0xFF;
            raw.tmp[1] = wired_adapter.data[port].output[1];
            load_mouse_axes(port, &raw.tmp[2]);
            snes_ctrl_ports[port].hw->data_buf[0] = raw.tmp32;
            break;
        }
    }
}

static unsigned latch_isr(unsigned cause) {
    const uint32_t now = xthal_get_ccount();
    const uint32_t low_io = GPIO.acpu_int;
    const uint32_t high_io = GPIO.acpu_int1.intr;
    struct snes_ctrl_port *p;

    if (high_io & P1_LATCH_MASK) {
        for (uint32_t port = 0; port < SNES_PORT_MAX; port++) {
            union {
                uint32_t tmp32[2];
                uint8_t tmp[8];
            } raw;

            raw.tmp32[0] = snes_ctrl_ports[port].hw->data_buf[0];
            raw.tmp32[1] = snes_ctrl_ports[port].hw->data_buf[1];
            p = &snes_ctrl_ports[port];
            int16_t bit_cnt = PCNT.cnt_unit[p->pcnt_id].cnt_val;

            if (port == NPISO_SCOPE_PORT && scope_enabled) {
                scope_poll(p, now);
            }
            else {
                load_buffer(port);
            }
            p->hw->slave.sync_reset = 1;
            p->hw->slave.trans_done = 0;
            p->hw->cmd.usr = 1;

            ++wired_adapter.data[port].frame_cnt;
            npiso_gen_turbo_mask(&wired_adapter.data[port]);

            if (bit_cnt > 24) {
                uint8_t cmd_sentry = (raw.tmp[2] << 1) | (raw.tmp[3] >> 7);
                uint8_t cmd_data = (raw.tmp[3] << 1) | (raw.tmp[4] >> 7);
                if (cmd_sentry != 'r' && cmd_sentry != 'b') {
                    cmd_sentry = raw.tmp[2];
                    cmd_data = raw.tmp[3];
                }

                switch (cmd_sentry) {
                    case 'b':
                        if (cmd_data < SNES_PAD_FORMAT_CNT) {
                            if (cmd_data) {
                                snes_ctrl_ports[port].hw->slv_wrbuf_dlen.bit_len = 64 - 1;
                                snes_ctrl_ports[port].hw->slv_rdbuf_dlen.bit_len = 64 - 1;
                            }
                            else {
                                snes_ctrl_ports[port].hw->slv_wrbuf_dlen.bit_len = 33 - 1;
                                snes_ctrl_ports[port].hw->slv_rdbuf_dlen.bit_len = 33 - 1;
                            }
                            p->format = cmd_data;
                        }
                        break;
                    case 'r':
                        if (cmd_data != p->rumble_data
                                && config.out_cfg[port].acc_mode & ACC_RUMBLE) {
                            struct raw_fb fb_data = {0};
                            fb_data.data[0] = (cmd_data & 0xF0) >> 4;
                            fb_data.data[1] = cmd_data & 0x0F;
                            fb_data.header.wired_id = port;
                            fb_data.header.type = FB_TYPE_RUMBLE;
                            fb_data.header.data_len = 2;
                            adapter_q_fb(&fb_data);
                        }
                        p->rumble_data = cmd_data;
                        //ets_printf("%02X %02X\n", cmd_sentry, cmd_data);
                        break;
                }
            }
        }
    }
    PCNT.ctrl.cnt_rst_u0 = 1;
    PCNT.ctrl.cnt_rst_u0 = 0;
    PCNT.ctrl.cnt_rst_u1 = 1;
    PCNT.ctrl.cnt_rst_u1 = 0;

    if (high_io) GPIO.status1_w1tc.intr_st = high_io;
    if (low_io) GPIO.status_w1tc = low_io;
    return 0;
}

void snes_spi_init(uint32_t package) {
    gpio_config_t io_conf = {0};

    /* One PCNT block serves both ports: enable and reset it once, before
     * either unit is configured (a reset pulse later would wipe unit 0). */
    PERIPH_RCC_ATOMIC() {
        pcnt_ll_enable_bus_clock(0, true);
        pcnt_ll_reset_register(0);
    }

    for (uint32_t i = 0; i < SNES_PORT_MAX; i++) {
        struct snes_ctrl_port *p = &snes_ctrl_ports[i];

        /* Latch */
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.intr_type = GPIO_INTR_POSEDGE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pin_bit_mask = 1ULL << p->latch_pin;
        gpio_config_iram(&io_conf);
        esp_rom_gpio_connect_in_signal(p->latch_pin, p->latch_sig, false);
        esp_rom_gpio_connect_in_signal(p->latch_pin, p->pcnt_ctrl_sig, false);

        /* CIPO */
        gpio_set_level_iram(p->cipo_pin, 1);
        gpio_set_direction_iram(p->cipo_pin, GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(p->cipo_pin, p->cipo_sig, false, false);
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG_IRAM[p->cipo_pin], PIN_FUNC_GPIO);

        /* COPI */
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pin_bit_mask = 1ULL << p->copi_pin;
        gpio_config_iram(&io_conf);
        esp_rom_gpio_connect_in_signal(p->copi_pin, p->copi_sig, false);

        /* Clock */
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pin_bit_mask = 1ULL << p->clk_pin;
        gpio_config_iram(&io_conf);
        esp_rom_gpio_connect_in_signal(p->clk_pin, p->clk_sig, true);
        esp_rom_gpio_connect_in_signal(p->clk_pin, p->pcnt_pulse_sig, true);

        periph_ll_enable_clk_clear_rst(p->spi_mod);

        spi_init(&p->cfg);
        pcnt_init(p->pcnt_id);
    }

    if (npiso_is_scope(NPISO_SCOPE_PORT)) {
        /* IOBit: open drain, so the scope only ever pulls it low against the console's pull-up */
        GPIO.out_w1ts = P2_COPI_MASK;
        io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pin_bit_mask = 1ULL << P2_COPI_PIN;
        gpio_config_iram(&io_conf);
        esp_rom_gpio_connect_out_signal(P2_COPI_PIN, SIG_GPIO_OUT_IDX, false, false);
        beam_timing_init(&scope_beam, CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ);
        memset(&scope_state, 0, sizeof(scope_state));
        scope_prev = 0;
        scope_armed = 0;
        scope_enabled = 1;
    }

    intexc_alloc_iram(ETS_GPIO_INTR_SOURCE, GPIO_INTR_NUM, latch_isr);

    if (scope_enabled) {
        ets_printf("# Super Scope on port 2\n");
        scope_loop();
    }
}

/*
 * SWD pinout scanner for ESP32-S3 / Cardputer BMP
 *
 * Ported to plain C from geo-tp/ESP32-Bit-Pirate JtagService.cpp
 * which itself is a port of Aodrulez/blueTag (RP2040).
 *
 * Tries all combinations of SWDIO/SWCLK across a given set of GPIOs
 * using the full ARM ADIv5 SWDJ activation sequence:
 *   1. Dormant wake-up (128-bit selection alert + activation code)
 *   2. JTAG-to-SWD switch sequence (0xE79E)
 *   3. Line reset
 *   4. IDCODE read (0xA5 request, check ACK=OK, read 32-bit IDCODE)
 *
 * On Cardputer the only external GPIO breakout is the Grove port
 * (GPIO1=G1, GPIO2=G2). Pass {1, 2} as the pin set to scan both
 * possible SWDIO/SWCLK orderings — useful when the cable is plugged
 * in and you're not sure which wire landed on which pin.
 */

#include "jtag_scanner.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

#define SWD_DELAY_US         5
#define LINE_RESET_CYCLES    62
#define JTAG_TO_SWD_CMD      0xE79EU
#define SWDP_ACTIVATION_CODE 0x1AU

static uint8_t s_swdio, s_swclk;

static void swd_delay(void) { esp_rom_delay_us(SWD_DELAY_US); }

static void swd_clk_pulse(void) {
    gpio_set_level(s_swclk, 0); swd_delay();
    gpio_set_level(s_swclk, 1); swd_delay();
}

static void swd_set_read(void)  { gpio_set_direction(s_swdio, GPIO_MODE_INPUT); }
static void swd_set_write(void) { gpio_set_direction(s_swdio, GPIO_MODE_OUTPUT); }

static void swd_write_bit(bool v) {
    gpio_set_level(s_swdio, v ? 1 : 0);
    swd_clk_pulse();
}

static void swd_write_bits(uint32_t val, int len) {
    for (int i = 0; i < len; i++)
        swd_write_bit((val >> i) & 1);
}

static bool swd_read_bit(void) {
    bool v = gpio_get_level(s_swdio);
    swd_clk_pulse();
    return v;
}

static bool swd_read_ack(void) {
    uint8_t ack = 0;
    for (int i = 0; i < 3; i++)
        ack |= (swd_read_bit() ? 1U : 0U) << i;
    return ack == 0x01; /* OK */
}

static void swd_line_reset(void) {
    swd_set_write();
    gpio_set_level(s_swdio, 1);
    for (int i = 0; i < LINE_RESET_CYCLES; i++) swd_clk_pulse();
}

static void swd_arm_wake_up(void) {
    /* ARM ADIv5 §5.3.4 - switch out of dormant state */
    swd_set_write();
    gpio_set_level(s_swdio, 1);
    for (int i = 0; i < 8; i++) swd_clk_pulse();

    /* 128-bit selection alert sequence */
    const uint8_t alert[16] = {
        0x92, 0xF3, 0x09, 0x62, 0x95, 0x2D, 0x85, 0x86,
        0xE9, 0xAF, 0xDD, 0xE3, 0xA2, 0x0E, 0xBC, 0x19
    };
    for (int i = 0; i < 16; i++) swd_write_bits(alert[i], 8);

    swd_write_bits(0x00, 4);                  /* 4 idle clocks LOW */
    swd_write_bits(SWDP_ACTIVATION_CODE, 8);  /* ARM SWD-DP activation code */
}

static bool swd_try_connect(uint32_t *idcode_out) {
    swd_arm_wake_up();
    swd_line_reset();
    swd_write_bits(JTAG_TO_SWD_CMD, 16); /* JTAG-to-SWD select */
    swd_line_reset();
    swd_write_bits(0x00, 4);  /* idle */
    swd_write_bits(0xA5, 8);  /* read IDCODE: APnDP=0 RnW=1 A[2:3]=00 parity=1 */

    swd_set_read();
    swd_clk_pulse(); /* turnaround */

    if (!swd_read_ack()) {
        swd_set_write();
        return false;
    }

    uint32_t idcode = 0;
    for (int i = 0; i < 32; i++)
        idcode |= (swd_read_bit() ? 1U : 0U) << i;
    swd_read_bit(); /* parity, ignored */
    swd_set_write();
    swd_clk_pulse(); /* trailing idle */

    *idcode_out = idcode;
    return true;
}

bool swd_scanner_scan(
    const uint8_t *pins, int pin_count,
    swd_scan_result_t *result
) {
    for (int ci = 0; ci < pin_count; ci++) {
        for (int ii = 0; ii < pin_count; ii++) {
            if (ci == ii) continue;

            s_swclk = pins[ci];
            s_swdio = pins[ii];

            gpio_set_direction(s_swclk, GPIO_MODE_OUTPUT);
            gpio_set_direction(s_swdio, GPIO_MODE_OUTPUT);
            gpio_set_level(s_swdio, 1);
            gpio_set_level(s_swclk, 1);

            uint32_t idcode = 0;
            if (swd_try_connect(&idcode)) {
                result->swdio  = s_swdio;
                result->swclk  = s_swclk;
                result->idcode = idcode;
                return true;
            }

            gpio_set_direction(s_swdio, GPIO_MODE_INPUT);
            gpio_set_direction(s_swclk, GPIO_MODE_INPUT);
        }
    }
    return false;
}
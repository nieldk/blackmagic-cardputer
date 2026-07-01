#pragma once
#include <timing.h>

extern uint32_t swd_delay_cnt;

void platform_swdio_mode_float(void);
void platform_swdio_mode_drive(void);
void platform_gpio_set_level(int32_t gpio_num, uint32_t value);
void platform_gpio_set(int32_t gpio_num);
void platform_gpio_clear(int32_t gpio_num);
int platform_gpio_get_level(int32_t gpio_num);

void led_set_red(uint8_t value);
void led_set_green(uint8_t value);
void led_set_blue(uint8_t value);

#define PLATFORM_IDENT "ESP32-S3"

#define NO_USB_PLEASE

#if ENABLE_DEBUG == 1
#define PLATFORM_HAS_DEBUG
extern bool debug_bmp;
#endif

#define SET_RUN_STATE(state) \
    { led_set_green(255 * state); }
#define SET_IDLE_STATE(state) \
    { led_set_blue(255 * state); }
#define SET_ERROR_STATE(state) \
    { led_set_red(255 * state); }

#define TMS_SET_MODE() \
    do {               \
    } while(0)


#if defined(CONFIG_BOARD_TDISPLAY_S3_AMOLED)
#define NRST_PIN (42)
#define TMS_PIN (43)
#define TCK_PIN (44)
#define TDI_PIN (46)
#define TDO_PIN (45)

#undef PLATFORM_HAS_TRACESWO
#define TRACESWO_PIN 18
#endif

#if defined(CONFIG_BOARD_CARDPUTER)
// SWD only, over the Grove port (G1/G2). No spare Grove pin for NRST or
// JTAG (TDI/TDO) - use connect-under-reset (`monitor connect_rst enable`)
// or wire NRST separately from the internal header if you need it.
#define NRST_PIN (-1)
#define TMS_PIN (1)   // Grove G1 = GPIO1
#define TCK_PIN (2)   // Grove G2 = GPIO2
#define TDI_PIN (-1)
#define TDO_PIN (-1)

// Enables the ui_capture_active redirect in gdb_packet.c (see
// patches/gdb_packet.c.patch) so monitor-command output goes to the
// on-screen scrollback when the keyboard REPL invokes command_process().
#define PLATFORM_HAS_LOCAL_UI
#endif

// ON ESP32 we dont have the PORTS, this is dummy value until code is corrected
#define SWCLK_PORT (0)
#define SWCLK_PIN TCK_PIN
#define SWDIO_PIN TMS_PIN

#define gpio_set_val(port, pin, value)       \
    do {                                     \
        platform_gpio_set_level(pin, value); \
    } while(0);

#define gpio_set(port, pin) platform_gpio_set(pin)
#define gpio_clear(port, pin) platform_gpio_clear(pin)
#define gpio_get(port, pin) platform_gpio_get_level(pin)

#define SWDIO_MODE_FLOAT()           \
    do {                             \
        platform_swdio_mode_float(); \
    } while(0)

#define SWDIO_MODE_DRIVE()           \
    do {                             \
        platform_swdio_mode_drive(); \
    } while(0)

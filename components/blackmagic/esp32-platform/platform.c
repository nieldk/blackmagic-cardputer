#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "general.h"
#include <esp_log.h>
#include <driver/gpio.h>
#include <rom/ets_sys.h>

#include <hal/gpio_ll.h>
#include <esp_rom_gpio.h>

#include <esp_timer.h>

#include "platform.h"

uint32_t swd_delay_cnt = 0;
// static const char* TAG = "gdb-platform";

uint32_t target_clk_divider = 0;

void inline platform_swdio_mode_float(void) {
    // gpio_set_direction(SWDIO_PIN, GPIO_MODE_INPUT);
    // gpio_set_pull_mode(SWDIO_PIN, GPIO_FLOATING);

    // Faster variant
    gpio_ll_output_disable(&GPIO, SWDIO_PIN);
    gpio_ll_input_enable(&GPIO, SWDIO_PIN);
}

void inline platform_swdio_mode_drive(void) {
    gpio_set_direction(SWDIO_PIN, GPIO_MODE_OUTPUT);

    esp_rom_gpio_connect_out_signal(SWDIO_PIN, SIG_GPIO_OUT_IDX, false, false);
}

void inline platform_gpio_set_level(int32_t gpio_num, uint32_t value) {
    gpio_set_level(gpio_num, value);
}

void inline platform_gpio_set(int32_t gpio_num) {
    platform_gpio_set_level(gpio_num, 1);
}

void inline platform_gpio_clear(int32_t gpio_num) {
    platform_gpio_set_level(gpio_num, 0);
}

int inline platform_gpio_get_level(int32_t gpio_num) {
    int level = gpio_get_level(gpio_num);

    return level;
}

// init platform
void platform_init() {
}

// // set reset target pin level
// void platform_srst_set_val(bool assert) {
//     (void)assert;
// }

// // get reset target pin level
// bool platform_srst_get_val(void) {
//     return false;
// }

void inline platform_nrst_set_val(bool assert) {
    if (NRST_PIN >= 0)
        gpio_set_level(NRST_PIN, (uint8_t)assert);
}

bool inline platform_nrst_get_val(void) {
    if (NRST_PIN >= 0)
        return gpio_get_level(NRST_PIN);
    return false;
}

// target voltage
const char* platform_target_voltage(void) {
    return NULL;
}

// platform time counter
uint32_t platform_time_ms(void) {
    int64_t time_milli = esp_timer_get_time() / 1000;
    return ((uint32_t)time_milli);
}

// delay ms
void platform_delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// hardware version
int platform_hwversion(void) {
    return 0;
}

// set timeout
void platform_timeout_set(platform_timeout_s* t, uint32_t ms) {
    t->time = platform_time_ms() + ms;
}

// check timeout
bool platform_timeout_is_expired(const platform_timeout_s* t) {
    return platform_time_ms() > t->time;
}

// set interface freq
void platform_max_frequency_set(uint32_t freq) {
}

// get interface freq
uint32_t platform_max_frequency_get(void) {
    return 0;
}

void platform_target_clk_output_enable(bool enable) {
    (void)enable;
}

bool platform_spi_init(const spi_bus_e bus)
{
	(void)bus;
	return false;
}

bool platform_spi_deinit(const spi_bus_e bus)
{
	(void)bus;
	return false;
}

bool platform_spi_chip_select(const uint8_t device_select)
{
	(void)device_select;
	return false;
}

uint8_t platform_spi_xfer(const spi_bus_e bus, const uint8_t value)
{
	(void)bus;
	return value;
}

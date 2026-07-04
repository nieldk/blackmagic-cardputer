#pragma once

#include <stdint.h>
#include <stdbool.h>

// Result from a successful SWD pinout scan
typedef struct {
    uint8_t  swdio;
    uint8_t  swclk;
    uint32_t idcode;
} swd_scan_result_t;

// Brute-force SWD pinout scan across the given GPIO list.
// Tries all combinations of SWDIO/SWCLK using the full ARM SWDJ
// activation sequence (dormant wake-up + line reset + IDCODE read).
// Returns true and fills result on success.
bool swd_scanner_scan(
    const uint8_t *pins, int pin_count,
    swd_scan_result_t *result
);

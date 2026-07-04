// Cardputer 8x7 IO-matrix keyboard driver.
//
// Pinout and scan algorithm verified against m5stack/M5Cardputer
// (src/utility/Keyboard/KeyboardReader/IOMatrix.cpp) and the key layout
// table in src/utility/Keyboard/Keyboard.h. Re-implemented here directly
// on top of the ESP-IDF gpio driver since BMP's esp32-platform tree has
// no Arduino layer.

#include "keyboard.h"
#include <driver/gpio.h>
#include <rom/ets_sys.h>

static const gpio_num_t col_select[3] = {GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_11};
static const gpio_num_t row_input[7]  = {GPIO_NUM_13, GPIO_NUM_15, GPIO_NUM_3,
                                          GPIO_NUM_4,  GPIO_NUM_5,  GPIO_NUM_6, GPIO_NUM_7};

typedef struct {
    uint8_t x1, x2;
} chart_t;

// row bit j -> {x when output>3, x when output<=3}
static const chart_t x_map[7] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, {8, 9}, {10, 11}, {12, 13},
};

// Normal-layer character map, [y][x], 4 rows x 14 cols.
// '\0' marks non-printable keys you'll want to special-case (handled by
// keyboard_char_for() below) rather than feed into a text command buffer.
static const char key_value_map[4][14] = {
    {'`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b'},
    {'\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\'},
    {0 /*fn*/, 0 /*shift*/, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '\r'},
    {0 /*ctrl*/, 0 /*opt*/, 0 /*alt*/, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', ' '},
};

// Shift layer - mirrors key_value_map positions.
// '\0' means no shift variant (key unchanged or non-printable).
static const char key_shift_map[4][14] = {
    {'~', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b'},
    {'\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '|'},
    {0,    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"',  '\r'},
    {0,    0,   0,   'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',  ' '},
};

char keyboard_char_for(kb_point_t p) {
    if (p.y > 3 || p.x > 13)
        return 0;
    return key_value_map[p.y][p.x];
}

char keyboard_char_for_shift(kb_point_t p) {
    if (p.y > 3 || p.x > 13)
        return 0;
    char c = key_shift_map[p.y][p.x];
    return c ? c : key_value_map[p.y][p.x];
}

void keyboard_init(void) {
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(col_select[i]);
        gpio_set_direction(col_select[i], GPIO_MODE_OUTPUT);
        gpio_set_level(col_select[i], 0);
    }
    for (int i = 0; i < 7; i++) {
        gpio_reset_pin(row_input[i]);
        gpio_set_direction(row_input[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(row_input[i], GPIO_PULLUP_ONLY); // matrix is active-low
    }
}

static void set_output(uint8_t v) {
    gpio_set_level(col_select[0], v & 1);
    gpio_set_level(col_select[1], v & 2);
    gpio_set_level(col_select[2], v & 4);
}

static uint8_t get_input(void) {
    uint8_t buf = 0;
    for (int i = 0; i < 7; i++) {
        if (gpio_get_level(row_input[i]) == 0) // pressed = pulled low
            buf |= (1U << i);
    }
    return buf;
}

int keyboard_scan(kb_point_t* out, int max_keys) {
    int n = 0;
    for (int i = 0; i < 8 && n < max_keys; i++) {
        set_output(i);
        ets_delay_us(5); // let the 74HC138 output settle before reading rows
        uint8_t bits = get_input();
        if (!bits)
            continue;
        for (int j = 0; j < 7 && n < max_keys; j++) {
            if (!(bits & (1U << j)))
                continue;
            uint8_t x = (i > 3) ? x_map[j].x1 : x_map[j].x2;
            uint8_t y = (i > 3) ? (uint8_t)(i - 4) : (uint8_t)i;
            y         = 3 - y;
            out[n].x  = x;
            out[n].y  = y;
            n++;
        }
    }
    return n;
}
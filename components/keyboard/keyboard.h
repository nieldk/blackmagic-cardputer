#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define KEYBOARD_MAX_KEYS 8 // generous; physically at most a handful held at once

typedef struct {
    uint8_t x; // 0-13
    uint8_t y; // 0-3
} kb_point_t;

// Decoded character/key-code for one matrix position, normal layer only.
// Returns 0 if the position has no mapping (shouldn't happen on Cardputer's
// 4x14 layout, but guard anyway).
char keyboard_char_for(kb_point_t p);

// Shift layer variant - returns the shifted character for the position,
// falling back to the normal layer if no shift variant exists.
char keyboard_char_for_shift(kb_point_t p);

void keyboard_init(void);

// Scans the matrix once. Writes up to max_keys points into out, returns
// the number written. Call this periodically (e.g. every 20-30ms) from a
// task - it is not interrupt-driven.
int keyboard_scan(kb_point_t* out, int max_keys);
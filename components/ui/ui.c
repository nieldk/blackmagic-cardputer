// On-screen console + keyboard REPL for the Cardputer build.
//
// Captures monitor-command output (normally framed as GDB O-packets, see
// patches/gdb_packet.c.patch) into a scrollback buffer, renders it with
// HAGL, and lets the on-device keyboard build and submit the same
// command_process() calls a host `monitor ...` command would.

#include "ui.h"
#include "keyboard.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include <hagl_hal.h>
#include <hagl.h>
#include <font6x9.h>

#include "command.h"  // blackmagic-fw: command_process()
#include "gdb_main.h" // blackmagic-fw: extern target_s *cur_target;
#include "usb-glue.h" // usb_glue_gdb_set_line_state_callback()
#include "tusb.h"     // tud_cdc_n_write / tud_cdc_n_connected

#include "ui_debug.h"

static const char* TAG = "ui";

// ui_capture_active is defined in gdb_packet.c (see patches/gdb_packet.c.patch),
// guarded behind PLATFORM_HAS_LOCAL_UI which platform.h defines for the
// Cardputer board. host_session_active is owned here.
volatile bool host_session_active = false;

// --- Scrollback buffer -----------------------------------------------------

#define UI_COLS 40         // was 22 - testing whether the wrap width itself
                            // was capping line length below what the panel
                            // can actually display, vs. a real hardware/
                            // hagl clip limit at the old value
#define UI_VISIBLE_ROWS 10 // ~ (240 - prompt line) / 9px font, conservative
#define UI_HISTORY_ROWS 50 // actual stored history - was equal to
                            // UI_VISIBLE_ROWS before, meaning anything
                            // scrolled off-screen was already gone from
                            // memory, not just off-screen. Scrolling
                            // needs real history to scroll into.
#define CMD_BUF_SIZE 64

static char scrollback[UI_HISTORY_ROWS][UI_COLS + 1];
static int  scrollback_head  = 0; // next slot to write (ring buffer)
static int  scrollback_count = 0; // valid lines so far, capped at UI_HISTORY_ROWS
static char line_buf[64];
static int  line_buf_len = 0;
static SemaphoreHandle_t scrollback_mutex;

static void scrollback_push_line(const char* text) {
    xSemaphoreTake(scrollback_mutex, portMAX_DELAY);
    strncpy(scrollback[scrollback_head], text, UI_COLS);
    scrollback[scrollback_head][UI_COLS] = '\0';
    scrollback_head = (scrollback_head + 1) % UI_HISTORY_ROWS;
    if (scrollback_count < UI_HISTORY_ROWS)
        scrollback_count++;
    xSemaphoreGive(scrollback_mutex);
}

// Splits arbitrary text (which may contain embedded newlines from a
// multi-line command_process() result) into UI_COLS-wide scrollback rows.
// Also mirrors output to the second USB CDC port (UART, index 1).
// Both the scrollback and the CDC output use the same line_buf accumulator
// so that multi-chunk gdb_out() calls (which send one logical line as
// several separate write calls with no trailing newline) get buffered
// into complete lines before being emitted - otherwise \r\n gets injected
// mid-entry and the terminal output looks garbled.
// scrollback_flush_partial() must be called after command_process()
// returns to flush any partial line that didn't end with \n.
void ui_capture_write(const char* str) {
    for (const char* p = str; *p; p++) {
        if (*p == '\n' || line_buf_len >= UI_COLS) {
            line_buf[line_buf_len] = '\0';
            // Flush complete line to CDC UART port
            tud_cdc_n_write(1, line_buf, line_buf_len);
            tud_cdc_n_write(1, "\r\n", 2);
            tud_cdc_n_write_flush(1);
            // And to on-screen scrollback
            scrollback_push_line(line_buf);
            line_buf_len = 0;
            if (*p == '\n')
                continue;
        }
        line_buf[line_buf_len++] = *p;
    }
}

static void scrollback_flush_partial(void) {
    if (line_buf_len > 0) {
        line_buf[line_buf_len] = '\0';
        tud_cdc_n_write(1, line_buf, line_buf_len);
        tud_cdc_n_write(1, "\r\n", 2);
        tud_cdc_n_write_flush(1);
        scrollback_push_line(line_buf);
        line_buf_len = 0;
    }
}

// --- Rendering ---------------------------------------------------------

static hagl_backend_t* disp;

// 0 = showing the most recent UI_VISIBLE_ROWS lines (the live tail).
// Larger values shift the visible window further back into history.
static int scroll_offset = 0;

static int max_scroll_offset(void) {
    int m = scrollback_count - UI_VISIBLE_ROWS;
    return (m > 0) ? m : 0;
}

static void render(const char* prompt_line) {
    if (!disp)
        return;

    hagl_clear(disp);

    xSemaphoreTake(scrollback_mutex, portMAX_DELAY);

    int max_off = max_scroll_offset();
    if (scroll_offset > max_off)
        scroll_offset = max_off;
    if (scroll_offset < 0)
        scroll_offset = 0;

    int start = scrollback_count - UI_VISIBLE_ROWS - scroll_offset;
    if (start < 0)
        start = 0;

    wchar_t wbuf[UI_COLS + 1];
    for (int i = 0; i < UI_VISIBLE_ROWS; i++) {
        int logical = start + i;
        if (logical >= scrollback_count)
            break;
        int row = (scrollback_head - scrollback_count + logical + 2 * UI_HISTORY_ROWS) % UI_HISTORY_ROWS;
        int j   = 0;
        for (; scrollback[row][j]; j++)
            wbuf[j] = (wchar_t)scrollback[row][j];
        wbuf[j] = 0;
        if (j > 0)
            hagl_put_text(disp, wbuf, 0, i * 10, 0x7E00 /* green */, font6x9);
    }
    xSemaphoreGive(scrollback_mutex);

    if (prompt_line) {
        char full_prompt[CMD_BUF_SIZE + 24];
        if (scroll_offset > 0)
            snprintf(full_prompt, sizeof(full_prompt), "%s [scroll %d/%d]",
                      prompt_line, scroll_offset, max_off);
        else
            snprintf(full_prompt, sizeof(full_prompt), "%s", prompt_line);

        wchar_t wbuf2[UI_COLS + 1];
        int j = 0;
        for (; full_prompt[j] && j < UI_COLS; j++)
            wbuf2[j] = (wchar_t)full_prompt[j];
        wbuf2[j] = 0;
        hagl_put_text(disp, wbuf2, 0, UI_VISIBLE_ROWS * 10, 0xFFFF /* white */, font6x9);
    }

    hagl_flush(disp);
}

// --- Keyboard -> command buffer -> command_process() -----------------------

static void ui_task(void* pv) {
    disp = hagl_init();
    hagl_clear(disp);
    hagl_flush(disp);

    keyboard_init();

    static char cmd_buf[CMD_BUF_SIZE];
    int cmd_len = 0;

    kb_point_t keys[KEYBOARD_MAX_KEYS];
    kb_point_t prev_keys[KEYBOARD_MAX_KEYS];
    int prev_n = 0;

    char prompt[CMD_BUF_SIZE + 8];

    // Only re-render when something actually changed - calling
    // hagl_clear()+redraw unconditionally every 25ms produced a visible
    // full-screen clear-then-redraw flicker, since this build has no
    // double buffering (Cardputer has no PSRAM to put a second 63KB
    // framebuffer in). Force one render on first boot.
    bool dirty = true;

    while (1) {
        int n = keyboard_scan(keys, KEYBOARD_MAX_KEYS);

        // fn is held (not edge-triggered) so fn+key combos work while
        // fn stays down. Position per keyboard.c's key_value_map: (0,2).
        bool fn_held = false;
        for (int i = 0; i < n; i++) {
            if (keys[i].x == 0 && keys[i].y == 2) {
                fn_held = true;
                break;
            }
        }

        // Edge-detect: only act on keys that are newly pressed this scan
        // vs. last scan (debounce is implicit since this loop runs every
        // ~25ms and a held key just keeps reappearing in `keys`).
        for (int i = 0; i < n; i++) {
            bool was_pressed = false;
            for (int j = 0; j < prev_n; j++) {
                if (keys[i].x == prev_keys[j].x && keys[i].y == prev_keys[j].y) {
                    was_pressed = true;
                    break;
                }
            }
            if (was_pressed)
                continue;

            dirty = true;

            // fn+; = scroll back (older), fn+. = scroll forward (newer),
            // matching the arrow icons printed on those keys. Positions
            // per keyboard.c's key_value_map: ';'=(11,2), '.'=(11,3).
            if (fn_held && keys[i].x == 11 && keys[i].y == 2) {
                scroll_offset++;
                continue;
            }
            if (fn_held && keys[i].x == 11 && keys[i].y == 3) {
                scroll_offset--;
                if (scroll_offset < 0)
                    scroll_offset = 0;
                continue;
            }

            char c = keyboard_char_for(keys[i]);
            if (c == '\r') {
                // Jumping back to live view on Enter avoids a confusing
                // state where you submit a command while still looking
                // at old scrollback.
                scroll_offset = 0;
                if (cmd_len > 0 && !host_session_active) {
                    cmd_buf[cmd_len] = '\0';
                    char echo[CMD_BUF_SIZE + 8];
                    snprintf(echo, sizeof(echo), "> %s", cmd_buf);
                    scrollback_push_line(echo);

                    ui_capture_active = true;
                    int result = command_process(cur_target, cmd_buf);
                    scrollback_flush_partial();
                    ui_capture_active = false;

                    if (result < 0)
                        scrollback_push_line("(no such command)");
                    else if (result > 0)
                        scrollback_push_line("(command failed)");
                } else if (host_session_active) {
                    scrollback_push_line("(USB host attached)");
                }
                cmd_len = 0;
            } else if (c == '\b') {
                if (cmd_len > 0)
                    cmd_len--;
            } else if (c >= 0x20 && c < 0x7f) {
                if (cmd_len < CMD_BUF_SIZE - 1)
                    cmd_buf[cmd_len++] = c;
            }
            // modifier keys (fn/shift/ctrl/opt/alt, c == 0) intentionally
            // ignored for now - extend key_value_map's shift layer in
            // keyboard.c if you want uppercase/symbols.
        }

        memcpy(prev_keys, keys, sizeof(kb_point_t) * n);
        prev_n = n;

        if (dirty) {
            cmd_buf[cmd_len < CMD_BUF_SIZE - 1 ? cmd_len : CMD_BUF_SIZE - 1] = '\0';
            snprintf(prompt, sizeof(prompt), "> %s_", cmd_buf);
            render(prompt);
            dirty = false;
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

static void on_gdb_line_state(bool dtr, bool rts, void* ctx) {
    (void)rts;
    (void)ctx;
    if (!dtr) {
        // DTR drop is immediate - GDB session genuinely ended.
        host_session_active = false;
        return;
    }
    // DTR assert: wait 200ms before believing it's a real GDB session.
    // Windows CDC enumeration often causes a brief transient DTR pulse
    // on the GDB port when *any* port on the device is opened (including
    // COM30/UART), which would otherwise latch host_session_active true
    // and suppress the keyboard REPL permanently.
    vTaskDelay(pdMS_TO_TICKS(200));
    // Re-read: if DTR dropped during the delay it was just a transient.
    // TinyUSB doesn't give us a "current DTR state" getter, so we check
    // whether tud_cdc_n_connected() (which does gate on DTR) is still true.
    if (tud_cdc_n_connected(0)) {
        host_session_active = true;
    }
}

void ui_start(void) {
    scrollback_mutex = xSemaphoreCreateMutex();
    
    // 1. Clear history buffer
    for (int i = 0; i < UI_HISTORY_ROWS; i++) {
        scrollback[i][0] = '\0';
    }

    xSemaphoreTake(scrollback_mutex, portMAX_DELAY);

    strncpy(scrollback[0], " ____   __  __  ____  ", UI_COLS);
    strncpy(scrollback[1], " || ))  ||\\/||  || )) ", UI_COLS);
    strncpy(scrollback[2], " ||  )) ||  ||  ||    ", UI_COLS);
    strncpy(scrollback[3], " ||__// ||  ||  ||    ", UI_COLS);
    strncpy(scrollback[4], " -------------------- ", UI_COLS);
    strncpy(scrollback[5], "   Cardputer v1.0", UI_COLS);
    strncpy(scrollback[6], "   (C) 2026 Niel Nielsen", UI_COLS);
    strncpy(scrollback[7], "   SWD Probe Ready", UI_COLS);

    scrollback_head = 8;
    scrollback_count = 8;

    xSemaphoreGive(scrollback_mutex);

    // 3. Initialize the rest of the systems
    usb_glue_gdb_set_line_state_callback(on_gdb_line_state, NULL);

    xTaskCreate(&ui_task, "ui_task", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "started");
}


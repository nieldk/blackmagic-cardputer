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

#define UI_COLS 40
#define UI_VISIBLE_ROWS 10
#define UI_HISTORY_ROWS 50
#define CMD_BUF_SIZE 64

static char scrollback[UI_HISTORY_ROWS][UI_COLS + 1];
static int  scrollback_head  = 0;
static int  scrollback_count = 0;
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

void ui_capture_write(const char* str) {
    for (const char* p = str; *p; p++) {
        if (*p == '\n' || line_buf_len >= UI_COLS) {
            line_buf[line_buf_len] = '\0';
            tud_cdc_n_write(1, line_buf, line_buf_len);
            tud_cdc_n_write(1, "\r\n", 2);
            tud_cdc_n_write_flush(1);
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

    // =========================================================================
    // FANCY BOOT ANIMATION (Runs for ~2.5 seconds on startup)
    // =========================================================================
    const char* boot_steps[] = {
        "Initializing HAL...",
        "Checking USB Stack...",
        "Starting SWD Engine...",
        "Calibrating Voltages..."
    };
    wchar_t w_boot_buf[UI_COLS + 1];

    for (int step = 0; step < 4; step++) {
        for (int frame = 0; frame < 3; frame++) {
            hagl_clear(disp);

            char status_line[64];
            char spinner = (frame == 0) ? '/' : (frame == 1) ? '-' : '\\';
            snprintf(status_line, sizeof(status_line), "[ %c ] %s", spinner, boot_steps[step]);

            int j = 0;
            for (; status_line[j] && j < UI_COLS; j++)
                w_boot_buf[j] = (wchar_t)status_line[j];
            w_boot_buf[j] = 0;

            hagl_put_text(disp, w_boot_buf, 10, 30, 0x07E0, font6x9);

            hagl_put_text(disp, L"LOADING SUBSYSTEMS:", 10, 60, 0xFFFF, font6x9);

            int total_ticks = (step * 3) + frame + 1;
            wchar_t progress_bar[20] = L"[";
            int p = 1;
            for (; p <= total_ticks; p++) progress_bar[p] = L'=';
            for (; p <= 12; p++) progress_bar[p] = L' ';
            progress_bar[13] = L']';
            progress_bar[14] = 0;

            hagl_put_text(disp, progress_bar, 10, 75, 0x07E0, font6x9);

            hagl_flush(disp);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    hagl_clear(disp);
    hagl_put_text(disp, L"[ OK ] SYSTEM READY", 10, 45, 0x07E0, font6x9);
    hagl_flush(disp);
    vTaskDelay(pdMS_TO_TICKS(300));

    // =========================================================================
    // POPULATE LOGO INTO RETRO BUFFER
    // =========================================================================
    xSemaphoreTake(scrollback_mutex, portMAX_DELAY);

    strncpy(scrollback[0], " ____   __  __  ____  ", UI_COLS);
    strncpy(scrollback[1], " || ))  ||\\/||  || )) ", UI_COLS);
    strncpy(scrollback[2], " ||  )) ||  ||  ||    ", UI_COLS);
    strncpy(scrollback[3], " ||__// ||  ||  ||    ", UI_COLS);
    strncpy(scrollback[4], " -------------------- ", UI_COLS);
    strncpy(scrollback[5], "   Cardputer v1.0",     UI_COLS);
    strncpy(scrollback[6], "   (C) 2026 Niel Nielsen", UI_COLS);
    strncpy(scrollback[7], "   SWD Probe Ready",    UI_COLS);

    scrollback_head = 8;
    scrollback_count = 8;

    xSemaphoreGive(scrollback_mutex);
    // =========================================================================

    static char cmd_buf[CMD_BUF_SIZE];
    int cmd_len = 0;

    kb_point_t keys[KEYBOARD_MAX_KEYS];
    kb_point_t prev_keys[KEYBOARD_MAX_KEYS];
    int prev_n = 0;

    char prompt[CMD_BUF_SIZE + 8];

    bool dirty = true;

    while (1) {
        int n = keyboard_scan(keys, KEYBOARD_MAX_KEYS);

        // fn is held (not edge-triggered) so fn+key combos work while
        // fn stays down. Position per keyboard.c's key_value_map: (0,2).
        // shift is at (1,2) - same treatment.
        bool fn_held = false;
        bool shift_held = false;
        for (int i = 0; i < n; i++) {
            if (keys[i].x == 0 && keys[i].y == 2) fn_held = true;
            if (keys[i].x == 1 && keys[i].y == 2) shift_held = true;
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

            char c = shift_held ? keyboard_char_for_shift(keys[i]) : keyboard_char_for(keys[i]);

            if (c == '\r') {
                scroll_offset = 0;
                if (cmd_len > 0 && !host_session_active) {
                    cmd_buf[cmd_len] = '\0';
                    char echo[CMD_BUF_SIZE + 8];
                    snprintf(echo, sizeof(echo), "> %s", cmd_buf);
                    scrollback_push_line(echo);
                    ui_capture_active = true;
                    if (!ui_debug_dispatch(cmd_buf)) {
                        int result = command_process(cur_target, cmd_buf);
                        if (result < 0)
                            scrollback_push_line("(no such command)");
                        else if (result > 0)
                            scrollback_push_line("(command failed)");
                    }
                    scrollback_flush_partial();
                    ui_capture_active = false;
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
        host_session_active = false;
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    if (tud_cdc_n_connected(0)) {
        host_session_active = true;
    }
}

void ui_start(void) {
    scrollback_mutex = xSemaphoreCreateMutex();

    for (int i = 0; i < UI_HISTORY_ROWS; i++)
        scrollback[i][0] = '\0';

    usb_glue_gdb_set_line_state_callback(on_gdb_line_state, NULL);

    xTaskCreate(&ui_task, "ui_task", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "started");
}
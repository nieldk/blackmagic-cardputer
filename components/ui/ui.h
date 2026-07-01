#pragma once
#include <stdbool.h>

// Set/cleared by gdb_out() in blackmagic-fw when ui_capture_active is true
// (see patches/gdb_packet.c.patch). When active, monitor-command output is
// routed to the on-screen scrollback instead of a GDB O-packet.
extern volatile bool ui_capture_active;
void ui_capture_write(const char* str);

// Set/cleared from the USB CDC line-state callback - see
// patches/usb-glue.c.patch. While true, the on-device REPL refuses to call
// command_process() to avoid racing a live GDB session on the same target.
extern volatile bool host_session_active;

// Starts the keyboard-scan + screen-render + REPL task. Call once from
// app_main(), after display_init() and gdb_glue_init().
void ui_start(void);

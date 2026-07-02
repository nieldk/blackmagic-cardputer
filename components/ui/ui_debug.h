#pragma once
#include <stdbool.h>

/*
 * Standalone on-device debugger/flasher verbs for the Cardputer BMP.
 *
 * These are the commands a host GDB session would normally drive (attach,
 * flash/load, register and memory inspection, run control). They are NOT part
 * of the BMP monitor command table; they are dispatched from ui.c's REPL
 * before it falls through to command_process(), so the BMP core stays
 * unpatched.
 *
 * Recognized verbs:
 *   attach [N]            attach to target N from the last scan (default 1)
 *   detach                detach current target
 *   flash <path> [hex]    program an ELF or .bin from a mounted FS. For .bin,
 *                         [hex] is the load base (default 0x08000000).
 *   regs                  dump core registers
 *   mem <hex> <len>       hex-dump len bytes of target memory
 *   reset | halt | run | step | poll
 *
 * ui_debug_dispatch() returns true if it recognized and consumed the command
 * (in which case ui.c must NOT also call command_process). It works on a
 * private copy of the line, so an unrecognized command leaves the caller's
 * buffer intact for command_process().
 *
 * Output goes through ui_capture_write() (scrollback + CDC UART), matching the
 * monitor-command path. Wrap the call the same way ui.c already wraps
 * command_process: set ui_capture_active, dispatch, scrollback_flush_partial,
 * clear ui_capture_active.
 */
bool ui_debug_dispatch(const char *line);

#pragma once

/*
 * Single lock guarding all SWD/target access. The gdb FreeRTOS task runs at a
 * higher priority than ui_task and will preempt it, so host_session_active
 * alone is not enough: it only stops the REPL from starting a command while a
 * host is attached, not a host attaching mid-flash. Both sides take this lock
 * around any sequence that touches the target.
 *
 * The lock is taken AFTER gdb_getpacket() returns (packet framing does not
 * touch the target and blocks waiting for bytes), so the UI is never starved
 * while no host is talking.
 */

void target_lock_init(void); /* call once from app_main, before task creation */
void target_lock(void);
void target_unlock(void);

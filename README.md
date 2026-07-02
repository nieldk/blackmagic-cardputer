# Black Magic Probe on M5Stack Cardputer

A port of [litui/blackmagic-esp32s3](https://github.com/litui/blackmagic-esp32s3) to the
[M5Stack Cardputer](https://docs.m5stack.com/en/core/Cardputer), adding an on-device
keyboard REPL and scrollable display console alongside the standard BMP USB functionality.

On top of the monitor REPL, the Cardputer can now **attach, inspect, and flash a target
entirely on its own**, with no host GDB session. Point it at a firmware image on a microSD
card and it becomes a self-contained debugger/flasher: scan, attach, program from `.elf`
or `.bin`, dump registers and memory, and drive run control from the keyboard.

<img src="screen.jpg" alt="Cardputer Boot Screen" width="250">

## Hardware

- **MCU**: ESP32-S3FN8 (no PSRAM)
- **Display**: ST7789, 135×240, SPI
- **Keyboard**: 8×7 IO-matrix over 74HC138 demux
- **SWD**: Grove port (G1=SWDIO, G2=SWCLK, GND)
- **microSD**: SPI, holds firmware images for on-device flashing (mounted at `/sdcard`)

No NRST pin is wired. Use `monitor connect_rst enable` in GDB, or the `connect_rst`
monitor command in the REPL, if the target requires connect-under-reset.

## Features

- **Dual USB-CDC**: COM port 1 (GDB remote protocol), COM port 2 (UART/debug mirror)
- **On-device REPL**: Type BMP monitor commands directly on the Cardputer keyboard,
  output appears on screen
- **Standalone debugger/flasher**: Attach to a target, flash an `.elf`/`.bin` from the
  microSD, read registers and memory, and control run state, all without a host. See
  [Standalone debugging and flashing](#standalone-debugging-and-flashing-no-host)
- **Scrollable history**: 50-line scrollback buffer, navigate with `Fn+;` (back) and
  `Fn+.` (forward)
- **USB output mirror**: All command output simultaneously sent to COM port 2 as raw
  text — open in any terminal (flow control: None) to capture full output

## Supported monitor commands

```
version        Display firmware version info
help           Display help for monitor commands
jtag_scan      Scan JTAG chain for devices
swd_scan       Scan SWD interface for devices: [TARGET_ID]
swdp_scan      Deprecated: use swd_scan instead
auto_scan      Automatically scan all chain types for devices
frequency      Set minimum high and low times: [FREQ]
targets        Display list of available targets
morse          Display morse error message
halt_timeout   Timeout to wait until Cortex-M is halted: [TIME]
connect_rst    Configure connect under reset: [enable|disable]
reset          Pulse the nRST line: [PULSE_LEN, default 0ms]
tdi_low_reset  Pulse nRST with TDI set low (wakes certain targets eg LPC82x)
rtt            RTT control: [enable|disable|status|channel|ident|cblock|ram|poll]
heapinfo       Set semihosting heapinfo: HEAP_BASE HEAP_LIMIT STACK_BASE STACK_LIMIT
debug_bmp      Output BMP debug strings to second vcom: [enable|disable]
```

Target-specific commands (eg nRF52 recovery AP) become available once a target is
attached via `swd_scan`.

## Standalone debugging and flashing (no host)

These verbs run on the Cardputer itself. They are not part of the BMP monitor table, they
are handled by the REPL before it falls through to `command_process()`, so they coexist
with every monitor command above. Unlike monitor commands, they set and use the device's
own attached target, so the whole attach/flash/inspect cycle works with no GDB connected.

```
attach [N]          Attach to target N from the last scan (default 1)
detach              Detach the current target
flash <path> [hex]  Program an ELF or .bin from /sdcard. For a raw .bin, [hex] is
                    the load base (default 0x08000000). ELF uses each segment's LMA.
regs                Dump core registers
mem <hex> <len>     Hex-dump len bytes of target memory (len capped at 256)
reset               Reset the attached core, or pulse nRST if none attached
halt                Request halt
run                 Resume execution
step                Single-step
poll                Report halt reason
```

`reset` is dual-purpose: with a target attached it resets the core, with none
attached it falls through to the monitor `reset` and pulses the nRST line, so the
standalone verb never hides the monitor one.

Typical standalone flow, all from the keyboard:

```
swd_scan                       # populate the target list
connect_rst enable             # only if the target needs it (no NRST wired)
attach 1                       # attach to target 1, sets the on-device target
flash /sdcard/firmware.elf     # erase, program, verify from SD
reset
run
```

`flash` auto-detects ELF vs raw binary from the file header. ELF images are programmed
per program-header at each segment's physical address (LMA), then read back and verified.
Progress and the final `segments / bytes / entry` summary print to the scrollback and the
COM port 2 mirror.

Typing `help` on-device prints a curated, single-screen listing of both the standalone
verbs and the common monitor commands, formatted to fit the 40-column display without
wrapping. Because that list is static, target-specific commands (which appear after
attach) and any custom monitor commands are not in it, use `help all` to fall through to
the full native listing instead.

## microSD

Firmware images are read from a FAT-formatted microSD, mounted at `/sdcard` at boot. Copy
your `.elf` or `.bin` to the card, insert it, and reference it by path in `flash`.

The SD SPI pins in `main/sdcard.c` default to the documented Cardputer values
(CLK 40, MISO 39, MOSI 14, CS 12). **Confirm these against your unit.** If the card shares
the SPI host with the ST7789 display, set `SDCARD_SHARED_BUS 1` and point `SD_SPI_HOST` at
the host the display already initialized, so the bus is not initialized twice.

If no card is present at boot the mount is skipped with a warning and flash-from-file is
simply unavailable, everything else (probe, REPL, host GDB) works unchanged.

## Supported targets (compiled in)

nRF51/nRF52, STM32F1/F4/G0/H5/H7/L0/L4/MP15, RP2040, SAMD/SAM3x/SAM4L/SAMx5x,
LPC11xx/15xx/17xx/40xx/43xx/546xx/55xx, Kinetis, EFM32, iMX-RT, Renesas RA/RZ,
RISC-V (RV32/RV64), nRF91, and more — see `blackmagic-fw/src/target/` for the full list.

## Build requirements

- **ESP-IDF v5.1.4** — not 5.3.x. TinyUSB's `dcd_esp32sx.c` (ESP32-S3 USB-OTG driver)
  depends on SoC register names that were reorganized in later IDF releases.

```sh
# Source the correct IDF version before building
. ~/esp/esp-idf-5.1/export.sh
```

## Build

```sh
git clone https://codeberg.org/nieldk/blackmagic-cardputer.git
cd blackmagic-cardputer
git submodule update --init --recursive

# Apply the required manual patch to gdb_packet.c (see Patching section below)

rm -f sdkconfig
rm -rf build
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.cardputer set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.cardputer build
```

Both commands must receive `-D SDKCONFIG_DEFAULTS=sdkconfig.defaults.cardputer`. Running
`set-target` without it writes stock defaults that silently override the Cardputer
config, since `SDKCONFIG_DEFAULTS` only seeds options not already present in an existing
`sdkconfig`.

## Flash

```sh
idf.py flash
```

Or flash just the app after an incremental build:

```sh
idf.py app-flash
```

## Patching

`gdb_packet.c` inside the `blackmagic-fw` submodule requires a manual edit — the
submodule points at an older fork where the include chain does not reach `platform.h`,
so the `PLATFORM_HAS_LOCAL_UI` guard must be added explicitly.

Edit `components/blackmagic/blackmagic-fw/src/gdb_packet.c` and add the following
immediately before the `gdb_out()` function:

```c
#include "platform.h"

#if defined(PLATFORM_HAS_LOCAL_UI)
volatile bool ui_capture_active = false;
void ui_capture_write(const char *str);
#endif
```

And inside `gdb_out()`, at the very top of the function body:

```c
#if defined(PLATFORM_HAS_LOCAL_UI)
    if (ui_capture_active) {
        ui_capture_write(buf);
        return;
    }
#endif
```

## Standalone frontend source layout

The standalone debugger/flasher lives entirely in the app layer, the BMP core is not
modified beyond the `gdb_packet.c` patch above.

In `components/ui/`:

- `ui_debug.c` / `.h` — the standalone verbs (`attach`, `flash`, `regs`, `mem`, run
  control), dispatched from the REPL before `command_process()`. Also holds the
  `target_controller_s` used for on-device attach and the flash sink bound to
  `target_flash_*`.
- `bmp_standalone_load.c` / `.h` — ELF32/`.bin` program-header loader. It walks the image
  from a `FILE*` and drives flash through a small callback sink, so it does not depend on
  the exact BMP flash signatures. Streams in 2 KB chunks (no PSRAM).
- `target_lock.c` / `.h` — a single mutex shared by the GDB task and the REPL.

In `main/`:

- `sdcard.c` / `.h` — microSD SPI mount at `/sdcard`.

Wiring, for anyone rebasing the fork:

- `ui.c`, in the Enter branch, calls `ui_debug_dispatch()` first and only falls through to
  `command_process()` when it returns false.
- `main.c` calls `target_lock_init()` and `sdcard_mount()` in `app_main()`, and wraps
  `gdb_main()` in the GDB task with `target_lock()` / `target_unlock()`.

The lock matters: the GDB task runs at a higher FreeRTOS priority than the UI task and
will preempt it. Without the lock, a host asserting DTR and sending a packet during an
on-device flash would drive the same SWD DP mid-erase. The lock is taken after
`gdb_getpacket()` returns, so waiting for host bytes never starves the keyboard.

## Wiring (SWD)

| Grove pin | GPIO | Target |
|-----------|------|--------|
| G1 | GPIO1 | SWDIO |
| G2 | GPIO2 | SWCLK |
| GND | GND | GND |

Power the target separately — the Grove port does not supply target power in this build.

## Connecting with GDB

The host GDB path is unchanged and still available for source-level debugging:

```sh
arm-none-eabi-gdb your_firmware.elf
(gdb) target extended-remote COM29      # or /dev/ttyACM0 on Linux
(gdb) monitor connect_rst enable        # if no NRST wired
(gdb) monitor swd_scan
(gdb) attach 1
(gdb) load
(gdb) break main
(gdb) continue
```

## On-device REPL

The Cardputer keyboard lets you run monitor commands and the standalone verbs without a
host GDB session:

- Type a command and press **Enter** to execute
- **help** lists the standalone verbs and common monitor commands, **help all** shows the
  full native command list
- **Fn + ;** — scroll back into history
- **Fn + .** — scroll forward to live view
- Pressing **Enter** always snaps back to live view

All command output also appears on **COM30** (the second USB-CDC port) as plain text.
Open it in any terminal with flow control set to **None**.

## Known limitations

- No NRST pin — use `connect_rst enable` for targets that require it
- Keyboard shift/Fn/Ctrl layers not decoded — only lowercase letters, digits, and
  unshifted symbols available in the REPL
- No double buffering (no PSRAM) — display redraws only on keypress to avoid flicker
- SWD only (JTAG wiring not brought out to the Grove port in this configuration)
- On-device `flash` erases the union span of an ELF's loadable segments in one pass, so a
  preserved config sector wedged between app regions would be wiped. Flash such layouts
  from host GDB, or switch to per-segment erase in `bmp_standalone_load.c`.
- SD pins default to the documented Cardputer values, confirm for your unit and set
  `SDCARD_SHARED_BUS` if the card shares the display SPI bus
- `mem` dumps are capped at 256 bytes per command, and `regs` prints raw register indices,
  not names

## Credits

Based on [litui/blackmagic-esp32s3](https://github.com/litui/blackmagic-esp32s3) and
the [Black Magic Debug](https://black-magic.org) project.
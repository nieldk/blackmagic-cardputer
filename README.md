# Black Magic Probe on M5Stack Cardputer

A port of [litui/blackmagic-esp32s3](https://github.com/litui/blackmagic-esp32s3) to the
[M5Stack Cardputer](https://docs.m5stack.com/en/core/Cardputer), adding an on-device
keyboard REPL and scrollable display console alongside the standard BMP USB functionality.

<img src="screen.jpg" alt="Cardputer Boot Screen" width="250">

## Hardware

- **MCU**: ESP32-S3FN8 (no PSRAM)
- **Display**: ST7789, 135×240, SPI
- **Keyboard**: 8×7 IO-matrix over 74HC138 demux
- **SWD**: Grove port (G1=SWDIO, G2=SWCLK, GND)

No NRST pin is wired. Use `monitor connect_rst enable` in GDB if the target requires
connect-under-reset.

## Features

- **Dual USB-CDC**: COM port 1 (GDB remote protocol), COM port 2 (UART/debug mirror)
- **On-device REPL**: Type BMP monitor commands directly on the Cardputer keyboard,
  output appears on screen
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

## Wiring (SWD)

| Grove pin | GPIO | Target |
|-----------|------|--------|
| G1 | GPIO1 | SWDIO |
| G2 | GPIO2 | SWCLK |
| GND | GND | GND |

Power the target separately — the Grove port does not supply target power in this build.

## Connecting with GDB

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

The Cardputer keyboard lets you run monitor commands without a host GDB session:

- Type a command and press **Enter** to execute
- **Fn + ;** — scroll back into history
- **Fn + .** — scroll forward to live view
- Pressing **Enter** always snaps back to live view

All command output also appears on **COM30** (the second USB-CDC port) as plain text.
Open it in any terminal with flow control set to **None**.

## Known limitations

- No NRST pin — use `monitor connect_rst enable` for targets that require it
- Keyboard shift/Fn/Ctrl layers not decoded — only lowercase letters, digits, and
  unshifted symbols available in the REPL
- No double buffering (no PSRAM) — display redraws only on keypress to avoid flicker
- SWD only (JTAG wiring not brought out to the Grove port in this configuration)

## Credits

Based on [litui/blackmagic-esp32s3](https://github.com/litui/blackmagic-esp32s3) and
the [Black Magic Debug](https://black-magic.org) project.
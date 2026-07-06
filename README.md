# Black Magic Probe on M5Stack Cardputer

A port of [litui/blackmagic-esp32s3](https://github.com/litui/blackmagic-esp32s3) to the
[M5Stack Cardputer](https://docs.m5stack.com/en/core/Cardputer), adding an on-device
keyboard REPL and scrollable display console alongside the standard BMP USB functionality.

On top of the monitor REPL, the Cardputer can **attach, inspect, and flash a target
entirely on its own**, with no host GDB session. Point it at a firmware image — dragged
onto its internal USB drive or copied to a microSD card — and it becomes a self-contained
debugger/flasher: scan, attach, program from `.elf` or `.bin`, dump registers and memory,
and drive run control from the keyboard.

<img src="screen.jpg" alt="Cardputer Boot Screen" width="250">

## Hardware

- **MCU**: ESP32-S3FN8 (8 MB flash, no PSRAM)
- **Display**: ST7789, 135×240, SPI
- **Keyboard**: 8×7 IO-matrix over 74HC138 demux
- **SWD**: Grove port (G1=SWDIO, G2=SWCLK, GND)
- **microSD**: SPI, holds firmware images for on-device flashing (mounted at `/sdcard`)

No NRST pin is wired. Use `monitor connect_rst enable` in GDB, or the `connect_rst`
monitor command in the REPL, if the target requires connect-under-reset.

## Features

- **Dual USB-CDC**: COM port 1 (GDB remote protocol), COM port 2 (UART/debug mirror).
  Switchable to a **GDB + USB mass-storage** mode, see below.
- **USB storage drive**: `usbmode msc` exposes a 3 MB internal FAT partition as a USB
  drive. Drag a firmware image onto it from your PC and flash it on-device — no SD card,
  no host GDB. `usbmode dual` switches back to the two serial ports. See
  [USB storage mode](#usb-storage-mode).
- **On-device REPL**: Type BMP monitor commands directly on the Cardputer keyboard.
  Full shift layer supported — hold `Aa` for uppercase and symbols including `_`, `!`,
  `@`, `#`, `$`, `%`, etc.
- **Standalone debugger/flasher**: Attach to a target, flash an `.elf`/`.bin` from the
  internal drive or microSD, read registers and memory, and control run state, all
  without a host. See [Standalone debugging and flashing](#standalone-debugging-and-flashing-no-host)
- **Emulated target**: `monitor emulate` registers a synthetic STM32F1 that BMP's own
  ADIv5 stack enumerates with no pins driven — probe, attach, flash, verify, and read
  registers with no target hardware attached. See [Emulated target](#emulated-target-no-hardware)
- **Auto pin detection**: `swd_scan` automatically tries both Grove pin orderings before
  connecting — no need to care which wire landed on SWDIO vs SWCLK
- **Scrollable history**: 50-line scrollback buffer, navigate with `Fn+;` (back) and
  `Fn+.` (forward)
- **USB output mirror**: All command output simultaneously sent to COM port 2 as raw
  text, open in any terminal (flow control: None) to capture full output
- **Boot animation**: Startup splash with progress bar and BMP logo

## Supported monitor commands

```
version        Display firmware version info
help           Display help for monitor commands
jtag_scan      Scan JTAG chain for devices
swd_scan       Scan SWD interface for devices: [TARGET_ID]
swdp_scan      Deprecated: use swd_scan instead
emulate        Stand up an emulated STM32F103 target (no pins)
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
swd_pinout     Brute-force SWD pinout scan across Grove pins: [pin1 pin2 ...]
```

Target-specific commands (eg nRF52 recovery AP) become available once a target is
attached via `swd_scan`.

## Standalone debugging and flashing (no host)

These verbs run on the Cardputer itself. They are dispatched from the REPL before it
falls through to `command_process()`, so they coexist with every monitor command above.

```
attach [N]          Attach to target N from the last scan (default 1)
detach              Detach the current target
flash <path> [hex]  Program an ELF or .bin from the internal drive / SD. For a raw
                    .bin, [hex] is the load base (default 0x08000000). ELF uses each
                    segment's LMA.
regs                Dump core registers
mem <hex> <len>     Hex-dump len bytes of target memory (len capped at 256)
reset               Reset the attached core, or pulse nRST if none attached
halt                Request halt
run                 Resume execution
step                Single-step
poll                Report halt reason
usbmode dual|msc    Switch USB mode (reboots): dual-CDC, or GDB + storage
```

Typical standalone flow, all from the keyboard:

```
swd_scan                       # populate the target list
connect_rst enable             # only if the target needs it (no NRST wired)
attach 1                       # attach to target 1
flash /sdcard/firmware.elf     # erase, program, verify
reset
run
```

`flash` auto-detects ELF vs raw binary from the file header, and rejects program headers
whose load addresses fall outside the target's flash/RAM map (so an image built for the
wrong chip fails cleanly rather than corrupting flash). Progress and the final
`segments / bytes / entry` summary print to the scrollback and the COM port 2 mirror.

## USB storage mode

The Cardputer can present a **3 MB internal FAT partition** as a USB mass-storage drive,
so you can drag a firmware image onto it from any host and flash it on-device — no SD card
and no host GDB needed. Because the ESP32-S3 USB-OTG can't bring up two CDC ports *and*
mass storage at once (it runs out of IN endpoints), the drive lives in a separate USB
mode you switch into:

```
usbmode msc     # reboots as: GDB CDC + USB mass-storage drive ("BMP Storage")
usbmode dual    # reboots as: dual-CDC (GDB + UART mirror) — the default
usbmode         # print the current mode
```

The selected mode is saved in NVS and survives reboots. GDB stays available in `msc`
mode (it's the single CDC), so you only give up the second UART-mirror port while the
drive is mounted.

Typical no-SD workflow:

```
usbmode msc                      # on the Cardputer keyboard; it reboots
# on your PC: copy firmware.elf onto the "BMP Storage" drive, then EJECT it
# back on the Cardputer keyboard:
emulate                          # or swd_scan for a real target
attach 1
flash /sdcard/firmware.elf       # reads the file you just dropped
usbmode dual                     # optional: back to dual-CDC
```

Notes:

- **Eject before flashing.** `flash` borrows the filesystem back from the host while it
  reads, so copy the file and safely-remove/eject the drive before running `flash`,
  otherwise the host's writes may not be flushed yet.
- **`/sdcard` is the internal drive in this mode.** The mount point is shared, so the
  same `flash /sdcard/...` path works whether the file came from the internal drive or a
  microSD.
- The partition is a 512-byte-sector FAT (Windows refuses 4K-native removable media). It
  is created and formatted empty on first boot; content persists across reboots.
- Requires the 8 MB flash + custom partition table config (see
  [Build requirements](#build-requirements)); a `storage` partition must exist.

## Emulated target (no hardware)

`monitor emulate` stands up a synthetic **STM32F1 medium density** target inside the
firmware — no SWD pins driven, nothing wired to the Grove port. BMP's own ADIv5 stack
enumerates it exactly as it would a real chip: DP power-up, single AP scan, ROM-table
walk, `cortexm_probe`, then `stm32f1_probe`. It's handy for exercising the
probe/attach/flash/verify/register paths, the REPL, and host-GDB connectivity on the
bench with no target hardware present.

```
emulate                # register one emulated STM32F1 medium density target
attach 1               # attaches as: STM32F1 medium density / M3
regs                   # sp/msp=0x20005000, pc=0x08000100, xpsr Thumb bit
mem 0x08000000 16      # SP, reset vector, then 0xffffffff (erased) or your image
```

It also works over host GDB — `monitor emulate` then `attach 1` from your GDB session,
then `load` / `compare-sections` / `info registers`. `emulate` and a real `swd_scan` are
mutually exclusive: only one debug port lives at a time, so run one or the other.

What it is and isn't:

- It is a **transport / enumeration / flash / register model**, not a CPU simulator.
  There is no instruction execution, so `run`/`step`/`continue` don't advance code.
- **Flash programming and verification work.** The full 128 KiB flash is modelled as
  sparse 1 KiB pages allocated on demand, so `flash`, GDB `load` and `compare-sections`
  erase, program and verify correctly. RAM used tracks the size of the image programmed,
  not the whole 128 KiB. NOR semantics are honoured (a page must be erased before it can
  be reprogrammed). The FPEC accepts the unlock/erase/program sequence and completes
  instantly (`BSY` never asserts).
- **Core registers are modelled** via the DCRSR/DCRDR transfer interface, seeded to a
  plausible halted-at-reset state (`sp`/`msp` = `0x20005000`, `pc` = `0x08000100`, xPSR
  Thumb bit). `regs` / `info registers` read them, and register writes (e.g. GDB setting
  `pc` after `load`) persist — but since nothing executes, they don't change on their own.
- SRAM is the full 20 KiB. Flash is 128 KiB (device id `0x410`).
- The identity registers report a genuine STM32F103: DBGMCU_IDCODE `0x20036410`,
  CPUID `0x412fc231` (Cortex-M3).

The implementation lives in `blackmagic-fw/src/target/emu_target.c` (the STM32F103 memory,
CoreSight and register model) and `emu_shim.c` (the ADIv5 DP/AP transport, with posted-read
semantics matching `firmware_swdp_read`). Both are kept at the repo root and copied into
the submodule at build time, like the other patched files.

## Internal storage & microSD

Firmware images can come from two places, both reachable at `/sdcard`:

- **Internal USB drive** (see [USB storage mode](#usb-storage-mode)) — a 3 MB FAT
  partition on the ESP32-S3's own flash, exposed over USB-MSC. This is the no-SD path.
- **microSD card** — a FAT-formatted card. Copy your `.elf`/`.bin` to it and reference it
  by path in `flash`. The SD SPI pins in `main/sdcard.c` default to the documented
  Cardputer values; **confirm these against your unit**. If the card shares the SPI host
  with the ST7789 display, set `SDCARD_SHARED_BUS 1` in `sdcard.c` and point `SD_SPI_HOST`
  at the host the display already initialized, so the bus is not initialized twice.

## Supported targets (compiled in)

nRF51/nRF52, STM32F1/F4/G0/H5/H7/L0/L4/MP15, RP2040, SAMD/SAM3x/SAM4L/SAMx5x,
LPC11xx/15xx/17xx/40xx/43xx/546xx/55xx, Kinetis, EFM32, iMX-RT, Renesas RA/RZ,
RISC-V (RV32/RV64), nRF91, and more, see `blackmagic-fw/src/target/` for the full list.

An emulated STM32F1 medium density target is also available with no hardware, see
[Emulated target](#emulated-target-no-hardware).

## Build requirements

- **ESP-IDF v5.1.4**, not 5.3.x. TinyUSB's `dcd_esp32sx.c` (ESP32-S3 USB-OTG driver)
  depends on SoC register names that were reorganized in later IDF releases.

```sh
. ~/esp/esp-idf-5.1/export.sh
```

The USB storage feature needs 8 MB flash and a custom partition table with a `storage`
partition. Both are set by `sdkconfig.defaults.cardputer` (flash size, custom partition
table) and `partitions.csv`, so a clean build picks them up automatically — no menuconfig.
**If you are upgrading an existing checkout, delete `sdkconfig` (or `idf.py fullclean`)
first**, otherwise a stale `sdkconfig` keeps the old single-app / 4 KB-sector settings and
the drive won't appear.

## Build

```sh
git clone https://codeberg.org/nieldk/blackmagic-cardputer.git
cd blackmagic-cardputer
git submodule update --init --recursive
rm -f sdkconfig
rm -rf build
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.cardputer set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.cardputer build
```

Both commands must receive `-D SDKCONFIG_DEFAULTS=sdkconfig.defaults.cardputer`. Running
`set-target` without it writes stock defaults that silently override the Cardputer config.
You can confirm the partition table took effect with `idf.py partition-table` — it should
list a `storage, data, fat` entry.

The patched `gdb_packet.c` and `command.c`, along with the emulated-target sources
`emu_target.c` / `emu_target.h` / `emu_shim.c` / `emu_shim.h`, are kept in the repo root
and copied automatically into the `blackmagic-fw` submodule during the build. No manual
patching required after `git submodule update --init --recursive`. Because the copy runs
at configure time, if you edit any of these repo-root files, run `idf.py reconfigure`
(or `rm -rf build`) before building so the fresh copy is picked up. (The USB-storage
sources live directly in `components/`, so they are not copied.)

## Flash

```sh
idf.py flash
```

## Wiring (SWD)

| Grove pin | GPIO | Target |
|-----------|------|--------|
| G1 | GPIO1 | SWDIO |
| G2 | GPIO2 | SWCLK |
| GND | GND | GND |

Power the target separately. `swd_scan` automatically tries both pin orderings, so the
cable can be plugged in either way. Use `swd_pinout` to explicitly identify which pin is
which.

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

- Type a command and press **Enter** to execute
- Hold **Aa** (shift) for uppercase and symbols: `_`, `!@#$%^&*()`, `{}|`, etc.
- **Fn + ;**: scroll back into history
- **Fn + .**: scroll forward to live view
- Pressing **Enter** always snaps back to live view

All command output also appears on **COM30** as plain text. Open in any terminal with
flow control set to **None**.

## Known limitations

- No NRST pin, use `connect_rst enable` for targets that require it
- No double buffering (no PSRAM), display redraws only on keypress to avoid flicker
- SWD only, JTAG wiring not brought out to the Grove port
- Keyboard Fn/Ctrl layers not decoded, only Fn+;/Fn+. scroll shortcuts are handled
- RTT and semihosting compiled in but untested
- USB mass storage and the second (UART-mirror) CDC can't be active at the same time —
  the S3 USB-OTG runs out of IN endpoints — so `usbmode msc` drops the UART port while
  the drive is mounted; GDB stays available
- `monitor emulate` is a transport/flash/register model, not a CPU simulator: flash
  program/erase/verify and register read/write work, but the core reads as permanently
  halted and `run`/`step` don't advance code
- SD pins default to documented Cardputer values, confirm for your unit and set
  `SDCARD_SHARED_BUS` if the card shares the display SPI bus

## Credits

Based on [litui/blackmagic-esp32s3](https://github.com/litui/blackmagic-esp32s3) and
the [Black Magic Debug](https://black-magic.org) project.
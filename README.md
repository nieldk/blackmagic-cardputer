# STM32F1 Emulation for Black Magic Probe

Two complementary emulator implementations:

## 1. Host Python Emulator (stm32emu.py)

Runs on your PC. Provides a Cortex-M3 CPU emulator + GDB RSP server for testing firmware before it reaches real hardware.

**Use case:** Validate STM32F103 firmware compilation and basic execution without a physical debugger or target.

**Features:**
- Unicorn Engine Cortex-M3 CPU emulation
- GDB RSP TCP server (listens on localhost:3333)
- Models RCC, SysTick, MMIO registers
- Loads ELF or binary firmware
- Breakpoints, stepping, register inspection in GDB

**Usage:**
```bash
cd stm32emu
python3 stm32emu.py firmware.elf
# In another terminal:
arm-none-eabi-gdb
(gdb) target extended-remote :3333
(gdb) info registers
(gdb) step
```

**Limitation:** No peripheral I/O simulation (GPIO, UART, etc. read zero).

---

## 2. Firmware Emulator (emu_target.c/h + emu_shim.c/h)

Runs **inside** the Cardputer firmware. BMP becomes the debug server that talks to an emulated STM32F103 target, without physical hardware.

**Use case:** Test BMP's target scanning, attachment, and memory access workflows on the Cardputer without needing an external target board.

**Features:**
- ADIv5 SWD-DP + AHB-AP debug interface emulation
- Fake ROM table with Cortex-M3 components
- SCS debug registers (DHCSR, CPUID, DWT, FPB, ITM)
- FPEC flash controller with unlock/erase/program
- 128 KiB flash + 20 KiB SRAM memory model

**Usage:**
```bash
idf.py build && idf.py flash
# Connect via USB
arm-none-eabi-gdb
(gdb) target extended-remote /dev/ttyUSB0
(gdb) monitor emulate
(gdb) x/4xw 0x08000000
```

**Limitation:** CPU always halted; code execution not modeled.

---

## Quick Comparison

| Aspect | Host Python | Firmware |
|--------|-------------|----------|
| **Runs on** | Your PC | Cardputer (ESP32-S3) |
| **Tests** | STM32F103 firmware execution | BMP debug transport + target scanning |
| **CPU emulation** | Yes (Unicorn) | No (always halted) |
| **Memory access** | Via CPU execution | Via ADIv5 SWD-DP + AHB-AP |
| **GDB server** | Custom RSP | BMP's built-in RSP |
| **Peripherals** | RCC, SysTick | Debug (SCS, FPEC) only |
| **Build** | Python + Unicorn | ESP-IDF + GCC cross |

---

## Files

### Host Emulator
- `stm32emu/stm32emu.py` — Main Unicorn engine + RSP server
- `stm32emu/README.md` — Detailed usage
- `stm32emu/examples/rcc_hang_fw.py` — Example: firmware that hangs in RCC poll loop

### Firmware Emulator
- `emu_target.c/h` — Core ADIv5 target model
- `emu_shim.c/h` — BMP integration layer
- `test_emu.c` — Standalone host test (compile with gcc)
- `README_FIRMWARE.md` — Cardputer usage guide
- `INTEGRATION.md` — Architecture and troubleshooting

---

## When to Use Each

**Host Python Emulator:**
- You're developing STM32F103 firmware and want to test it in GDB before flashing
- You need CPU execution simulation (breakpoints, stepping, register changes)
- You don't have physical hardware yet

**Firmware Emulator:**
- You're testing BMP's target scanning and memory access on Cardputer
- You want to verify the Cardputer can act as a debugger without an external target
- You need to validate the ADIv5 SWD-DP layer in BMP

---

## Standalone Test

Test the core emulation model without BMP or firmware:

```bash
gcc -Wall -Wextra test_emu.c emu_target.c -o test_emu
./test_emu
```

Should pass 14 tests (DP/AP register layout, ROM table, posted reads, FPEC W1C semantics).
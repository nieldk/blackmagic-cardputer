# Emulated STM32F103 Target for Black Magic Probe

This directory contains an in-firmware emulated ADIv5 target (STM32F103 medium density) for the M5Stack Cardputer Black Magic Probe port. It allows offline testing of debug workflows without hardware.

## Files

- **emu_target.c / emu_target.h** — Core emulation model (API-neutral, host-testable).
  - Emulates STM32F103xB (medium density: 128 KiB flash, 20 KiB RAM).
  - Models ADIv5 DP (0x1ba01477, power-up ack mirroring), AHB-AP0, ROM table, SCS debug regs, FPEC flash controller.
  - Memory read/write with posted-read semantics for AP_DRW.
  - All state is local variables (no PSRAM needed on Cardputer).

- **emu_shim.c / emu_shim.h** — BMP integration layer.
  - Implements the four ADIv5 DP primitives (`dp_read`, `low_access`, `error`, `abort`).
  - Routes transactions to `emu_target` model.
  - Provides `emu_scan()` entry point (callable from monitor command).

- **test_emu.c** — Standalone host test (compiles with gcc, no ARM toolchain).
  - Links against emu_target.c directly, bypassing the shim.
  - Validates core model (register layout, ROM constants, posted reads, FPEC W1C).
  - Run: `gcc -Wall -Wextra test_emu.c emu_target.c -o test_emu && ./test_emu`.

## Integration Steps

### 1. Copy Files into BMP Source

```bash
cp emu_target.c emu_target.h emu_shim.c emu_shim.h /path/to/blackmagic/src/target/
```

### 2. Add Command to command.c

At the top of `command.c` (after existing includes), add:

```c
#include "emu_shim.h"
```

Add this function definition (after the other `cmd_*` functions):

```c
static bool cmd_emulate(target_s *t, int argc, const char **argv)
{
	(void)t;
	(void)argc;
	(void)argv;

	if (!emu_scan()) {
		gdb_out("Failed to set up emulated target\n");
		return false;
	}

	/* List the newly probed targets. */
	cmd_targets(NULL, 0, NULL);
	return true;
}
```

Add to the `cmd_list[]` table (before the `{NULL, NULL, NULL}` sentinel):

```c
{"emulate", cmd_emulate, "Set up emulated STM32F103 target (no hardware)"},
```

### 3. Update BMP Build System

#### For CMake (src/CMakeLists.txt or similar)

Ensure the new .c files are included in the target source list:

```cmake
add_executable(blackmagic
  # ... existing sources ...
  target/emu_target.c
  target/emu_shim.c
  # ... more sources ...
)
```

#### For Make (if using Make)

Add to the STM32 object list or equivalent:

```make
TARGETS := ... emu_target.o emu_shim.o ...
```

### 4. Rebuild and Test

```bash
cd /path/to/blackmagic
mkdir build && cd build
cmake ..
make -j4
```

Flash to Cardputer, connect GDB, and run:

```
(gdb) monitor emulate
(gdb) info target
```

You should see the emulated STM32F103 medium density target (128 KiB flash, 20 KiB RAM).

## Verification Checklist

- [ ] `emu_target.c/.h` compile without warnings
- [ ] `emu_shim.c/.h` compile without warnings
- [ ] BMP firmware compiles and flashes
- [ ] `monitor emulate` detects fake target
- [ ] GDB can `x 0x08000000` (read flash)
- [ ] GDB can halt/step/inspect registers
- [ ] BMP halt loop converges (posts initial read, corrects on second iteration)

## Known Limitations

1. **No code execution** — CPU is always halted; instruction execution is not modeled.
2. **Synchronous transactions** — BMP's timeout handling assumes real hardware. Emulated access is instant.
3. **No breakpoints** — FPB/DWT/ITM are enumerated but not functional; hardware breakpoints don't trap.
4. **No peripheral simulation** — Only debug (DHCSR, CPUID, SCS, FPEC) is modeled; RCC, GPIO, etc. read zero.
5. **Posted reads only** — AP_DRW reads are posted (return previous, post current); other AP reads are immediate.

## Corrected Constants (from Source Analysis)

- **ROM table**: Part 0x4c3 (Cortex-M3 ROM), Designer ARM (0x43b)
- **SCS**: Part 0x000, Class 0xe (Generic IP Component, cidc_gipc)
  - *Previous error*: was class 0x9 (debug component), triggered DEVTYPE/DEVARCH reads and cidc mismatch warning
- **AHB-AP0**: IDR 0x14770011 (TYPE 1 = AHB3, CLASS 8), BASE 0xe00ff003, CSW default 0x23000042
- **FPEC**: KEYR writes set PG/PER/MER/STRT in CR; SR EOP (bit5) is write-1-clear; DBGMCU@0xe0042000 = 0x20036410 (device ID 0x410 = medium density)

## Design Notes

### Posted-Read Semantics

The ADIv5 MEM-AP uses posted reads for efficiency: a read request returns the previous result while posting the new request. BMP's `advi5_mem_read_bytes` implements this:

```c
adiv5_dp_low_access(READ, ADIV5_AP_DRW, 0);   // Primer (result ignored)
while (--len) {
    value = adiv5_dp_low_access(READ, ADIV5_AP_DRW, 0);  // Posted reads
    // Process value
}
value = adiv5_dp_low_access(READ, ADIV5_DP_RDBUFF, 0);   // Final via RDBUFF
```

Our shim tracks `g_posted` and performs the bank bit extraction via SELECT writes, matching BMP's `firmware_ap_read` flow exactly.

### FPEC Unlock Sequence

`stm32f1_probe` reads DBGMCU_IDCODE@0xe0042000 to detect the device. Our model returns 0x20036410 (device ID 0x410 = medium density STM32F103). Probe hardcodes 128 KiB flash / 20 KiB RAM for this ID.

Flash programming follows the sequence: KEY1 (0x45670123) + KEY2 (0xcdef89ab) → LOCK cleared → PG | STRT → poll SR for EOP/BSY, write word, repeat. Our model simulates instant completion (no timing) and W1C on SR EOP/error bits.

### Target List Coordination

`adiv5_dp_init` manages the DP refcount and registers probed targets in the global `target_list`. On success, `emu_scan` returns true and the target is available to GDB. BMP's `cmd_targets` iterates `target_list`, so the emulated target appears alongside any real targets.

## Troubleshooting

**"Failed to set up emulated target"**
- `emu_scan()` returned false (no target added to list).
- Check firmware build includes `emu_shim.c` and `emu_target.c`.
- Verify `emu_scan()` is being called (set breakpoint if possible).

**CIDR class mismatch warning**
- Occurs if SCS CIDR class is 0x9 instead of 0xe; BMP expects 0xe for part 0x000.
- Verify CS_CID in emu_target.c line ~33 is `0xb105e00du` (not `0xb105900du`).

**Probe detects wrong flash size**
- Check DBGMCU_IDCODE read returns 0x20036410 (device 0x410).
- `stm32f1_probe` hardcodes flash layout per device ID; wrong ID → wrong size.

**Halting loops forever**
- Initial halt read is posted; halts on second iteration if DHCSR_S_HALT is set.
- Ensure our SCS DHCSR always returns DHCSR_S_HALT | DHCSR_S_REGRDY (0xc0020000).

## References

- **ADIv5 Specification**: Chapters 5–7 (DP, AHB-AP, ROM table)
- **ARM CoreSight Architecture Spec v3.0**: Sections B2.3 (component ID register layout)
- **STM32F103 Reference Manual**: Sections 31.2–31.3 (debug, FPEC)
- **BMP adiv5.c**: `arm_component_lut`, `adiv5_component_probe`, `adiv5_dp_init`

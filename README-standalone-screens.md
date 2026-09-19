### On-screen examples

Rendered mockups of the standalone verbs on the Cardputer console (240×135
ST7789, font6x9). Generated from the firmware's actual output strings; register
and memory values shown are the emulated STM32F1's seed state.

| | |
|---|---|
| **attach / detach** — attach to the emulated STM32F1, then detach<br><img src="docs/standalone-attach.png" width="360"> | **flash** — program a two-segment ELF, erase → write → verify<br><img src="docs/standalone-flash.png" width="360"> |
| **regs** — core register dump (sp, pc, xPSR of the halted-at-reset seed)<br><img src="docs/standalone-regs.png" width="360"> | **mem** — hex-dump the vector table: SP, reset vector, erased flash<br><img src="docs/standalone-mem.png" width="360"> |
| **read** — whole-flash dump to `/sdcard/dump.bin` with progress<br><img src="docs/standalone-read.png" width="360"> | **halt / poll / step / run / reset** — run control<br><img src="docs/standalone-runctl.png" width="360"> |
| **usbmode** — query, then switch to CDC+MSC (reboots)<br><img src="docs/standalone-usbmode.png" width="360"> | **help** — curated standalone list, scrolled back with `Fn+;`<br><img src="docs/standalone-help.png" width="360"> |
| **serialflash** — UART-flash an ESP SoC over Grove: connect, program, MD5-verify<br><img src="docs/standalone-serialflash.png" width="360"> | |

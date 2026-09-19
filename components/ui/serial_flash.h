#ifndef UI_SERIAL_FLASH_H
#define UI_SERIAL_FLASH_H
#include <stdbool.h>
/* serialflash <file> [hexoffset] — flash an ESP-SoC target over the Grove UART. */
bool serial_flash_cmd(int argc, char **argv);
#endif

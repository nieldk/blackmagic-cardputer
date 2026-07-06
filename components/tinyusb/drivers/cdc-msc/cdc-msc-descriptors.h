#pragma once
#include <tusb.h>

extern tusb_desc_device_t const cdcmsc_desc_device;
extern uint8_t const cdcmsc_desc_fs_configuration[];
#if TUD_OPT_HIGH_SPEED
extern uint8_t const cdcmsc_desc_hs_configuration[];
#endif

void cdcmsc_set_serial_number(const char *serial_number);
uint16_t const *cdcmsc_descriptor_string_cb(uint8_t index, uint16_t langid);

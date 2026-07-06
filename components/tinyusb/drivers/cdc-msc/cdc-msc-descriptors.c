/*
 * CDC + MSC composite descriptors.
 *
 * A single CDC (used for GDB) plus a Mass Storage interface. This fits the
 * ESP32-S3 USB-OTG endpoint budget where 2xCDC + MSC does not (that config
 * fails SET_CONFIGURATION with -EPIPE because it runs out of IN endpoints).
 * Selected at boot as USBDeviceTypeCdcMsc so files can be dragged onto the
 * internal storage over USB; switch back to dual-CDC for the UART mirror.
 */
#include <tusb.h>
#include <string.h>
#include <stdlib.h>
#include "cdc-msc-descriptors.h"

tusb_desc_device_t const cdcmsc_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,

    // IAD for the CDC function
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = 0x303A,  // Espressif
    .idProduct = 0x4002, // distinct PID so hosts don't reuse the 4001 composite binding
    .bcdDevice = 0x0100,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,

    .bNumConfigurations = 0x01,
};

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_MSC, ITF_NUM_TOTAL };

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

// CDC on EP1 (notif) / EP2 (data); MSC on EP3. 3 IN endpoints total - fits.
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_DATA  0x02
#define EPNUM_MSC_OUT   0x03
#define EPNUM_MSC_IN    0x83

uint8_t const cdcmsc_desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // CDC (GDB): itf, string, notif EP, notif size, data out, data in, data size
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_DATA, 0x80 | EPNUM_CDC_DATA, 64),

    // MSC: itf, string, EP out, EP in, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

#if TUD_OPT_HIGH_SPEED
uint8_t const cdcmsc_desc_hs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_DATA, 0x80 | EPNUM_CDC_DATA, 512),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 512),
};
#endif

static char *cdcmsc_string_desc[] = {
    (char[]){0x09, 0x04},       // 0: English (0x0409)
    "Niel Nielsen",             // 1: Manufacturer
    "Blackmagic CardPuter MSC", // 2: Product
    "blackmagic",               // 3: Serial (overwritten)
    "Blackmagic GDB",           // 4: CDC Interface
    "BMP Storage",              // 5: MSC Interface
};

void cdcmsc_set_serial_number(const char *serial_number) {
    cdcmsc_string_desc[3] = malloc(strlen("blackmagic_") + strlen(serial_number) + 1);
    strcpy(cdcmsc_string_desc[3], "blackmagic_");
    strcat(cdcmsc_string_desc[3], serial_number);
}

#define MAX_DESC_BUF_SIZE 40
static uint16_t _desc_str[MAX_DESC_BUF_SIZE];

uint16_t const *cdcmsc_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;
    if(index == 0) {
        memcpy(&_desc_str[1], cdcmsc_string_desc[0], 2);
        chr_count = 1;
    } else {
        if(index >= sizeof(cdcmsc_string_desc) / sizeof(cdcmsc_string_desc[0]))
            return NULL;
        const char *str = cdcmsc_string_desc[index];
        chr_count = strlen(str);
        if(chr_count > MAX_DESC_BUF_SIZE - 1)
            chr_count = MAX_DESC_BUF_SIZE - 1;
        for(uint8_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = str[i];
    }
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);
    return _desc_str;
}

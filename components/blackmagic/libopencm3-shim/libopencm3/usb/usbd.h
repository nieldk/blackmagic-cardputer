/*
 * Minimal libopencm3 shim for the ESP32 build of Black Magic.
 *
 * The ESP32 port uses TinyUSB, not libopencm3's USB stack, but a few common
 * BMP headers (usb.h, usb_serial.h, traceswo.h) name the `usbd_device` type in
 * prototypes that are never actually called on this platform. Only the type
 * name needs to exist for those headers to parse, so an opaque forward
 * declaration is sufficient. This lets the tree build with no libopencm3
 * dependency at all.
 */
#ifndef LIBOPENCM3_USB_USBD_H_SHIM
#define LIBOPENCM3_USB_USBD_H_SHIM

typedef struct _usbd_device usbd_device;

#endif /* LIBOPENCM3_USB_USBD_H_SHIM */

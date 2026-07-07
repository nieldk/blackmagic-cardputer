/*
 * Minimal libopencm3 shim for the ESP32 build of Black Magic.
 *
 * platforms/common/usb_types.h typedefs a set of libopencm3 USB descriptor
 * structs and two enums. On the ESP32 port these types are only forward-
 * referenced (never instantiated or dereferenced), so incomplete struct
 * declarations plus placeholder enum definitions are enough to compile,
 * with no real libopencm3 present.
 */
#ifndef LIBOPENCM3_USB_DFU_H_SHIM
#define LIBOPENCM3_USB_DFU_H_SHIM

/* Forward declarations (incomplete types are fine for the typedefs). */
struct usb_device_descriptor;
struct usb_config_descriptor;
struct usb_interface_descriptor;
struct usb_endpoint_descriptor;
struct usb_iface_assoc_descriptor;
struct usb_interface;
struct usb_setup_data;
struct usb_cdc_header_descriptor;
struct usb_cdc_call_management_descriptor;
struct usb_cdc_acm_descriptor;
struct usb_cdc_union_descriptor;
struct usb_cdc_line_coding;
struct usb_cdc_notification;
struct usb_dfu_descriptor;

/* Enums must be complete to be typedef'd, so give them placeholder members. */
enum usbd_request_return_codes { USBD_REQ_SHIM_PLACEHOLDER = 0 };
enum dfu_state { DFU_STATE_SHIM_PLACEHOLDER = 0 };

#endif /* LIBOPENCM3_USB_DFU_H_SHIM */

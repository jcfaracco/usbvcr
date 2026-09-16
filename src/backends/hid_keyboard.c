#include <stdlib.h>
#include <string.h>

#include "../core/device.h"
#include "../core/protocol.h"
#include "../lib/log.h"

/* HID boot-protocol keyboard. Endpoint 0x81 (interrupt IN), 8-byte reports. */

#define KBD_VID 0x04d9
#define KBD_PID 0x1702
#define KBD_EP_IN 0x81

/* ---- Descriptors ---- */

static const uint8_t device_desc[18] = {
    0x12, USB_DESC_DEVICE,
    0x00, 0x02,             /* bcdUSB 2.00 */
    0x00, 0x00, 0x00,       /* class/subclass/protocol (defined at interface) */
    0x08,                   /* bMaxPacketSize0 */
    KBD_VID & 0xff, KBD_VID >> 8,
    KBD_PID & 0xff, KBD_PID >> 8,
    0x00, 0x01,             /* bcdDevice 1.00 */
    0x01, 0x02, 0x00,       /* iManufacturer, iProduct, iSerial */
    0x01,                   /* bNumConfigurations */
};

/* HID report descriptor: standard 6-key boot keyboard (63 bytes). */
static const uint8_t report_desc[63] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
    0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

/* Config (9) + Interface (9) + HID (9) + Endpoint (7) = 34 bytes. */
static const uint8_t config_desc[34] = {
    /* Configuration */
    0x09, USB_DESC_CONFIG, 34, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32,
    /* Interface: class 3 (HID), subclass 1 (boot), protocol 1 (keyboard) */
    0x09, USB_DESC_INTERFACE, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    /* HID: bcdHID 1.11, 1 report descriptor of 63 bytes */
    0x09, USB_DESC_HID, 0x11, 0x01, 0x00, 0x01, USB_DESC_HID_REPORT, 63, 0x00,
    /* Endpoint 0x81, interrupt IN, 8-byte max packet, 10ms interval */
    0x07, USB_DESC_ENDPOINT, KBD_EP_IN, 0x03, 0x08, 0x00, 0x0A,
};

static const char *strings[] = { "Custom Vendor", "USB keyboard" };

/* ---- Backend state ---- */

typedef struct {
    int report_step;        /* drives the demo key sequence */
    uint8_t strbuf[128];    /* scratch for the last string descriptor */
} kbd_state_t;

/* ---- ops ---- */

static int kbd_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                              uint16_t lang, const uint8_t **out, uint16_t *len)
{
    (void)lang;
    kbd_state_t *st = dev->state;

    switch (type) {
    case USB_DESC_DEVICE:
        *out = device_desc; *len = sizeof(device_desc); return 0;
    case USB_DESC_CONFIG:
        *out = config_desc; *len = sizeof(config_desc); return 0;
    case USB_DESC_HID_REPORT:
        *out = report_desc; *len = sizeof(report_desc); return 0;
    case USB_DESC_STRING:
        if (index == 0) {                       /* LANGID: US English */
            st->strbuf[0] = 4; st->strbuf[1] = USB_DESC_STRING;
            st->strbuf[2] = 0x09; st->strbuf[3] = 0x04;
            *out = st->strbuf; *len = 4; return 0;
        }
        if (index <= sizeof(strings) / sizeof(strings[0])) {
            const char *s = strings[index - 1];
            size_t n = strlen(s);
            st->strbuf[0] = (uint8_t)(2 + 2 * n);
            st->strbuf[1] = USB_DESC_STRING;
            for (size_t i = 0; i < n; i++) {    /* ASCII -> UTF-16LE */
                st->strbuf[2 + 2 * i] = (uint8_t)s[i];
                st->strbuf[3 + 2 * i] = 0x00;
            }
            *out = st->strbuf; *len = st->strbuf[0]; return 0;
        }
        return -1;
    default:
        return -1;
    }
}

static int kbd_control(usb_device_t *dev,
                       const usb_control_packet_header_t *setup,
                       uint8_t *data, uint16_t *len)
{
    (void)dev; (void)data;
    /* ACK any HID class request (SET_IDLE, SET_PROTOCOL, ...) with no data. */
    if ((setup->requesttype & USB_TYPE_MASK) == USB_TYPE_CLASS) {
        *len = 0;
        return 0;
    }
    return -1;
}

static int kbd_ep_in(usb_device_t *dev, uint8_t ep, uint8_t *buf, size_t cap)
{
    kbd_state_t *st = dev->state;
    if (ep != KBD_EP_IN || cap < 8)
        return 0;

    /* Demo: press 'a' (usage 0x04) once, then release, then idle. */
    memset(buf, 0, 8);
    if (st->report_step == 0) {
        buf[2] = 0x04;          /* keycode 'a' */
        st->report_step = 1;
        return 8;
    }
    if (st->report_step == 1) {
        st->report_step = 2;    /* all-zero release report */
        return 8;
    }
    return 0;                    /* nothing more to send */
}

static const usb_device_ops kbd_ops = {
    .name           = "hid-keyboard",
    .get_descriptor = kbd_get_descriptor,
    .control        = kbd_control,
    .ep_in          = kbd_ep_in,
    .ep_out         = NULL,
    .tick           = NULL,
};

usb_device_t *hid_keyboard_create(void)
{
    usb_device_t *dev = calloc(1, sizeof(*dev));
    kbd_state_t *st = calloc(1, sizeof(*st));
    if (!dev || !st) {
        free(dev); free(st);
        LOG_ERR("hid_keyboard_create: out of memory");
        return NULL;
    }
    dev->ops = &kbd_ops;
    dev->state = st;
    return dev;
}

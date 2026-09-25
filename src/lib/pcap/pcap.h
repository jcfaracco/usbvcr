#ifndef PCAP_H
#define PCAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * pcap.h – Read/write module for USB captures in the usbmon (Linux USB
 * Monitor) format, carried in either classic pcap or pcapng.
 *
 * This module is deliberately orthogonal to its consumers:
 *   - usb_core can WRITE a capture while a session runs (record real traffic).
 *   - pcap_backend can READ a capture and replay it as a live device.
 * Both share the packet model and framing code below, but neither depends on
 * the other. Keep it free of engine/device types.
 *
 * usbmon link-layer types:
 *   LINKTYPE_USB_LINUX          189  – legacy 48-byte header
 *   LINKTYPE_USB_LINUX_MMAPPED  220  – 64-byte header (current usbmon)
 */

/* Transfer direction, from the host's point of view. */
typedef enum {
    PCAP_DIR_OUT = 0,   /* host -> device */
    PCAP_DIR_IN  = 1,   /* device -> host */
} pcap_dir_t;

/* usbmon transfer type (matches URB_* / usbmon xfer_type). */
typedef enum {
    PCAP_XFER_ISO     = 0,
    PCAP_XFER_INT     = 1,
    PCAP_XFER_CONTROL = 2,
    PCAP_XFER_BULK    = 3,
} pcap_xfer_t;

/* One decoded usbmon transfer. On read, `data` points into trace-owned
 * storage valid until pcap_trace_free(); on write, the caller owns `data`.
 * For control transfers `setup` holds the 8-byte SETUP packet when
 * setup_valid == 1. */
typedef struct {
    uint64_t    ts_usec;      /* capture timestamp, microseconds */
    uint8_t     bus;
    uint8_t     device;
    uint8_t     endpoint;     /* full bEndpointAddress incl. direction bit */
    pcap_dir_t  dir;
    pcap_xfer_t xfer;
    int         setup_valid;
    uint8_t     setup[8];
    const uint8_t *data;
    size_t      data_len;
} pcap_packet_t;

/* ---- Reader (replay side, e.g. pcap_backend) --------------------------- */

typedef struct pcap_trace pcap_trace_t;

/* Load a pcap/pcapng usbmon capture. Returns NULL on error. */
pcap_trace_t *pcap_trace_load(const char *path);

/* Number of decoded transfers. */
size_t pcap_trace_count(const pcap_trace_t *t);

/* Borrow transfer `i` (0-based). Returns NULL if out of range. */
const pcap_packet_t *pcap_trace_get(const pcap_trace_t *t, size_t i);

void pcap_trace_free(pcap_trace_t *t);

/* ---- Writer (record side, e.g. usb_core) ------------------------------- */

typedef struct pcap_writer pcap_writer_t;

/* Open `path` for writing a usbmon capture (pcapng). Returns NULL on error. */
pcap_writer_t *pcap_writer_open(const char *path);

/* Append one transfer. Returns 0, or -1 on error. */
int pcap_writer_add(pcap_writer_t *w, const pcap_packet_t *pkt);

void pcap_writer_close(pcap_writer_t *w);

#endif /* PCAP_H */

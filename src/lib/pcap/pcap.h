#ifndef PCAP_H
#define PCAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * pcap.h – Reader for USB captures taken from usbmon (Linux USB Monitor),
 * in either classic pcap or pcapng container format. STUB.
 *
 * usbmon frames carry a fixed usbmon header (bus/device/endpoint, setup
 * packet, status, transfer type/direction) followed by the transfer payload.
 * The relevant link-layer types are:
 *   LINKTYPE_USB_LINUX          189  – legacy 48-byte header
 *   LINKTYPE_USB_LINUX_MMAPPED  220  – 64-byte header (current usbmon)
 * The pcap/pcapng framing is stripped here; the backend consumes the decoded
 * per-transfer records.
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

/* One decoded usbmon transfer. `data`/`data_len` point into trace-owned
 * storage valid until pcap_trace_free(). For control transfers `setup` holds
 * the 8-byte SETUP packet (setup_valid == 1). */
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

typedef struct pcap_trace pcap_trace_t;

/* Load a pcap/pcapng usbmon capture. Returns NULL on error. */
pcap_trace_t *pcap_trace_load(const char *path);

/* Number of decoded transfers. */
size_t pcap_trace_count(const pcap_trace_t *t);

/* Borrow transfer `i` (0-based). Returns NULL if out of range. */
const pcap_packet_t *pcap_trace_get(const pcap_trace_t *t, size_t i);

void pcap_trace_free(pcap_trace_t *t);

#endif /* PCAP_H */

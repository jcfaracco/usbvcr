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

#define PCAP_MAGIC_NUMBER_MS 0xa1b2c3d4
#define PCAP_MAGIC_NUMBER_NS 0xa1b23c4d

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
    uint64_t    urb_id;       /* URB / transfer ID */
    uint8_t     bus;
    uint8_t     device;
    uint8_t     endpoint;     /* full bEndpointAddress incl. direction bit */
    pcap_dir_t  dir;
    pcap_xfer_t xfer;
    char        event_type;   /* 'S'ubmit, 'C'allback */
    int         setup_valid;
    uint8_t     setup[8];
    const uint8_t *data;
    size_t      data_len;
} pcap_packet_t;

/* PCAP global header */
typedef struct {
    uint32_t magic_number;   /* magic number (0xa1b2c3d4) */
    uint16_t version_major;  /* major version number (2) */
    uint16_t version_minor;  /* minor version number (4) */
    int32_t  thiszone;       /* GMT to local correction */
    uint32_t sigfigs;        /* accuracy of timestamps */
    uint32_t snaplen;        /* max length of captured packets */
    uint32_t network;        /* data link type (189 for DLT_USB_LINUX) */
} pcap_header_t;

/* PCAP record (packet) header */
typedef struct {
    uint32_t ts_sec;         /* timestamp seconds */
    uint32_t ts_usec;        /* timestamp microseconds */
    uint32_t incl_len;       /* number of octets of packet saved in file */
    uint32_t orig_len;       /* actual length of packet */
} pcap_rec_header_t;

/* --- PCAPNG Block Headers --- */
typedef struct {
    uint32_t block_type;    /* 0x0A0D0D0A Section Header Block */
    uint32_t block_len;     /* Total length of this block */
    uint32_t byte_order_magic; /* 0x1A2B3C4D */
    uint16_t major_version; /* 1 */
    uint16_t minor_version; /* 0 */
    int64_t  section_len;   /* -1 if unknown */
    /* Trailing block_len is handled during manual write */
} pcapng_shb_t;

typedef struct {
    uint32_t block_type;    /* 0x00000001 Interface Description Block */
    uint32_t block_len;     
    uint16_t link_type;     /* 189 for DLT_USB_LINUX */
    uint16_t reserved;      /* 0 */
    uint32_t snaplen;       /* 65535 */
    /* Trailing block_len handled during write */
} pcapng_idb_t;

typedef struct {
    uint32_t block_type;    /* 0x00000006 Enhanced Packet Block */
    uint32_t block_len;     
    uint32_t interface_id;  /* 0 */
    uint32_t ts_high;       /* High 32 bits of timestamp */
    uint32_t ts_low;        /* Low 32 bits of timestamp */
    uint32_t cap_packet_len;/* Length of data captured */
    uint32_t orig_packet_len;/* Actual length of data */
} pcapng_epb_t;

/* Output Engine Format Flag */
typedef enum {
    FORMAT_LEGACY_PCAP,
    FORMAT_PCAPNG
} pcap_format_t;

/* 
 * Linux usbmon binary header format (64 bytes).
 * Matches the layout expected by Wireshark for DLT_USB_LINUX.
 */
typedef struct {
    uint64_t id;             /* URB ID */
    unsigned char type;      /* 'S'ubmit, 'C'allback, 'E'rror */
    unsigned char xfer_type; /* 0: Isoch, 1: Interrupt, 2: Control, 3: Bulk */
    unsigned char epnum;     /* Endpoint number and direction */
    unsigned char devnum;    /* Device address */
    uint16_t busnum;         /* Bus number */
    char flag_setup;         /* '-' if setup block is valid, otherwise status */
    char flag_data;          /* '=' if data is present */
    int64_t ts_sec;          /* Kernel timestamp seconds */
    int32_t ts_usec;         /* Kernel timestamp microseconds */
    int32_t status;          /* URB status */
    uint32_t length;         /* Length of data (represented on wire) */
    uint32_t len_cap;        /* Length of data captured in this file */
    
    /* Union of Setup Block or Isochronous metadata */
    union {
        unsigned char setup[8]; 
        struct {
            int32_t error_count;
            int32_t numdesc;
        } iso;
    } s;
    int32_t interval;        /* Interval for Interrupt/Isochronous */
    int32_t start_frame;     /* Start frame for Isochronous */
    uint32_t xfer_flags;     /* Copy of URB transfer flags */
    uint32_t ndesc;          /* Number of actual Isochronous descriptors */
} usbmon_header_t;

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

/* Append one transfer. */
void pcap_writer_add(pcap_writer_t *w, const pcap_packet_t *pkt);

void pcap_writer_close(pcap_writer_t *w);

#endif /* PCAP_H */

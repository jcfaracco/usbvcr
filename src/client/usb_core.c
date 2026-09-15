#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include "usb_core.h"
#include "connection.h"
#include "log.h"

/* The hello payload: 64‑byte version string + 4‑byte capabilities */
#define HELLO_VERSION "usb_redir_hello 0.1"
#define HELLO_PAYLOAD_SIZE 68

static const char *packet_type_name(uint32_t type)
{
    switch (type) {
        case 0: return "hello";
        case 1: return "device_connect";
        case 2: return "device_disconnect";
        case 3: return "reset";
        case 4: return "interface_info";
        case 5: return "ep_info";
        case 6: return "set_configuration";
        case 7: return "get_configuration";
        case 8: return "configuration_status";
        case 9: return "set_alt_setting";
        case 10: return "get_alt_setting";
        case 11: return "alt_setting_status";
        case 12: return "start_iso_stream";
        case 13: return "stop_iso_stream";
        case 14: return "iso_stream_status";
        case 15: return "start_interrupt_receiving";
        case 16: return "stop_interrupt_receiving";
        case 17: return "interrupt_receiving_status";
        case 18: return "alloc_bulk_streams";
        case 19: return "free_bulk_streams";
        case 20: return "bulk_streams_status";
        case 21: return "cancel_data_packet";
        case 22: return "filter_reject";
        case 23: return "filter_filter";
        case 24: return "device_disconnect_ack";
        case 100: return "data_control_packet";
        case 101: return "data_bulk_packet";
        case 102: return "data_iso_packet";
        case 103: return "data_interrupt_packet";
        default: return "UNKNOWN";
    }
}


/* Receive and print one usbredir packet header + optional payload */
int receive_and_print_packet(int sock)
{
    uint8_t hdr_buf[12];
    if (recv_all(sock, hdr_buf, sizeof(hdr_buf)) <= 0)
        return -1;

    /* Parse little‑endian fields */
    uint32_t type = hdr_buf[0] | (hdr_buf[1] << 8) | (hdr_buf[2] << 16) | (hdr_buf[3] << 24);
    uint32_t len  = hdr_buf[4] | (hdr_buf[5] << 8) | (hdr_buf[6] << 16) | (hdr_buf[7] << 24);
    uint32_t id   = hdr_buf[8] | (hdr_buf[9] << 8) | (hdr_buf[10] << 16) | (hdr_buf[11] << 24);

    LOG_IN("Received packet: type=%s (%u), length=%u, id=0x%x",
           packet_type_name(type), type, len, id);

    char hex[12 * 3 + 1];
    int off = 0;
    for (int i = 0; i < 12; i++)
        off += snprintf(hex + off, sizeof(hex) - off, "%02x ", hdr_buf[i]);
    LOG_IN("    Raw header: %s", hex);

    /* Consume the payload (print first 64 bytes if it's a hello) */
    if (len > 0) {
        if (type == RT_HELLO && len >= 64) {
            uint8_t version[65];
            memset(version, 0, sizeof(version));
            if (recv_all(sock, version, 64) <= 0)
                return -1;
            LOG_INFO("    Server version: \"%s\"", version);

            /* Discard the rest of the hello payload (capabilities, etc.) */
            if (len > 64) {
                if (recv_all(sock, NULL, len - 64) < 0)
                    return -1;
            }
        } else {
            /* Discard unknown payload */
            if (recv_all(sock, NULL, len) < 0)
                return -1;
        }
    }

    return (int)type;
}


/* ------------------------------------------------------------------ *
 * USB‑redir hello exchange                                            *
 * ------------------------------------------------------------------ */

/* Build our hello packet into `out`. Returns total size. */
size_t build_hello_packet(uint8_t *out)
{
    uint8_t payload[HELLO_PAYLOAD_SIZE];
    memset(payload, 0, sizeof(payload));

    /* First 64 bytes: version string (zero‑padded) */
    strncpy((char *)payload, HELLO_VERSION, 64);

    /* Last 4 bytes: capabilities bitmap */
    payload[64] = 0xdf;
    payload[65] = 0x00;
    payload[66] = 0x00;
    payload[67] = 0x00;

    /* Prepend the 12‑byte header */
    usbredir_header hdr;
    hdr.type   = RT_HELLO;
    hdr.length = HELLO_PAYLOAD_SIZE;
    hdr.id     = 0;

    memcpy(out, &hdr, sizeof(hdr));
    memcpy(out + sizeof(hdr), payload, HELLO_PAYLOAD_SIZE);

    return sizeof(hdr) + HELLO_PAYLOAD_SIZE;
}

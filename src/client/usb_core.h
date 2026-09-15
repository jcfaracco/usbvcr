#ifndef USBREDIR_CLIENT_USB_CORE_H
#define USBREDIR_CLIENT_USB_CORE_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ *
 * USB‑redir protocol definitions (from the usbredir spec)            *
 * ------------------------------------------------------------------ */

/* Every usbredir packet starts with this 12‑byte header (little‑endian) */
#pragma pack(push, 1)
typedef struct {
    uint32_t type;
    uint32_t length;
    int32_t  id;
} usbredir_header;
#pragma pack(pop)

/* Packet type constants */
enum {
    RT_HELLO              = 0,
    RT_DEVICE_CONNECT     = 1,
    RT_DEVICE_DISCONNECT  = 2,
    RT_RESET              = 3,
    RT_INTERFACE_INFO     = 4,
    RT_EP_INFO            = 5,
};

/* Receive one usbredir packet (header + payload) from `sock` and print it.
 * Returns the packet type, or -1 on error. */
int receive_and_print_packet(int sock);

/* Build our hello packet into `out`. Returns total size in bytes. */
size_t build_hello_packet(uint8_t *out);

#endif

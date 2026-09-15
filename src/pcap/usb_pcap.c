#include "usb_pcap.h"

// TODO: Target Device IDs to filter for (Modify these)
#define TARGET_VENDOR_ID  0x1b1c
#define TARGET_PRODUCT_ID 0x0c1a

void packet_handler(u_char *user_data, const struct pcap_pkthdr *pkthdr, const u_char *packet) {
    if (pkthdr->caplen < sizeof(struct usbmon_packet_header)) return;

    struct usbmon_packet_header *usb_hdr = (struct usbmon_packet_header *)packet;
    const u_char *payload = packet + sizeof(struct usbmon_packet_header);
    int payload_len = pkthdr->caplen - sizeof(struct usbmon_packet_header);

    // 1. PEEK PHASE: Look for GET DESCRIPTOR Responses to map VID/PID to a device address
    // transfer_type == 2 (Control Transfer), type == 'C' (Callback / Response payload)
    if (usb_hdr->transfer_type == 2 && usb_hdr->type == 'C' && payload_len >= 18) {
        // Device Descriptor payload is exactly 18 bytes long
        // Byte 0 is length (0x12), Byte 1 is Descriptor Type (0x01 = Device Descriptor)
        if (payload[0] == 0x12 && payload[1] == 0x01) {
            uint16_t vendor_id = payload[8] | (payload[9] << 8);
            uint16_t product_id = payload[10] | (payload[11] << 8);

            if (vendor_id == TARGET_VENDOR_ID && product_id == TARGET_PRODUCT_ID) {
                target_bus_id = usb_hdr->bus_id;
                target_device_address = usb_hdr->device_address;
                printf("[MATCH] Found Target Device! Bus: %d, Addr: %d\n",
                       target_bus_id, target_device_address);
            }
        }
    }

    // 2. FILTER PHASE: Match packets originating from or going to our tracked device address
    if (target_bus_id != -1 && target_device_address != -1) {
        if (usb_hdr->bus_id == target_bus_id && usb_hdr->device_address == target_device_address) {

            // Successfully filtered packet! Let's print details:
            printf("Filtered Packet - Bus: %u Addr: %u | EP: 0x%02X | Type: %c | Len: %u bytes\n",
                   usb_hdr->bus_id,
                   usb_hdr->device_address,
                   usb_hdr->endpoint_number,
                   usb_hdr->type,
                   usb_hdr->data_length);

            // Your custom logic or packet dumping goes here!
        }
    }
}

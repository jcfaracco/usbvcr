#include "usb_pcap.h"

// TODO: State variables to map the matching device address dynamically
int target_bus_id = -1;
int target_device_address = -1;

void packet_handler(u_char *user_data, const struct pcap_pkthdr *pkthdr,
                    const u_char *packet) {
    (void)user_data;
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

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <capture.pcap>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t *handle = pcap_open_offline(argv[1], errbuf);
    if (!handle) {
        fprintf(stderr, "pcap_open_offline: %s\n", errbuf);
        return EXIT_FAILURE;
    }

    if (pcap_datalink(handle) != DLT_USB_LINUX_MMAPPED)
        fprintf(stderr, "[!] Warning: capture is not DLT_USB_LINUX_MMAPPED\n");

    pcap_loop(handle, 0, packet_handler, NULL);
    pcap_close(handle);
    return EXIT_SUCCESS;
}

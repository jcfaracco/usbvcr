import json
import yaml

def parse_pcap_json(json_file_path):
    with open(json_file_path, 'r') as f:
        packets = json.load(f)

    pending_urbs = {}
    rules = []
    
    # Default device baseline
    device_info = {
        "vid": "UNKNOWN",
        "pid": "UNKNOWN",
        "class": "0x00",
        "description": "Auto-generated Mock Device"
    }

    state_counter = 1

    for pkt in packets:
        layers = pkt.get("_source", {}).get("layers", {})
        usb = layers.get("usb", {})
        
        urb_type = usb.get("usb.urb_type")
        urb_id = usb.get("usb.urb_id")

        # 1. Handle Host-to-Device Requests (Submit)
        if urb_type == "'S'":
            setup_data = layers.get("Setup Data")
            if setup_data:
                # Wireshark parses fields differently based on the class (e.g., standard vs HID)
                bmRequestType = setup_data.get("usb.bmRequestType", "")
                bRequest = setup_data.get("usb.setup.bRequest", setup_data.get("usbhid.setup.bRequest", ""))
                
                # Reconstruct wValue if Wireshark split it into bDescriptorType and DescriptorIndex
                wValue = setup_data.get("usb.setup.wValue", setup_data.get("usbhid.setup.wValue", ""))
                if not wValue and "usb.bDescriptorType" in setup_data:
                    desc_type = int(setup_data["usb.bDescriptorType"], 16)
                    desc_idx = int(setup_data.get("usb.DescriptorIndex", "0"), 16)
                    wValue = f"0x{(desc_type << 8) | desc_idx:04x}"

                pending_urbs[urb_id] = {
                    "bmRequestType": bmRequestType,
                    "bRequest": bRequest,
                    "wValue": wValue
                }

        # 2. Handle Device-to-Host Responses (Complete)
        elif urb_type == "'C'":
            # Extract VID/PID if this completion contains a DEVICE DESCRIPTOR
            if "DEVICE DESCRIPTOR" in layers:
                device_info["vid"] = layers["DEVICE DESCRIPTOR"].get("usb.idVendor", device_info["vid"])
                device_info["pid"] = layers["DEVICE DESCRIPTOR"].get("usb.idProduct", device_info["pid"])

            # Map to the pending request
            if urb_id in pending_urbs:
                request_match = pending_urbs.pop(urb_id)
                
                # Look for data fragment (e.g., Set Report / Get Report payloads)
                payload = "00" # Default ACK placeholder
                if "Setup Data" in layers and "usb.data_fragment" in layers["Setup Data"]:
                    payload = layers["Setup Data"]["usb.data_fragment"]
                
                # Note: For full descriptor payloads, tshark must be run with `-x` to include raw hex
                
                rule = {
                    "name": f"State_{state_counter}_Auto",
                    "state": "ANY",
                    "match": {k: v for k, v in request_match.items() if v}, # Strip empty keys
                    "action": {
                        "type": "respond",
                        "data": payload
                    }
                }
                rules.append(rule)
                state_counter += 1

    # Compile the final DSL structure
    output = {
        "device": device_info,
        "rules": rules
    }

    # Dump to YAML without sorting keys to maintain readability
    return yaml.dump(output, sort_keys=False, indent=2)

if __name__ == "__main__":
    yaml_output = parse_pcap_json("../usb_traffic.json")
    print(yaml_output)

import json
import os
import yaml

def generate_emulation_project(json_file_path, output_dir="emulated_device"):
    with open(json_file_path, 'r') as f:
        pcap_data = json.load(f)

    # 1. Setup Directories
    dirs = ['schemas', 'devices', 'flows', 'payloads']
    for d in dirs:
        os.makedirs(os.path.join(output_dir, d), exist_ok=True)

    # Dictionaries to hold extracted data
    device_profile = {
        "version": "1.0",
        "name": "Extracted USB Device",
        "imports": ["schemas/usb_base.yaml"],
        "device_descriptor": {"template": "usb_base.device_descriptor"},
        "strings": {},
        "configurations": [{"template": "usb_base.configuration_descriptor", "interfaces": []}]
    }
    
    flow_logic = {
        "version": "1.0",
        "kind": "FlowLogic",
        "states": [{"name": "enumeration", "rules": []}]
    }

    # Track URBs to pair Submit ('S') with Complete ('C')
    pending_requests = {}

    for packet in pcap_data:
        layers = packet['_source']['layers']
        if 'usb' not in layers:
            continue

        usb = layers['usb']
        urb_id = usb.get('usb.urb_id')
        urb_type = usb.get('usb.urb_type', '').strip("'")

        # Handle Submit (Host -> Device Request)
        if urb_type == 'S' and 'Setup Data' in layers:
            setup = layers['Setup Data']
            pending_requests[urb_id] = {
                "bmRequestType": setup.get('usb.bmRequestType'),
                "bRequest": setup.get('usb.setup.bRequest'),
                "wValue": setup.get('usb.setup.wValue', '0x' + setup.get('usb.DescriptorIndex', '00') + setup.get('usb.bDescriptorType', '00')),
                "wIndex": setup.get('usb.setup.wIndex', '0')
            }

        # Handle Complete (Device -> Host Response)
        elif urb_type == 'C' and urb_id in pending_requests:
            req = pending_requests.pop(urb_id)
            
            # Create a basic flow rule for this interaction
            rule = {
                "match": {
                    "bmRequestType": int(req['bmRequestType'], 16),
                    "bRequest": int(req['bRequest']),
                    "wValue": int(req['wValue'], 16)
                },
                "actions": []
            }

            # Extract Device Descriptor
            if 'DEVICE DESCRIPTOR' in layers:
                desc = layers['DEVICE DESCRIPTOR']
                device_profile['device_descriptor']['idVendor'] = desc.get('usb.idVendor')
                device_profile['device_descriptor']['idProduct'] = desc.get('usb.idProduct')
                rule['actions'].append({"respond_with": "$device.device_descriptor"})

            # Extract Configuration Descriptor
            elif 'CONFIGURATION DESCRIPTOR' in layers:
                desc = layers['CONFIGURATION DESCRIPTOR']
                device_profile['configurations'][0]['bmAttributes'] = desc.get('usb.configuration.bmAttributes')
                rule['actions'].append({"respond_with": "$device.configurations[0]"})

            # Extract Strings
            elif 'STRING DESCRIPTOR' in layers:
                str_desc = layers['STRING DESCRIPTOR']
                if 'usb.bString' in str_desc:
                    str_val = str_desc['usb.bString']
                    # Use wValue lower byte as index
                    str_idx = int(req['wValue'], 16) & 0xFF
                    device_profile['strings'][str_idx] = str_val
                rule['actions'].append({"respond_with": f"$device.strings[{int(req['wValue'], 16) & 0xFF}]"})

            # Extract Raw HID Data / Opaque Payloads
            elif 'usbhid' in layers:
                # If it's a report descriptor, dump the raw hex to a payload file
                payload_filename = f"report_desc_{req['wValue']}.bin"
                payload_path = os.path.join(output_dir, 'payloads', payload_filename)
                
                # In a real scenario, you'd extract the raw hex string from the packet frame here.
                # For this script, we'll write a placeholder mapped to the flow.
                with open(payload_path, 'wb') as bin_f:
                    bin_f.write(b'\\x00') 

                rule['actions'].append({
                    "respond_raw": {"file": f"payloads/{payload_filename}"}
                })
            else:
                # Generic fallback for unknown completions
                rule['actions'].append({"respond_with_status": 0})

            # Prevent duplicate rules in the flow
            if rule not in flow_logic['states'][0]['rules']:
                flow_logic['states'][0]['rules'].append(rule)

    # 2. Write the YAML files
    device_profile['flows'] = [{"include": "flows/extracted_flow.yaml"}]

    with open(os.path.join(output_dir, 'devices', 'extracted_device.yaml'), 'w') as f:
        yaml.dump(device_profile, f, sort_keys=False)

    with open(os.path.join(output_dir, 'flows', 'extracted_flow.yaml'), 'w') as f:
        yaml.dump(flow_logic, f, sort_keys=False)

    print(f"Project successfully generated in ./{output_dir}/")

if __name__ == "__main__":
    # Point this to your Wireshark JSON export
    generate_emulation_project("usb_traffic.json")

import yaml
import struct
from scapy.all import PcapReader, conf, Raw
from scapy.layers.usb import USBpcap

# Forçar o motor do Scapy a aceitar o Link-Layer do utilitário Windows
conf.l2types.register(220, USBpcap)

TRANSFER_TYPES = {0: "Isochronous", 1: "Interrupt", 2: "Control", 3: "Bulk"}

def pcap_to_scope_yaml(pcap_path, yaml_path):
    print(f"Analisando lógica de {pcap_path} (Modo de Compatibilidade Estrita)...")
    
    submits = {}
    completed_pairs = []
    global_device_info = {}

    with PcapReader(pcap_path) as packets:
        for idx, packet in enumerate(packets):
            if not packet.haslayer(USBpcap):
                continue
            
            usb = packet[USBpcap]
            
            # Coleta segura dos metadados do cabeçalho USBpcap
            irp_id = hex(getattr(usb, "irpId", 0))
            info_field = int(getattr(usb, "info", 0))
            transfer_field = int(getattr(usb, "transfer", 2))
            endpoint_field = int(getattr(usb, "endpoint", 0))
            bus_field = int(getattr(usb, "bus", 0))
            device_field = int(getattr(usb, "device", 0))
            status_field = int(getattr(usb, "usbd_status", 0))

            if not global_device_info:
                global_device_info = {
                    "bus_id": bus_field,
                    "device_address": device_field,
                    "endpoint_base": hex(endpoint_field)
                }

            # info & 1 == 1 -> Resposta do Dispositivo (Complete)
            # info & 1 == 0 -> Requisição do Host (Submit)
            is_response = (info_field & 1) == 1

            # Extração dos dados brutos (Payload)
            raw_payload = b""
            if packet.haslayer(Raw):
                raw_payload = packet[Raw].load
            elif hasattr(usb, 'payload') and usb.payload:
                raw_payload = bytes(usb.payload)

            # 1. FLUXO DE REQUISIÇÃO (Host -> Device)
            if not is_response:
                match_criteria = {
                    "urb_type": "Submit",
                    "transfer_type": TRANSFER_TYPES.get(transfer_field, transfer_field),
                    "endpoint": hex(endpoint_field),
                }
                
                # Se for uma transferência de Controle, extraímos os 8 bytes de Setup 
                # diretamente do início dos dados brutos recebidos (onde o Setup reside no Windows)
                if transfer_field == 2 and len(raw_payload) >= 8:
                    bmRequestType, bRequest, wValue, wIndex, wLength = struct.unpack("<BBHHH", raw_payload[:8])
                    match_criteria["setup"] = {
                        "bmRequestType": hex(bmRequestType),
                        "bRequest": int(bRequest),
                        "wValue": hex(wValue),
                        "wIndex": int(wIndex),
                        "wLength": int(wLength),
                    }
                    # Oculta os 8 bytes do Setup do payload bruto para não duplicar dados
                    raw_payload = raw_payload[8:]
                
                submits[irp_id] = {
                    "description": f"Transação do Teclado Holtek (IRP ID: {irp_id})",
                    "request_match": match_criteria
                }

            # 2. FLUXO DE RESPOSTA (Device -> Host)
            elif is_response and irp_id in submits:
                block = submits.pop(irp_id)
                block["response_action"] = {
                    "urb_type": "Complete",
                    "status": status_field,
                    "payload_hex": raw_payload.hex()
                }
                completed_pairs.append(block)

    # Varre requisições pendentes que ficaram órfãs no final do pcap
    for irp_id, block in submits.items():
        block["response_action"] = {
            "urb_type": "Complete",
            "status": 0,
            "payload_hex": ""
        }
        completed_pairs.append(block)

    final_scope_structure = {
        "device_info": global_device_info,
        "negotiation_logic": completed_pairs
    }

    print(f"Salvando estrutura de escopo em {yaml_path}...")
    with open(yaml_path, "w") as f:
        yaml.dump(final_scope_structure, f, default_flow_style=False, sort_keys=False)
    print("Mapeamento concluído com sucesso!")

if __name__ == "__main__":
    pcap_to_scope_yaml("../holtek_keyboard.pcap", "holtek.yaml")


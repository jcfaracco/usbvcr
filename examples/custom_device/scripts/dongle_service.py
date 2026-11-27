#!/usr/bin/env python3
"""
Custom Proprietary USB Dongle Service Script.

This project-scoped script implements the proprietary protocol for the
vendor-specific USB device (bDeviceClass: 0xFF, VID: 0x1234, PID: 0x5678).
It communicates with usbvcr via standard I/O:
  - Bulk OUT (Host -> Device) packets are received on stdin
  - Bulk IN (Device -> Host) response packets are emitted to stdout
"""

import sys
import struct
import time

# Proprietary Opcode Definitions
CMD_PING        = 0x01
CMD_GET_VERSION = 0x02
CMD_READ_SENSOR = 0x03
CMD_SET_LED     = 0x04

RESP_ACK        = 0x00
RESP_ERROR      = 0xFF

LED_STATE = 0


def handle_packet(payload: bytes) -> bytes:
    """Process an incoming command payload and return a response packet."""
    global LED_STATE

    if len(payload) < 4:
        return struct.pack("<BBBB", RESP_ERROR, 0, 0, 0x01)

    cmd, seq, param1, param2 = struct.unpack("<BBBB", payload[:4])

    if cmd == CMD_PING:
        # PING -> PONG echo with sequence match
        return struct.pack("<BBBB", CMD_PING | 0x80, seq, RESP_ACK, 0x00)

    elif cmd == CMD_GET_VERSION:
        # Return version string payload
        ver_str = b"DONGLE_V2.1.0"
        hdr = struct.pack("<BBBB", CMD_GET_VERSION | 0x80, seq, RESP_ACK, len(ver_str))
        return hdr + ver_str

    elif cmd == CMD_READ_SENSOR:
        # Simulate sensor telemetry: temp=23.4 C, humidity=48%, 32-bit uptime
        temp_val = int(23.4 * 10) # 234
        humidity = 48
        uptime_sec = int(time.time()) & 0xFFFFFFFF
        telemetry = struct.pack("<HBI", temp_val, humidity, uptime_sec)
        hdr = struct.pack("<BBBB", CMD_READ_SENSOR | 0x80, seq, RESP_ACK, len(telemetry))
        return hdr + telemetry

    elif cmd == CMD_SET_LED:
        # Set simulated LED and return previous/new state
        prev = LED_STATE
        LED_STATE = param1
        return struct.pack("<BBBB", CMD_SET_LED | 0x80, seq, prev, LED_STATE)

    else:
        # Unhandled opcode
        return struct.pack("<BBBB", RESP_ERROR, seq, 0xFE, cmd)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--test":
        # Self-test mode
        ping = handle_packet(struct.pack("<BBBB", CMD_PING, 7, 0, 0))
        assert ping[0] == 0x81 and ping[1] == 7
        ver = handle_packet(struct.pack("<BBBB", CMD_GET_VERSION, 1, 0, 0))
        assert b"DONGLE_V2.1.0" in ver
        print("Self-test passed.")
        return 0

    try:
        while True:
            # Read incoming USB transfer (up to max packet size 64 bytes)
            packet = sys.stdin.buffer.read(64)
            if not packet:
                break

            response = handle_packet(packet)
            if response:
                sys.stdout.buffer.write(response)
                sys.stdout.buffer.flush()
    except (KeyboardInterrupt, BrokenPipeError):
        pass

    return 0


if __name__ == "__main__":
    sys.exit(main())

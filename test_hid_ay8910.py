#!/usr/bin/env python3
"""RPFM HID test — AY8910 simple tone"""

import hid
import struct
import sys

VID = 0x2E8A
PID = 0x1090

def crc8(data):
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x31) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc

CMD_WRITE_REG = 0x01

def make_frame(cmd, seq, payload):
    frame = bytearray(64)
    frame[0] = cmd
    frame[1] = seq
    frame[2] = len(payload)
    frame[3:3+len(payload)] = payload
    frame[63] = crc8(frame[:63])
    return bytes(frame)

def write_reg(dev, seq, slot, addr, data):
    """CMD_WRITE_REG: [slot, addr, data]"""
    payload = bytes([slot, addr, data])
    frame = make_frame(CMD_WRITE_REG, seq, payload)
    # HID report needs leading report ID byte (0x00)
    dev.write(b'\x00' + frame)
    # Read response
    resp = dev.read(64, timeout_ms=100)
    return resp

def main():
    print("Opening RPFM HID device...")
    try:
        dev = hid.device(VID, PID)
        dev.open(VID, PID)
    except OSError as e:
        print(f"Cannot open device: {e}")
        sys.exit(1)

    print(f"Manufacturer: {dev.get_manufacturer_string()}")
    print(f"Product: {dev.get_product_string()}")
    print()

    # AY8910 is on slot 0 (same as YM2413 CS0)
    # AY8910 registers:
    #   0x00-0x01: Channel A tone period (fine, coarse)
    #   0x06: Noise period
    #   0x07: Mixer (bit=1 disables: bits 0-2=tone, 3-5=noise)
    #   0x08: Channel A volume (bits 0-3=volume, bit 4=envelope)
    #   0x0B-0x0C: Envelope period
    #   0x0D: Envelope shape

    SLOT = 0
    seq = 0

    # AY8910 clock is typically 1.7897725 MHz (MSX) or 2 MHz
    # For ~440Hz (A4): period = clock / (16 * freq) = 1789772 / (16 * 440) ≈ 254
    # Middle C (261.63Hz): period = 1789772 / (16 * 261.63) ≈ 428

    tone_period = 254  # ~440 Hz

    print(f"Playing A4 (440Hz) on AY8910 slot {SLOT}...")

    # Set tone period for Channel A
    seq = (seq + 1) & 0xFF
    r = write_reg(dev, seq, SLOT, 0x00, tone_period & 0xFF)  # Fine
    print(f"  Reg 0x00 = {tone_period & 0xFF:#04x}, ACK={r[0] if r else 'N/A'}")

    seq = (seq + 1) & 0xFF
    r = write_reg(dev, seq, SLOT, 0x01, (tone_period >> 8) & 0x0F)  # Coarse
    print(f"  Reg 0x01 = {(tone_period >> 8) & 0x0F:#04x}, ACK={r[0] if r else 'N/A'}")

    # Mixer: enable tone on channel A only (bit 0 = 0)
    seq = (seq + 1) & 0xFF
    r = write_reg(dev, seq, SLOT, 0x07, 0x3E)  # 0b00111110 = tone A on, rest off
    print(f"  Reg 0x07 = 0x3E, ACK={r[0] if r else 'N/A'}")

    # Volume: channel A max volume
    seq = (seq + 1) & 0xFF
    r = write_reg(dev, seq, SLOT, 0x08, 0x0F)  # Max volume
    print(f"  Reg 0x08 = 0x0F, ACK={r[0] if r else 'N/A'}")

    print()
    print("Tone playing! Press Enter to stop...")
    input()

    # Mute
    seq = (seq + 1) & 0xFF
    write_reg(dev, seq, SLOT, 0x08, 0x00)  # Volume 0
    print("Muted.")

    dev.close()

if __name__ == "__main__":
    main()

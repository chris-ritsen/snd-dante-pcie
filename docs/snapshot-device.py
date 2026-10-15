#!/usr/bin/env python3
"""Snapshot Dante PCIe device state for clean room provenance verification.

Captures all observable hardware state from the running device:
- BAR0 registers and descriptor RAM (VDMA engine + FPGA)
- BAR4 registers (GN412x bridge)
- ALSA PCM state
- Interrupt counts
- Audio meter peaks

Output is a timestamped JSON file in docs/ that can be diffed between
the original driver and the replacement driver to verify identical behavior.
"""

import json
import os
import struct
import sys
import time
from datetime import datetime

def find_debugfs():
    import glob

    dirs = sorted(glob.glob("/sys/kernel/debug/dante-pcie[0-9]*"))
    if dirs:
        return dirs[0]
    return "/sys/kernel/debug/dante-pcie"


DEBUGFS = find_debugfs()
OUTPUT_DIR = os.path.dirname(os.path.abspath(__file__))

BAR0_REGISTERS = {
    0x0004: "UNKNOWN_04",
    0x0010: "VDMA_EVENT",
    0x0014: "VDMA_EVENT_EN",
    0x0018: "VDMA_SYS_ADDR_LO",
    0x001c: "VDMA_SYS_ADDR_HI",
    0x0020: "VDMA_DPTR",
    0x0024: "VDMA_XFER_CTL",
    0x0028: "VDMA_RA",
    0x002c: "VDMA_RB",
    0x0030: "VDMA_CSR",
    0x0034: "UNKNOWN_34",
    0x003c: "UNKNOWN_3C",
    0x0070: "FIRMWARE_VERSION",
    0x0074: "SAMPLE_COUNT_LO",
    0x0078: "SAMPLE_COUNT_HI",
}

DMAP_WORDS = {
    0x00: "CONSTANT_ZERO",
    0x01: "CONSTANT_ONE",
    0x02: "CONSTANT_NEG_ONE",
    0x03: "XFER_CTL_OUTBOUND",
    0x04: "XFER_CTL_INBOUND",
    0x05: "CHUNK_SIZE",
    0x06: "OUTBOUND_CHANNELS",
    0x07: "INBOUND_CHANNELS",
    0x08: "PERIODS_PER_IRQ",
    0x09: "PERIOD_COUNTDOWN",
    0x10: "OUTBOUND_CHUNKS",
    0x11: "INBOUND_CHUNKS",
    0x12: "OUTBOUND_CHANNEL_STRIDE",
    0x13: "INBOUND_CHANNEL_STRIDE",
    0x14: "OUTBOUND_BUF_ADDR_LO",
    0x15: "OUTBOUND_BUF_ADDR_HI",
    0x16: "INBOUND_BUF_ADDR_LO",
    0x17: "INBOUND_BUF_ADDR_HI",
    0x18: "OUTBOUND_CUR_CHUNK",
    0x19: "INBOUND_CUR_CHUNK",
    0x1a: "OUTBOUND_BASE_ADDR_LO",
    0x1b: "OUTBOUND_BASE_ADDR_HI",
    0x1c: "INBOUND_BASE_ADDR_LO",
    0x1d: "INBOUND_BASE_ADDR_HI",
    0x1e: "OUTBOUND_WORK_ADDR_LO",
    0x1f: "OUTBOUND_WORK_ADDR_HI",
    0x20: "INBOUND_WORK_ADDR_LO",
    0x21: "INBOUND_WORK_ADDR_HI",
}

BAR4_REGISTERS = {
    0x800: "PCI_SYS_CFG_SYSTEM",
    0x804: "LB_CTL",
    0x808: "CLK_CSR",
    0x80c: "PCI_BAR_CONFIG",
    0x810: "INT_CTRL",
    0x814: "INT_STAT",
    0x818: "PEX_ERROR_STAT",
    0x820: "INT_CFG0",
    0x824: "INT_CFG1",
    0x828: "INT_CFG2",
    0x82c: "INT_CFG3",
    0x830: "INT_CFG4",
    0x834: "INT_CFG5",
    0x838: "INT_CFG6",
    0x83c: "INT_CFG7",
    0xa00: "GPIO_BYPASS_MODE",
    0xa04: "GPIO_DIRECTION_MODE",
    0xa08: "GPIO_OUTPUT_ENABLE",
    0xa0c: "GPIO_OUTPUT_VALUE",
    0xa10: "GPIO_INPUT_VALUE",
    0xa14: "GPIO_INT_MASK",
    0xa1c: "GPIO_INT_MASK_SET",
    0xa20: "GPIO_INT_STATUS",
    0xa24: "GPIO_INT_TYPE",
    0xa28: "GPIO_INT_VALUE",
    0xa2c: "GPIO_INT_ON_ANY",
}


def read_register(fd, offset):
    os.lseek(fd, offset, os.SEEK_SET)
    data = os.read(fd, 4)
    if len(data) < 4:
        return None
    return struct.unpack("<I", data)[0]


def read_bar_registers(bar_path, register_map):
    results = {}
    try:
        fd = os.open(bar_path, os.O_RDONLY)
        for offset, name in sorted(register_map.items()):
            value = read_register(fd, offset)
            results[name] = {"offset": f"0x{offset:04x}", "value": f"0x{value:08x}" if value is not None else "READ_ERROR"}
        os.close(fd)
    except OSError as exception:
        results["_error"] = str(exception)
    return results


def read_descriptor_ram(bar0_path):
    primary = {}
    shadow = {}
    try:
        fd = os.open(bar0_path, os.O_RDONLY)
        for word_index in range(0x30):
            primary_value = read_register(fd, 0x4000 + word_index * 4)
            shadow_value = read_register(fd, 0x6000 + word_index * 4)
            name = DMAP_WORDS.get(word_index, f"WORD_{word_index:02x}")
            primary[name] = f"0x{primary_value:08x}" if primary_value is not None else "READ_ERROR"
            shadow[name] = f"0x{shadow_value:08x}" if shadow_value is not None else "READ_ERROR"
        for word_index in range(0x30, 0x70):
            primary_value = read_register(fd, 0x4000 + word_index * 4)
            primary[f"PROGRAM_{word_index:02x}"] = f"0x{primary_value:08x}" if primary_value is not None else "READ_ERROR"
        os.close(fd)
    except OSError as exception:
        primary["_error"] = str(exception)
    return {"primary_0x4000": primary, "shadow_0x6000": shadow}


def read_alsa_state():
    state = {}
    for filename in ["cards", "devices", "pcm"]:
        path = f"/proc/asound/{filename}"
        try:
            with open(path) as handle:
                state[filename] = handle.read().strip()
        except OSError:
            state[filename] = "UNAVAILABLE"

    for direction in ["pcm0p", "pcm0c"]:
        for subfile in ["info", "sub0/hw_params", "sub0/status"]:
            path = f"/proc/asound/card0/{direction}/{subfile}"
            key = f"{direction}/{subfile}"
            try:
                with open(path) as handle:
                    state[key] = handle.read().strip()
            except OSError:
                state[key] = "UNAVAILABLE"
    return state


def read_interrupts():
    try:
        with open("/proc/interrupts") as handle:
            for line in handle:
                if "dante" in line.lower() or "DantePCIe" in line or "snd_dante" in line:
                    return line.strip()
    except OSError:
        pass
    return "NOT_FOUND"


def read_meter_peaks():
    peaks = {"rx": {}, "tx": {}}
    try:
        with open(f"{DEBUGFS}/meters") as handle:
            current = None
            for line in handle:
                if line.startswith("RX"):
                    current = peaks["rx"]
                elif line.startswith("TX"):
                    current = peaks["tx"]
                elif current is not None and ":" in line:
                    parts = line.strip().split()
                    if len(parts) >= 3:
                        try:
                            channel = int(parts[0].rstrip(":"))
                            peak_value = parts[-1]
                            if peak_value != "-inf":
                                current[str(channel)] = peak_value
                        except (ValueError, IndexError):
                            pass
    except OSError:
        peaks["_error"] = "meters not available"
    return peaks


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "snapshot"
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_path = os.path.join(OUTPUT_DIR, f"device_state_{label}_{timestamp}.json")

    snapshot = {
        "metadata": {
            "timestamp": datetime.now().isoformat(),
            "label": label,
            "kernel": os.uname().release,
            "driver": "original" if label != "replacement" else "replacement",
        },
        "bar0_registers": read_bar_registers(f"{DEBUGFS}/bar0", BAR0_REGISTERS),
        "bar4_registers": read_bar_registers(f"{DEBUGFS}/bar4", BAR4_REGISTERS),
        "descriptor_ram": read_descriptor_ram(f"{DEBUGFS}/bar0"),
        "alsa_state": read_alsa_state(),
        "interrupts": read_interrupts(),
        "meter_peaks": read_meter_peaks(),
    }

    with open(output_path, "w") as handle:
        json.dump(snapshot, handle, indent=2)

    print(f"Snapshot saved to {output_path}")
    print(f"BAR0 CSR: {snapshot['bar0_registers'].get('VDMA_CSR', {}).get('value', '?')}")
    print(f"Firmware: {snapshot['bar0_registers'].get('FIRMWARE_VERSION', {}).get('value', '?')}")
    print(f"Active RX peaks: {len(snapshot['meter_peaks'].get('rx', {}))}")
    print(f"Active TX peaks: {len(snapshot['meter_peaks'].get('tx', {}))}")


if __name__ == "__main__":
    main()

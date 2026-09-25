"""Wire contract: docs/PROTOCOL.md. All values are integers in wire units."""

import struct

TEMPERATURE = 0
DOOR = 1


def crc8(data: bytes) -> int:
    """CRC-8/SMBUS: poly 0x07, init/xorout 0, no reflection."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ (0x07 if crc & 0x80 else 0)) & 0xFF
    return crc


def _integer(name: str, value: int, low: int, high: int) -> None:
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{name} must be an integer in {low}..{high}")


def reading(pod: int, channel: int, value: int, status: int,
            counter: int) -> tuple[int, bytes]:
    _integer("pod", pod, 1, 127)
    _integer("channel", channel, 0, 1)
    _integer("value", value, -32768, 32767)
    _integer("status", status, 0, 1)
    _integer("counter", counter, 0, 255)
    if channel == DOOR and value not in (0, 1):
        raise ValueError("door value must be 0 or 1")
    data = struct.pack("<BhBB", channel, value, status, counter)
    return 0x100 + pod, data + bytes([crc8(data)])


def heartbeat(pod: int, state: int, uptime_s: int,
              counter: int) -> tuple[int, bytes]:
    _integer("pod", pod, 1, 127)
    _integer("state", state, 0, 2)
    _integer("uptime", uptime_s, 0, 0xFFFFFFFF)
    _integer("counter", counter, 0, 255)
    data = struct.pack("<BIB", state, uptime_s, counter)
    return 0x700 + pod, data + bytes([crc8(data)])

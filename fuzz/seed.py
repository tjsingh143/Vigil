#!/usr/bin/env python3
"""Deterministic seed corpus, including literal cross-language fixtures."""
from pathlib import Path
import struct
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from simulator.protocol import crc8

path = Path(sys.argv[1])
path.mkdir(parents=True, exist_ok=True)
seeds = []
for line in (ROOT / 'tests/protocol_vectors.txt').read_text().splitlines():
    if line and not line.startswith('#'):
        can_id, payload, *_ = line.split()
        seeds.append(struct.pack('<IB', int(can_id, 16), 0) + bytes.fromhex(payload))
for length in range(10):
    seeds.append(struct.pack('<IB', 0x101, 0) + bytes(length))
for raw_id, fd in ((0, 0), (0x80000101, 0), (0x40000101, 0), (0x20000101, 0), (0x101, 1)):
    seeds.append(struct.pack('<IB', raw_id, fd) + bytes.fromhex('0098080000b7'))
for can_id, payload in ((0x101, '0298080000'), (0x101, '0098080200'),
                        (0x101, '0102000000'), (0x701, '030000000000')):
    data = bytes.fromhex(payload)
    seeds.append(struct.pack('<IB', can_id, 0) + data + bytes([crc8(data)]))
seeds.append(struct.pack('<IB', 0x101, 0) + bytes.fromhex('0098080000b6'))
for index, seed in enumerate(seeds):
    (path / f'seed-{index:02d}').write_bytes(seed)
print(f'Created {len(seeds)} deterministic seeds in {path}')

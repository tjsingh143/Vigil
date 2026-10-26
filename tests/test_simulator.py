"""Expected behavior: literal wire vectors, strict input ranges, deterministic pods."""
from verification import require  # also refuses optimized execution before fixture imports

from pathlib import Path
import unittest

from simulator.pod import Pod
from simulator.protocol import crc8, heartbeat, reading

VECTORS = Path(__file__).with_name("protocol_vectors.txt")


class ProtocolTests(unittest.TestCase):
    def test_known_crc(self):
        self.assertEqual(crc8(b"123456789"), 0xF4)
        self.assertEqual(crc8(b""), 0)

    def test_literal_vectors(self):
        rows = 0
        for line in VECTORS.read_text().splitlines():
            if line.startswith("#"):
                continue
            can_id, payload, *fields = line.split()
            kind, pod, channel, value, status, counter, state, uptime = map(int, fields)
            with self.subTest(payload=payload):
                actual = (reading(pod, channel, value, status, counter) if kind == 1
                          else heartbeat(pod, state, uptime, counter))
                self.assertEqual(actual, (int(can_id, 16), bytes.fromhex(payload)))
            rows += 1
        self.assertEqual(rows, 10)

    def test_reject_invalid_readings(self):
        defaults = [1, 0, 2200, 0, 0]
        bad = {0: (0, 128, -1, True), 1: (-1, 2), 2: (-32769, 32768, 1.5),
               3: (-1, 2), 4: (-1, 256)}
        for index, values in bad.items():
            for value in values:
                args = defaults.copy()
                args[index] = value
                with self.subTest(args=args), self.assertRaises(ValueError):
                    reading(*args)
        for value in (-1, 2, 32767):
            with self.assertRaises(ValueError):
                reading(1, 1, value, 0, 0)

    def test_reject_invalid_heartbeats(self):
        for args in [(0, 1, 0, 0), (128, 1, 0, 0), (1, 3, 0, 0),
                     (1, 1, -1, 0), (1, 1, 2**32, 0), (1, 1, 0, 256)]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                heartbeat(*args)

    def test_cycle_and_independent_pods(self):
        one, two = Pod(1), Pod(2)
        self.assertEqual(one.cycle(0, 0), [
            (0x101, bytes.fromhex("0098080000b7")),
            (0x101, bytes.fromhex("010000000165")),
            (0x701, bytes.fromhex("01000000000029"))])
        self.assertEqual(two.reading_counter, 0)
        self.assertEqual(one.reading_counter, 2)
        self.assertEqual(one.heartbeat_counter, 1)

    def test_repeatability_and_wrap(self):
        one, copy = Pod(1), Pod(1)
        for tick in range(300):
            frames = one.cycle(tick, tick)
            self.assertEqual(frames, copy.cycle(tick, tick))
            self.assertEqual(frames[0][1][4], tick * 2 % 256)
            self.assertEqual(frames[1][1][4], (tick * 2 + 1) % 256)
            self.assertEqual(frames[2][1][5], tick % 256)
            temperature = int.from_bytes(frames[0][1][1:3], "little", signed=True)
            self.assertTrue(2200 <= temperature <= 2220)
            self.assertEqual(frames[1][1][1], tick // 10 % 2)


if __name__ == "__main__":
    unittest.main()

"""Deterministic two-channel pod, using genuine Linux SocketCAN."""

import argparse
import math
import signal
import sys
import threading
import time
from dataclasses import dataclass

from .protocol import DOOR, TEMPERATURE, heartbeat, reading


@dataclass
class Pod:
    pod_id: int
    reading_counter: int = 0
    heartbeat_counter: int = 0

    def cycle(self, tick: int, uptime_s: int) -> list[tuple[int, bytes]]:
        """Stable deterministic triangle, 22.00..22.20 C; door toggles every 10 ticks."""
        phase = tick % 40
        temperature = 2200 + (phase if phase <= 20 else 40 - phase)
        frames = [reading(self.pod_id, TEMPERATURE, temperature, 0,
                          self.reading_counter),
                  reading(self.pod_id, DOOR, (tick // 10) % 2, 0,
                          (self.reading_counter + 1) % 256),
                  heartbeat(self.pod_id, 1, uptime_s % (2**32),
                            self.heartbeat_counter)]
        self.reading_counter = (self.reading_counter + 2) % 256
        self.heartbeat_counter = (self.heartbeat_counter + 1) % 256
        return frames


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", default="vcan0")
    parser.add_argument("--pod-id", type=int, action="append",
                        help="repeat for a second pod; default 1")
    parser.add_argument("--interval", type=float, default=1.0,
                        help="seconds between cycles (default 1)")
    parser.add_argument("--cycles", type=int, default=0,
                        help="stop after N cycles; 0 means run until signalled")
    parser.add_argument("--repeat-each-frame", action="store_true",
                        help="fault injection: immediately transmit each frame twice")
    args = parser.parse_args()
    ids = args.pod_id or [1]
    if (not 1 <= len(ids) <= 2 or len(set(ids)) != len(ids) or
            any(not 1 <= pod <= 127 for pod in ids)):
        parser.error("choose one or two distinct pod IDs in 1..127")
    if not math.isfinite(args.interval) or not 0.001 <= args.interval <= 3600:
        parser.error("interval must be finite and in 0.001..3600 seconds")
    if args.cycles < 0:
        parser.error("cycles cannot be negative")
    if sys.platform != "linux":
        parser.error("real Linux SocketCAN is required; use scripts/dev")
    import can  # Pure Pod/encoder tests do not require a transport.

    stop = threading.Event()
    def shutdown(_signum: int, _frame: object) -> None:
        stop.set()
    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)
    pods = [Pod(pod) for pod in ids]
    start = time.monotonic()
    tick = 0
    try:
        with can.Bus(interface="socketcan", channel=args.interface,
                     ignore_config=True, fd=False) as bus:
            print(f"ready interface={args.interface} pods={ids}", flush=True)
            while not stop.is_set() and (args.cycles == 0 or tick < args.cycles):
                uptime_s = int(time.monotonic() - start)
                for pod in pods:
                    for can_id, data in pod.cycle(tick, uptime_s):
                        for _ in range(2 if args.repeat_each_frame else 1):
                            bus.send(can.Message(arbitration_id=can_id, data=data,
                                                 is_extended_id=False, check=True),
                                     timeout=1.0)
                tick += 1
                if args.cycles and tick >= args.cycles:
                    break
                # No catch-up burst after scheduling delays.
                stop.wait(args.interval)
    except (can.CanError, OSError) as exc:
        print(f"simulator: {exc}", file=sys.stderr)
        return 1
    print(f"stopped cycles={tick}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

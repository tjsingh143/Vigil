#!/usr/bin/env python3
"""Record the small candump/cansend checklist exchange (supplements fault assertions)."""
from verification import require  # also refuses optimized execution before fixture imports
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
from evidence import Evidence
from integration import process, line_ready


def exchange(decoder):
    # This masked filter is registered in rx_fil after bind. Wait for its actual
    # subscription rather than guessing readiness with a fixed delay.
    with process(['candump', '-L', '-n', '3', 'vcan0,101:7FF']) as dump:
        deadline = time.monotonic() + 3
        while True:
            require((dump.poll() is None), lambda: ('candump exited before subscription'))
            filters = Path('/proc/net/can/rcvlist_fil').read_text()
            if any(line.split()[:3] == ['vcan0', '101', '000007ff'] for line in filters.splitlines()):
                break
            require((time.monotonic() < deadline), lambda: (filters))
            time.sleep(0.01)
        with process([decoder, '--listen', 'vcan0', '3']) as probe:
            line_ready(probe.stderr)
            commands = []
            for frame in ('101#0098080000B7', '101#00', '101#0098080000B6'):
                command = ['cansend', 'vcan0', frame]
                print('COMMAND:', ' '.join(command), flush=True)
                subprocess.run(command, check=True, timeout=3)
                commands.append(command)
            decoded, err = probe.communicate(timeout=5)
            observed, dump_err = dump.communicate(timeout=5)
            require((probe.returncode == 1 and not err), lambda: ((probe.returncode, err)))
            require((dump.returncode == 0 and not dump_err), lambda: (dump_err))
            rows = [json.loads(line) for line in decoded.splitlines()]
            require(([row['result'] for row in rows] == ['ok', 'length', 'crc']), lambda: (rows))
            require((rows[0]['value'] == 2200))
            require(([line.split()[-1] for line in observed.splitlines()] == [c[-1] for c in commands]))
            print(observed, end='')
            print(decoded, end='')
            return dict(commands=commands, candump=observed, decoder=rows, decoder_exit=probe.returncode)


def main():
    if sys.platform != 'linux' or sys.flags.optimize:
        raise RuntimeError('real Linux required; assertions must remain enabled')
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--decoder', default='build/vigil-decode')
    args = parser.parse_args()
    evidence = Evidence(args.output)
    evidence.run('can-utils', 'three literal cansend frames observed independently by candump; '
                 'decoder reports ok/length/crc and exits 1 for deliberate invalid input',
                 lambda: exchange(str(Path(args.decoder).resolve())))
    evidence.finish()


if __name__ == '__main__':
    main()

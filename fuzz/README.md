# Decoder fuzz target

`./scripts/fuzz 120` builds `fuzz-decoder` with Clang/libFuzzer, AddressSanitizer
and UndefinedBehaviorSanitizer, then runs a 120-second budget (libFuzzer normally
finishes the last interval slightly after the requested budget). Exit nonzero
means failure. `tee` is protected by `pipefail`. The script keeps its exact command,
log, mutated corpus and any crash/timeout input under a new `artifacts/fuzz-*`
directory. CI uses 10 seconds. Setup installs Clang and its runtime.

Input layout: four little-endian raw CAN-ID/SocketCAN-flag bytes, one byte whose
low bit selects CAN FD, then the remaining bytes as payload. The fuzz length
limit is 69 bytes (five header + 64 payload); the pure decoder only accepts
classic six/seven-byte payloads. It receives the actual available length, never
a fabricated readable buffer length. Very short inputs return immediately.

`seed.py` deterministically creates 30 inputs from the shared literal wire
vectors, length boundaries, format flags, invalid IDs, bad CRC and CRC-correct
invalid enums. It does not change wire behavior or fixtures. Rejected frames
must leave every output byte unchanged. Successful fields satisfy protocol
invariants. Null arguments are rejected. Assertions remain enabled even if the
build mode defines NDEBUG. There are no clocks, sockets, heap allocations,
randomness or global mutable state in the target.

The decoder itself is compiled with sanitizer/coverage instrumentation, not
linked from an uninstrumented library. ASan checks memory accesses; UBSan checks
undefined operations; the assertions check part of the API contract. This target
does not exercise stateful monitoring, concurrency, persistence, systemd or
physical CAN. A clean finite fuzz session is not proof of correctness.

Reproduce a saved failure (use its actual path):

```sh
build-fuzz/fuzz-decoder artifacts/fuzz-RUN/crash-HASH
```

The actual command and observed results are in [verification](../docs/VERIFICATION.md).
The workflow follows the [LLVM libFuzzer interface](https://llvm.org/docs/LibFuzzer.html).

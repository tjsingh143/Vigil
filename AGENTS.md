# Vigil engineering rules

Vigil is developed by **one Codex agent only**. Do not spawn subagents,
delegate work, or launch other Codex instances.

Read all of `PROJECT_BRIEF.pdf` (version 3), `CODEX_STATE.md`,
`docs/DECISIONS.md`, and `docs/LEARNING_GUIDE.md` before changing the project.
The current brief is authoritative. Record necessary deviations explicitly.
Stay within the checkpoint the owner authorized; do not add deferred products.

Use real Linux for SocketCAN/vcan, timerfd, signalfd, and systemd. On Apple
Silicon use the repository's Ubuntu ARM64 Lima VM. Never substitute a Python
virtual bus or mocked macOS integration for the Linux integration gate.

Keep C warnings strict, decoding length checked, integers/endian conversions
explicit, and invalid frames incapable of mutating a decoded result. Update
the protocol document and cross-language vectors together when changing wire
behavior. CRC and counters do not authenticate senders. Flat readings are
suspicious, not proof of sensor failure. vcan does not simulate physical CAN.

Record expected test behavior before running tests. Build and execute relevant
checks, fix failures, then rerun the complete checkpoint gate from a clean
build. Report only observed results. Review the diff and update persistent
state and learning documentation before a local checkpoint commit.

Daemon ownership rules: monitoring owns in-memory alarms; database I/O belongs
only to the persistence thread. Queue locks never cover I/O. Freshness,
progress, and plausibility remain separate dimensions.

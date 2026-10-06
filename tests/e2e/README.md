# End-to-end audio recovery harness (real network)

Self-contained tools: no dependency on the library sources; they speak
raw RTP (PT=97) over UDP so they can validate the temporal-redundancy +
AudioBurstGuard design across a real lossy link (e.g. iPad Wi-Fi with
AWDL coexistence blackouts) from any Linux/macOS shell.

- `ipad_audio_client.c` — receiver
- `pc_audio_sender.c`  — sender
- `Makefile`           — `make` builds both

See `tests/README.md` for usage and expected results.

## `ipad_audio_client_v2` — arrival-lateness instrumentation

Variant of the receiver that records the **first-arrival lateness** of every
frame (histogram with 20ms-class buckets) plus a "late beyond deadline"
counter, and at stream end reports the *never-arrived* (true loss) floor.

Usage: `ipad_audio_client_v2 <port> <window_ms> <run_seconds>`

This distinguishes true RF losses from merely-late deliveries (AP MAC
retransmission bridging an AWDL blackout window), which is the key
measurement for sizing jitter buffers vs redundancy delays.

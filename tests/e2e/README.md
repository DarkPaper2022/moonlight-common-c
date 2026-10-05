# End-to-end audio recovery harness (real network)

Self-contained tools: no dependency on the library sources; they speak
raw RTP (PT=97) over UDP so they can validate the temporal-redundancy +
AudioBurstGuard design across a real lossy link (e.g. iPad Wi-Fi with
AWDL coexistence blackouts) from any Linux/macOS shell.

- `ipad_audio_client.c` — receiver
- `pc_audio_sender.c`  — sender
- `Makefile`           — `make` builds both

See `tests/README.md` for usage and expected results.

# Audio recovery test suite

## Deterministic unit tests (repo root)

- `test_audio_burst_guard.c` — covers all 80 AWDL phase alignments (0..395ms)
  of the 400ms/80ms interference pattern: asserts 0 PLC when temporal
  duplicates are enabled, and asserts exactly-one-PLC-frame per lost frame.
- `test_regression.c` — regression tests for the legacy `RtpAudioQueue`
  ms/us unit bug and whole-FEC-block skipping.

Build and run (no external dependencies beyond the bundled nanors):

```sh
gcc -O2 -Inanors -Inanors/deps/obl -Isrc \
    nanors/rs.c nanors/deps/obl/oblas_common.c nanors/deps/obl/oblas_lite.c \
    src/RtpAudioQueue.c src/AudioBurstRecovery.c \
    test_audio_burst_guard.c -o /tmp/test_guard && /tmp/test_guard
```

## Real-network end-to-end harness (`tests/e2e/`)

Self-contained UDP pair for validating the host-side temporal redundancy
+ client-side AudioBurstGuard loop over a real (lossy) Wi-Fi link,
without a full GameStream session:

- `ipad_audio_client.c` — receiver: RTP reordering slots (150ms window),
  duplicate/late-packet absorption, fake 5ms-paced sound card with
  starvation + PLC statistics, per-second log lines.
- `pc_audio_sender.c` — sender: 200 pkt/s high-precision RTP pacer with
  optional +120ms temporal redundancy duplicates (`redundant` mode).

Usage (receiver host IP 192.0.2.10, port 48000):

```sh
make -C tests/e2e
./tests/e2e/ipad_audio_client 48000                # on the receiver
./tests/e2e/pc_audio_sender 192.0.2.10 48000 redundant 15   # on the sender
```

Expected result with redundancy enabled on a typical AWDL-degraded link:
PLC = 0, starvation = 0, a steady 15-20ms fake-card water level, and
1-2 rescued frames per second reported by the receiver.

# AudioBurstGuard design notes

## Problem

Apple Wi-Fi stacks coexist with AWDL (AirDrop/Sidecar/Peer-to-Peer) by
periodically leaving the BSS channel for a ~80ms availability window,
roughly every 400ms. During those windows the streaming link blackens
out entirely. A 5ms/10ms Opus audio stream therefore loses one to
seventeen consecutive frames in a burst — far beyond what a small
de-jitter buffer absorbs, and RS FEC cannot repair whole-block losses.

## Design: two-sided mitigation

### Host side — temporal redundancy

The host sends every audio datagram twice: the original immediately,
and a byte-identical duplicate 120ms later. The delay is chosen so the
original and its duplicate can never fall inside the same 80ms blackout
window (80ms < D < 320ms for a 400ms scan period).

On the reference Sunshine host this is implemented as an
`LD_PRELOAD` `sendto`/`sendmsg` interceptor (`sunshine-audio-redundancy`
in the companion workspace) with a monotonic-clock timer thread; no
Sunshine source changes are required.

### Client side — deadline-driven playout guard

`AudioBurstGuard` (enabled via `LiSetAudioBurstGuardEnabled(true)`)
receives every audio datagram before the RTP queue:

1. **Timeline anchoring** — the first data frame pins a local
   monotonic timestamp to its RTP sequence number; every later frame's
   *nominal arrival time* is `anchor + (seq - anchorSeq) * frameDuration`.
2. **Slot storage** — frames land in fixed slots indexed by
   `seq % capacity`. Out-of-order or late duplicates fill whatever is
   still empty; a slot occupied by a pending frame swallows duplicates
   without disturbing the timeline.
3. **FEC recovery** — packets are also fed to the RS FEC queue;
   recovered frames are written into empty slots as
   `BURST_FRAME_FEC_RECOVERED`.
4. **Single decision per frame** — at `deadline = nominal + recovery
   window` (default 150ms) exactly one verdict is made per sequence
   number: original / duplicate / FEC-recovered payload is handed to
   the decoder, otherwise a single PLC frame is synthesized. The
   sequence then advances unconditionally, so a hole can never stall
   the pipeline or advance the deadline prematurely.

The decoder thread paces frames at the playout deadline with a
`PltSleepUs`-driven fixed-delay scheduler and keeps a small SDL output
cushion (default 20ms) in front of the DAC.

## Tuning

| Knob | Default | Meaning |
| --- | --- | --- |
| `LiSetAudioRecoveryWindowMs` | 150 | Deadline slack after nominal arrival; must exceed the host duplicate delay (120ms) plus jitter |
| `LiSetAudioPcmQueueMs` | 20 | SDL output cushion |
| `LiSetAudioPlayoutDelayMs` | 150 | Fixed playout delay of the scheduler |

## Validation

- Deterministic unit tests (`test_audio_burst_guard.c`) sweep all 80
  phase alignments of the 400ms/80ms pattern: 0 PLC with duplicates,
  exactly one PLC per otherwise-lost frame without.
- Real-network harness (`tests/e2e/`) reproduces the same result over
  live Wi-Fi: 0 PLC, 0 starvation, 1-2 rescued frames/s.
- `LiGetAudioStats()` reports live totals/loss rate for client UIs.

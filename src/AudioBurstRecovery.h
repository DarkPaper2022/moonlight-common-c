#pragma once

#include "Limelight.h"
#include "Limelight-internal.h"
#include "RtpAudioQueue.h"

#ifdef __cplusplus
extern "C" {
#endif

// Burst recovery result for a specific media frame
typedef enum _BURST_FRAME_STATUS {
    BURST_FRAME_EMPTY = 0,         // Waiting for deadline / packet
    BURST_FRAME_ORIGINAL,          // Available (original packet)
    BURST_FRAME_DUPLICATE,         // Available (duplicate packet)
    BURST_FRAME_FEC_RECOVERED,     // Recovered via Reed-Solomon FEC
    BURST_FRAME_PLC,               // Missing at deadline -> synthesize PLC
    BURST_FRAME_LATE,              // Arrived after deadline (dropped)
    BURST_FRAME_DUPLICATE_IGNORED  // Duplicate arrived when frame already received
} BURST_FRAME_STATUS;

typedef struct _BURST_SLOT {
    uint16_t sequenceNumber;
    uint32_t timestamp;
    uint64_t deadlineUs;           // Absolute monotonic decode deadline
    BURST_FRAME_STATUS status;
    bool finalized;
    uint16_t length;
    uint8_t* payload;              // Deep-copied datagram payload (owned)
} BURST_SLOT, *PBURST_SLOT;

#define BURST_SLOT_CAPACITY 256    // Power of 2, covers 256 * 5ms = 1280ms history

typedef struct _AUDIO_BURST_GUARD {
    bool enabled;
    uint32_t recoveryWindowMs;     // Default: 150 ms
    uint32_t pcmTargetQueueMs;     // Default: 20 ms
    uint64_t anchorLocalUs;
    uint16_t anchorSeq;
    bool anchored;

    uint16_t nextExpectedSeq;
    uint64_t nextExpectedDeadlineUs;

    // Ring buffer of media frame slots indexed by seq % BURST_SLOT_CAPACITY
    BURST_SLOT slots[BURST_SLOT_CAPACITY];

    // Under-the-hood RS FEC helper (4 data + 2 parity)
    RTP_AUDIO_QUEUE fecQueue;

    // Statistics
    uint64_t countTotalFinalized;
    uint64_t countOriginalDelivered;
    uint64_t countDuplicateDelivered;
    uint64_t countFecDelivered;
    uint64_t countPlcSynthesized;
    uint64_t countLateDiscarded;
    uint64_t countDuplicateDiscarded;
} AUDIO_BURST_GUARD, *PAUDIO_BURST_GUARD;

void AudioBurstGuardInit(PAUDIO_BURST_GUARD guard, uint32_t recoveryWindowMs, uint32_t pcmQueueMs);
void AudioBurstGuardCleanup(PAUDIO_BURST_GUARD guard);
void AudioBurstGuardReset(PAUDIO_BURST_GUARD guard);
bool AudioBurstGuardGetStats(PAUDIO_BURST_GUARD guard, PAUDIO_STATS stats);

// Ingest incoming packet (PT97 audio or PT127 FEC)
// Returns true if packet was accepted
bool AudioBurstGuardIngest(PAUDIO_BURST_GUARD guard, const uint8_t* datagram, uint16_t length, uint64_t nowUs);

// Check if next frame is ready to be finalized
// nowUs: current monotonic time
// If nowUs >= nextDeadline, pop and return the frame data (or NULL if PLC)
// outLength: length of returned payload, or 0 if PLC
// outSeq: sequence number of finalized frame
uint8_t* AudioBurstGuardFinalizeNext(PAUDIO_BURST_GUARD guard, uint64_t nowUs, uint16_t* outLength, uint16_t* outSeq, BURST_FRAME_STATUS* outStatus);

// Get deadline of next frame waiting to be finalized
uint64_t AudioBurstGuardGetNextDeadlineUs(PAUDIO_BURST_GUARD guard);

#ifdef __cplusplus
}
#endif

#include "AudioBurstRecovery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RTP_PT_AUDIO 97
#define RTP_PT_FEC   127

void AudioBurstGuardInit(PAUDIO_BURST_GUARD guard, uint32_t recoveryWindowMs, uint32_t pcmQueueMs) {
    memset(guard, 0, sizeof(*guard));
    guard->enabled = true;
    guard->recoveryWindowMs = (recoveryWindowMs > 0) ? recoveryWindowMs : 150;
    guard->pcmTargetQueueMs = (pcmQueueMs > 0) ? pcmQueueMs : 20;
    RtpaInitializeQueue(&guard->fecQueue);
}

void AudioBurstGuardCleanup(PAUDIO_BURST_GUARD guard) {
    AudioBurstGuardReset(guard);
    RtpaCleanupQueue(&guard->fecQueue);
}

void AudioBurstGuardReset(PAUDIO_BURST_GUARD guard) {
    for (int i = 0; i < BURST_SLOT_CAPACITY; i++) {
        if (guard->slots[i].payload != NULL) {
            free(guard->slots[i].payload);
            guard->slots[i].payload = NULL;
        }
        guard->slots[i].status = BURST_FRAME_EMPTY;
        guard->slots[i].finalized = false;
        guard->slots[i].length = 0;
    }
    guard->anchored = false;
    guard->anchorLocalUs = 0;
    guard->anchorSeq = 0;
}

static uint64_t calculateDeadline(PAUDIO_BURST_GUARD guard, uint16_t seq) {
    uint64_t frameDurationUs = (uint64_t)AudioPacketDuration * 1000;
    int16_t diff = (int16_t)(seq - guard->anchorSeq);
    int64_t nominalUs = (int64_t)guard->anchorLocalUs + ((int64_t)diff * (int64_t)frameDurationUs);
    if (nominalUs < 0) nominalUs = 0;
    return (uint64_t)nominalUs + ((uint64_t)guard->recoveryWindowMs * 1000);
}

bool AudioBurstGuardIngest(PAUDIO_BURST_GUARD guard, const uint8_t* datagram, uint16_t length, uint64_t nowUs) {
    if (!guard->enabled || length < sizeof(RTP_PACKET)) {
        return false;
    }

    PRTP_PACKET rtp = (PRTP_PACKET)datagram;
    uint8_t pt = rtp->packetType;

    if (pt == RTP_PT_AUDIO) {
        uint16_t seq = rtp->sequenceNumber;
        uint32_t ts = rtp->timestamp;

        // Establish anchor on the first received data frame
        if (!guard->anchored) {
            guard->anchored = true;
            guard->anchorSeq = seq;
            guard->anchorLocalUs = nowUs;

            // Anchor establishes the timeline. The initial expected sequence number is seq.
            guard->nextExpectedSeq = seq;
            guard->nextExpectedDeadlineUs = calculateDeadline(guard, seq);
        } else {
            // If we receive a packet before our anchorSeq (e.g. duplicate of an earlier packet that was lost during initial blackout)
            int16_t diffAnchor = (int16_t)(seq - guard->anchorSeq);
            if (diffAnchor < 0) {
                // Re-anchor to the earlier packet so that its deadline and timeline are correctly maintained
                uint64_t frameDurationUs = (uint64_t)AudioPacketDuration * 1000;
                int64_t shiftUs = (int64_t)(-diffAnchor) * (int64_t)frameDurationUs;
                if ((int64_t)guard->anchorLocalUs >= shiftUs) {
                    guard->anchorLocalUs -= shiftUs;
                } else {
                    guard->anchorLocalUs = 0;
                }
                guard->anchorSeq = seq;
                guard->nextExpectedSeq = seq;
                guard->nextExpectedDeadlineUs = calculateDeadline(guard, seq);
            }
        }

        // Check if packet is already past its deadline / already finalized
        int16_t diffExpected = (int16_t)(seq - guard->nextExpectedSeq);
        if (diffExpected < 0) {
            // Already finalized/played sequence number -> late discard
            guard->countLateDiscarded++;
            return false;
        }

        if (diffExpected >= BURST_SLOT_CAPACITY) {
            // Unreasonable leap ahead -> out of buffer capacity
            return false;
        }

        int slotIdx = seq % BURST_SLOT_CAPACITY;
        PBURST_SLOT slot = &guard->slots[slotIdx];

        if (slot->status != BURST_FRAME_EMPTY && !slot->finalized && slot->sequenceNumber == seq) {
            // Duplicate of pending frame!
            guard->countDuplicateDiscarded++;
            return true;
        }

        uint64_t frameDurationUs = (uint64_t)AudioPacketDuration * 1000;
        int16_t diff = (int16_t)(seq - guard->anchorSeq);
        int64_t nominalUs = (int64_t)guard->anchorLocalUs + ((int64_t)diff * (int64_t)frameDurationUs);
        bool isDuplicate = (nominalUs >= 0 && nowUs >= (uint64_t)nominalUs + 60000);

        // Allocate and store
        if (slot->payload != NULL) {
            free(slot->payload);
            slot->payload = NULL;
        }

        slot->sequenceNumber = seq;
        slot->timestamp = ts;
        slot->deadlineUs = calculateDeadline(guard, seq);
        slot->finalized = false;
        slot->length = length;
        slot->payload = malloc(length);
        if (slot->payload != NULL) {
            memcpy(slot->payload, datagram, length);
            slot->status = isDuplicate ? BURST_FRAME_DUPLICATE : BURST_FRAME_ORIGINAL;
        }

        // Also pass to FEC queue for multi-shard recovery
        RtpaAddPacket(&guard->fecQueue, rtp, length);
        return true;
    } else if (pt == RTP_PT_FEC) {
        // Feed into RS FEC queue
        int res = RtpaAddPacket(&guard->fecQueue, rtp, length);
        if (RTPQ_PACKET_READY(res)) {
            // Check if any lost packets were recovered by RS
            uint16_t fecLen = 0;
            PRTP_PACKET rec = NULL;
            while ((rec = RtpaGetQueuedPacket(&guard->fecQueue, 0, &fecLen)) != NULL) {
                if (fecLen >= sizeof(RTP_PACKET)) {
                    uint16_t recSeq = rec->sequenceNumber;
                    int16_t diff = (int16_t)(recSeq - guard->nextExpectedSeq);
                    if (diff >= 0 && diff < BURST_SLOT_CAPACITY) {
                        int sIdx = recSeq % BURST_SLOT_CAPACITY;
                        PBURST_SLOT s = &guard->slots[sIdx];
                        if (s->status == BURST_FRAME_EMPTY) {
                            s->sequenceNumber = recSeq;
                            s->timestamp = rec->timestamp;
                            s->deadlineUs = calculateDeadline(guard, recSeq);
                            s->finalized = false;
                            s->length = fecLen;
                            s->payload = malloc(fecLen);
                            if (s->payload != NULL) {
                                memcpy(s->payload, rec, fecLen);
                                s->status = BURST_FRAME_FEC_RECOVERED;
                            }
                        }
                    }
                }
                free(rec);
            }
        }
        return true;
    }

    return false;
}

uint64_t AudioBurstGuardGetNextDeadlineUs(PAUDIO_BURST_GUARD guard) {
    if (!guard->anchored) {
        return 0;
    }
    return guard->nextExpectedDeadlineUs;
}

uint8_t* AudioBurstGuardFinalizeNext(PAUDIO_BURST_GUARD guard, uint64_t nowUs, uint16_t* outLength, uint16_t* outSeq, BURST_FRAME_STATUS* outStatus) {
    if (!guard->anchored) {
        *outLength = 0;
        return NULL;
    }

    if (nowUs < guard->nextExpectedDeadlineUs) {
        // Not yet deadline for nextExpectedSeq
        return NULL;
    }

    uint16_t seq = guard->nextExpectedSeq;
    *outSeq = seq;
    int slotIdx = seq % BURST_SLOT_CAPACITY;
    PBURST_SLOT slot = &guard->slots[slotIdx];

    uint8_t* resultPayload = NULL;
    guard->countTotalFinalized++;

    if (slot->status != BURST_FRAME_EMPTY && slot->sequenceNumber == seq && slot->payload != NULL) {
        // Successfully available at deadline (Original, Duplicate, or FEC-recovered)
        resultPayload = slot->payload;
        *outLength = slot->length;
        *outStatus = slot->status;

        if (*outStatus == BURST_FRAME_DUPLICATE) {
            guard->countDuplicateDelivered++;
        } else if (*outStatus == BURST_FRAME_FEC_RECOVERED) {
            guard->countFecDelivered++;
        } else {
            guard->countOriginalDelivered++;
        }

        // Caller takes ownership of payload
        slot->payload = NULL;
    } else {
        // Missing at deadline -> synthesize PLC
        *outLength = 0;
        *outStatus = BURST_FRAME_PLC;
        guard->countPlcSynthesized++;
    }

    slot->finalized = true;
    slot->status = BURST_FRAME_EMPTY;

    // Advance next expected sequence number and deadline
    guard->nextExpectedSeq++;
    guard->nextExpectedDeadlineUs = calculateDeadline(guard, guard->nextExpectedSeq);

    return resultPayload;
}

bool AudioBurstGuardGetStats(PAUDIO_BURST_GUARD guard, PAUDIO_STATS stats) {
    if (!guard || !stats) {
        return false;
    }
    stats->totalFrames = guard->countTotalFinalized;
    stats->plcFrames = guard->countPlcSynthesized;
    stats->originalFrames = guard->countOriginalDelivered;
    stats->duplicateFrames = guard->countDuplicateDelivered;
    stats->fecFrames = guard->countFecDelivered;
    stats->lossRatePercent = (guard->countTotalFinalized > 0) ?
        ((float)guard->countPlcSynthesized * 100.0f / (float)guard->countTotalFinalized) : 0.0f;
    return true;
}

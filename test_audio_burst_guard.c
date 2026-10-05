#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "rs.h"
#include "Limelight.h"
#include "Limelight-internal.h"
#include "AudioBurstRecovery.h"

int AppVersionQuad[4] = {7, 1, 450, 0};
int AudioPacketDuration = 5;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks = {0};

uint64_t simulated_time_us = 0;
uint64_t PltGetMicroseconds(void) {
    return simulated_time_us;
}

#define BLOCK_BYTES 32

// T01: Lossless transmission - 100% delivered, 0 duplicates played, 0 PLC
void test_t01_lossless() {
    AUDIO_BURST_GUARD guard;
    AudioBurstGuardInit(&guard, 150, 20);

    int frames = 100;
    int delivered = 0, plc = 0;

    for (int ms = 0; ms <= frames * 5 + 200; ms += 5) {
        simulated_time_us = (uint64_t)ms * 1000;
        int f = ms / 5;
        if (f < frames) {
            uint8_t buf[sizeof(RTP_PACKET) + BLOCK_BYTES];
            PRTP_PACKET rtp = (PRTP_PACKET)buf;
            rtp->header = 0x80;
            rtp->packetType = 97;
            rtp->sequenceNumber = (uint16_t)f;
            rtp->timestamp = f * 5;
            rtp->ssrc = 1;
            memset(buf + sizeof(RTP_PACKET), 0xAA, BLOCK_BYTES);
            AudioBurstGuardIngest(&guard, buf, sizeof(buf), simulated_time_us);
        }

        uint16_t outLen = 0, outSeq = 0;
        BURST_FRAME_STATUS status;
        while (guard.anchored) {
            status = BURST_FRAME_EMPTY;
            uint8_t* p = AudioBurstGuardFinalizeNext(&guard, simulated_time_us, &outLen, &outSeq, &status);
            if (p != NULL) {
                delivered++;
                free(p);
            } else if (status == BURST_FRAME_PLC) {
                plc++;
            } else {
                break;
            }
            if (outSeq >= frames - 1) break;
        }
        if (outSeq >= frames - 1) break;
    }

    AUDIO_STATS stats;
    AudioBurstGuardGetStats(&guard, &stats);
    printf("T01 (Lossless): total=%llu, delivered=%llu, plc=%llu, loss=%.2f%%\n",
           (unsigned long long)stats.totalFrames, (unsigned long long)stats.originalFrames,
           (unsigned long long)stats.plcFrames, stats.lossRatePercent);
    assert(stats.totalFrames == frames);
    assert(stats.plcFrames == 0);
    assert(stats.lossRatePercent == 0.0f);
    AudioBurstGuardCleanup(&guard);
}

// T02: AWDL 80/400ms erasure across all 80 phases - 100% recovered, 0 PLC
void test_t02_all_phases() {
    int frames = 120;
    for (int phase_ms = 0; phase_ms < 400; phase_ms += 5) {
        AUDIO_BURST_GUARD guard;
        AudioBurstGuardInit(&guard, 150, 20);

        int delivered = 0, plc = 0;
        for (int ms = 0; ms <= frames * 5 + 300; ms += 5) {
            simulated_time_us = (uint64_t)ms * 1000;
            int f = ms / 5;
            if (f < frames) {
                bool orig_in_blackout = (((ms - phase_ms) % 400 + 400) % 400) < 80;
                if (!orig_in_blackout) {
                    uint8_t buf[sizeof(RTP_PACKET) + BLOCK_BYTES];
                    PRTP_PACKET rtp = (PRTP_PACKET)buf;
                    rtp->header = 0x80;
                    rtp->packetType = 97;
                    rtp->sequenceNumber = (uint16_t)f;
                    rtp->timestamp = f * 5;
                    rtp->ssrc = 1;
                    memset(buf + sizeof(RTP_PACKET), 0xAA, BLOCK_BYTES);
                    AudioBurstGuardIngest(&guard, buf, sizeof(buf), simulated_time_us);
                }
            }

            int dup_f = f - 24; // 120ms later
            if (dup_f >= 0 && dup_f < frames) {
                bool dup_in_blackout = (((ms - phase_ms) % 400 + 400) % 400) < 80;
                if (!dup_in_blackout) {
                    uint8_t buf[sizeof(RTP_PACKET) + BLOCK_BYTES];
                    PRTP_PACKET rtp = (PRTP_PACKET)buf;
                    rtp->header = 0x80;
                    rtp->packetType = 97;
                    rtp->sequenceNumber = (uint16_t)dup_f;
                    rtp->timestamp = dup_f * 5;
                    rtp->ssrc = 1;
                    memset(buf + sizeof(RTP_PACKET), 0xAA, BLOCK_BYTES);
                    AudioBurstGuardIngest(&guard, buf, sizeof(buf), simulated_time_us);
                }
            }

            uint16_t outLen = 0, outSeq = 0;
            BURST_FRAME_STATUS status;
            while (guard.anchored) {
                status = BURST_FRAME_EMPTY;
                uint8_t* p = AudioBurstGuardFinalizeNext(&guard, simulated_time_us, &outLen, &outSeq, &status);
                if (p != NULL) {
                    delivered++;
                    free(p);
                } else if (status == BURST_FRAME_PLC) {
                    plc++;
                } else {
                    break;
                }
                if (outSeq >= frames - 1) break;
            }
            if (outSeq >= frames - 1) break;
        }

        AUDIO_STATS stats;
        AudioBurstGuardGetStats(&guard, &stats);
        AudioBurstGuardCleanup(&guard);
        if (stats.plcFrames != 0) {
            printf("T02 Failed at phase %d ms with %llu PLC!\n", phase_ms, (unsigned long long)stats.plcFrames);
            exit(1);
        }
    }
    printf("T02 (All 80 AWDL Phases): 100%% PASSED (0 PLC across every phase)\n");
}

// T03: Audio Loss Rate Accuracy Test (both dropped -> PLC forced)
void test_t03_loss_rate_metrics() {
    AUDIO_BURST_GUARD guard;
    AudioBurstGuardInit(&guard, 150, 20);

    int total_to_send = 100;
    for (int ms = 0; ms <= total_to_send * 5 + 200; ms += 5) {
        simulated_time_us = (uint64_t)ms * 1000;
        int f = ms / 5;
        if (f < total_to_send) {
            bool orig_lost = (f >= 10 && f < 20) || (f >= 30 && f < 35);
            if (!orig_lost) {
                uint8_t buf[sizeof(RTP_PACKET) + BLOCK_BYTES];
                PRTP_PACKET rtp = (PRTP_PACKET)buf;
                rtp->header = 0x80;
                rtp->packetType = 97;
                rtp->sequenceNumber = (uint16_t)f;
                rtp->timestamp = f * 5;
                rtp->ssrc = 1;
                memset(buf + sizeof(RTP_PACKET), 0xAA, BLOCK_BYTES);
                AudioBurstGuardIngest(&guard, buf, sizeof(buf), simulated_time_us);
            }
        }

        int dup_f = f - 24; // 120ms later
        if (dup_f >= 0 && dup_f < total_to_send) {
            bool dup_lost = (dup_f >= 30 && dup_f < 35);
            if (!dup_lost) {
                uint8_t buf[sizeof(RTP_PACKET) + BLOCK_BYTES];
                PRTP_PACKET rtp = (PRTP_PACKET)buf;
                rtp->header = 0x80;
                rtp->packetType = 97;
                rtp->sequenceNumber = (uint16_t)dup_f;
                rtp->timestamp = dup_f * 5;
                rtp->ssrc = 1;
                memset(buf + sizeof(RTP_PACKET), 0xAA, BLOCK_BYTES);
                AudioBurstGuardIngest(&guard, buf, sizeof(buf), simulated_time_us);
            }
        }

        uint16_t outLen = 0, outSeq = 0;
        BURST_FRAME_STATUS status;
        while (guard.anchored) {
            status = BURST_FRAME_EMPTY;
            uint8_t* p = AudioBurstGuardFinalizeNext(&guard, simulated_time_us, &outLen, &outSeq, &status);
            if (p != NULL) {
                free(p);
            } else if (status != BURST_FRAME_PLC) {
                break;
            }
            if (outSeq >= total_to_send - 1) break;
        }
        if (outSeq >= total_to_send - 1) break;
    }

    AUDIO_STATS stats;
    bool ok = AudioBurstGuardGetStats(&guard, &stats);
    assert(ok);

    printf("T03 (Metrics): total=%llu, orig=%llu, dup=%llu, lost(plc)=%llu, lossRate=%.2f%%\n",
           (unsigned long long)stats.totalFrames,
           (unsigned long long)stats.originalFrames,
           (unsigned long long)stats.duplicateFrames,
           (unsigned long long)stats.plcFrames,
           stats.lossRatePercent);

    assert(stats.totalFrames == 100);
    assert(stats.originalFrames == 85);
    assert(stats.duplicateFrames == 10);
    assert(stats.plcFrames == 5);
    assert(stats.lossRatePercent == 5.0f);
    printf("T03 (Metrics): PASSED!\n");
    AudioBurstGuardCleanup(&guard);
}

int main() {
    printf("=== AudioBurstRecovery Comprehensive Suite ===\n");
    test_t01_lossless();
    test_t02_all_phases();
    test_t03_loss_rate_metrics();
    printf("ALL TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}

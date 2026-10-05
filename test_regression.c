#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>
#include "rs.h"
#include "Limelight.h"
#include "Limelight-internal.h"
#include "RtpAudioQueue.h"

int AppVersionQuad[4] = {7, 1, 450, 0};
int AudioPacketDuration = 5;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks = {0};

uint64_t simulated_time_us = 0;
uint64_t PltGetMicroseconds(void) {
    return simulated_time_us;
}

#define BLOCK_BYTES 32

// Test T07: What happens when an entire FEC block is completely lost, and a future block arrives?
// Can the delayed duplicate recover the lost block in current RtpAudioQueue?
int test_whole_block_skip() {
    reed_solomon_init();
    RTP_AUDIO_QUEUE queue;
    RtpaInitializeQueue(&queue);

    // Initial sequence is synchronized on block 0 (seq 0,1,2,3)
    uint8_t buf[sizeof(RTP_PACKET) + BLOCK_BYTES];
    PRTP_PACKET rtp = (PRTP_PACKET)buf;
    rtp->header = 0x80;
    rtp->packetType = 97;
    rtp->sequenceNumber = 4;
    rtp->timestamp = 20;
    rtp->ssrc = 1;
    memset(buf + sizeof(RTP_PACKET), 0xAA, BLOCK_BYTES);

    simulated_time_us = 20000;
    // Deliver seq 4 (block 1 starts at 4)
    int res = RtpaAddPacket(&queue, rtp, sizeof(buf));
    printf("Add seq 4 -> res=%d, nextExpected=%u, oldestBase=%u\n",
           res, queue.nextRtpSequenceNumber, queue.oldestRtpBaseSequenceNumber);

    // Now: Block 2 (seq 8..11) and Block 3 (seq 12..15) completely lost (e.g. 40ms blackout)
    // Packet seq 16 arrives! (Block 4)
    rtp->sequenceNumber = 16;
    rtp->timestamp = 80;
    simulated_time_us += 60000;
    res = RtpaAddPacket(&queue, rtp, sizeof(buf));
    printf("Add seq 16 (after gap 8..15) -> res=%d, nextExpected=%u, oldestBase=%u\n",
           res, queue.nextRtpSequenceNumber, queue.oldestRtpBaseSequenceNumber);

    // Now, at t = 140ms (120ms after seq 8 was sent), duplicate of seq 8 arrives!
    rtp->sequenceNumber = 8;
    rtp->timestamp = 40;
    simulated_time_us += 60000;
    res = RtpaAddPacket(&queue, rtp, sizeof(buf));
    printf("Add duplicate seq 8 -> res=%d\n", res);

    if (res == 0) {
        printf("[CONFIRMED REGRESSION] Duplicate seq 8 was REJECTED because oldestBase was prematurely advanced to %u!\n",
               queue.oldestRtpBaseSequenceNumber);
    } else {
        printf("Duplicate seq 8 accepted!\n");
    }

    RtpaCleanupQueue(&queue);
    return (res == 0) ? 1 : 0;
}

int main() {
    printf("=== Baseline Regression Reproduction Test ===\n");
    int regression_found = test_whole_block_skip();
    return 0;
}

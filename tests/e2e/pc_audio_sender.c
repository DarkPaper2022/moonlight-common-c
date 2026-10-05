#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PACKET_DURATION_MS 5
#define DUP_DELAY_PACKETS (120 / PACKET_DURATION_MS) // 24 packets = 120ms
#define HISTORY_CAPACITY 128

#pragma pack(push, 1)
typedef struct _RTP_PACKET {
    uint8_t header;
    uint8_t packetType;
    uint16_t sequenceNumber;
    uint32_t timestamp;
    uint32_t ssrc;
} RTP_PACKET;
#pragma pack(pop)

typedef struct {
    uint8_t data[128];
    size_t len;
} SAVED_PACKET;

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (ts.tv_nsec / 1000);
}

int main(int argc, char** argv) {
    const char* targetIp = "192.168.125.168";
    int targetPort = 48000;
    bool enableRedundancy = false;
    int durationSec = 30;

    if (argc > 1) targetIp = argv[1];
    if (argc > 2) targetPort = atoi(argv[2]);
    if (argc > 3) {
        if (strcmp(argv[3], "redundant") == 0 || strcmp(argv[3], "1") == 0 || strcmp(argv[3], "true") == 0) {
            enableRedundancy = true;
        }
    }
    if (argc > 4) durationSec = atoi(argv[4]);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(targetPort);
    if (inet_pton(AF_INET, targetIp, &dest.sin_addr) <= 0) {
        perror("inet_pton");
        return 1;
    }

    printf("===================================================================\n");
    printf("  PC Audio RTP Streamer\n");
    printf("  Target: %s:%d\n", targetIp, targetPort);
    printf("  Duration: %d seconds | Packet Interval: %d ms (200 pkt/s)\n",
           durationSec, PACKET_DURATION_MS);
    printf("  Temporal Redundancy: %s (120ms duplicate injection)\n",
           enableRedundancy ? "ENABLED (+120ms redundant duplicates)" : "DISABLED (baseline single-path)");
    printf("===================================================================\n");

    SAVED_PACKET history[HISTORY_CAPACITY];
    int histCount = 0;

    uint16_t seq = 1;
    uint32_t ts = 0;
    uint64_t startTimeUs = get_time_us();
    uint64_t nextSendUs = startTimeUs;
    uint64_t endTimeUs = startTimeUs + (uint64_t)durationSec * 1000000ULL;

    uint64_t sentOriginal = 0;
    uint64_t sentDuplicate = 0;

    while (get_time_us() < endTimeUs) {
        uint64_t now = get_time_us();
        if (now < nextSendUs) {
            usleep(nextSendUs - now);
            now = get_time_us();
        }
        nextSendUs += PACKET_DURATION_MS * 1000;

        // Build 5ms Opus-like RTP audio packet
        uint8_t pktBuf[sizeof(RTP_PACKET) + 80];
        RTP_PACKET* rtp = (RTP_PACKET*)pktBuf;
        rtp->header = 0x80;
        rtp->packetType = 97;
        rtp->sequenceNumber = htons(seq);
        rtp->timestamp = htonl(ts);
        rtp->ssrc = htonl(0x12345678);

        // Dummy Opus audio payload
        memset(pktBuf + sizeof(RTP_PACKET), 0xAA, 80);

        // Send original packet
        sendto(sockfd, pktBuf, sizeof(pktBuf), 0, (struct sockaddr*)&dest, sizeof(dest));
        sentOriginal++;

        // Store into history ring buffer
        int curIdx = seq % HISTORY_CAPACITY;
        memcpy(history[curIdx].data, pktBuf, sizeof(pktBuf));
        history[curIdx].len = sizeof(pktBuf);
        histCount++;

        // If redundancy is enabled, send duplicate from 120ms (24 packets) ago!
        if (enableRedundancy && histCount > DUP_DELAY_PACKETS) {
            uint16_t dupSeq = seq - DUP_DELAY_PACKETS;
            int dupIdx = dupSeq % HISTORY_CAPACITY;
            sendto(sockfd, history[dupIdx].data, history[dupIdx].len, 0,
                   (struct sockaddr*)&dest, sizeof(dest));
            sentDuplicate++;
        }

        seq++;
        ts += 240; // 5ms @ 48kHz = 240 samples

        if (sentOriginal % 200 == 0) {
            printf("[PC Sender] Sent: %lu originals, %lu duplicates (Elapsed: %.1fs)\n",
                   sentOriginal, sentDuplicate, (double)(now - startTimeUs) / 1000000.0);
            fflush(stdout);
        }
    }

    printf("===================================================================\n");
    printf("  Stream Completed!\n");
    printf("  Total Sent: %lu originals, %lu duplicates (Total wire packets: %lu)\n",
           sentOriginal, sentDuplicate, sentOriginal + sentDuplicate);
    printf("===================================================================\n");

    close(sockfd);
    return 0;
}

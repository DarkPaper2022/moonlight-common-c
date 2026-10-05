#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PACKET_DURATION_MS 5
#define RECOVERY_WINDOW_MS 150
#define PCM_TARGET_QUEUE_MS 20
#define BURST_SLOT_CAPACITY 512

#pragma pack(push, 1)
typedef struct _RTP_PACKET {
    uint8_t header;
    uint8_t packetType;
    uint16_t sequenceNumber;
    uint32_t timestamp;
    uint32_t ssrc;
} RTP_PACKET, *PRTP_PACKET;
#pragma pack(pop)

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_RECEIVED_ORIGINAL,
    SLOT_RECEIVED_DUPLICATE_RESCUE,
    SLOT_FINALIZED
} SLOT_STATE;

typedef struct {
    uint16_t seq;
    uint32_t timestamp;
    uint64_t deadlineUs;
    SLOT_STATE state;
    uint16_t len;
} AUDIO_SLOT;

typedef struct {
    AUDIO_SLOT slots[BURST_SLOT_CAPACITY];
    bool anchored;
    uint16_t anchorSeq;
    uint64_t anchorLocalUs;
    uint16_t nextExpectedSeq;
    uint64_t nextExpectedDeadlineUs;

    // Playout / Sound card state
    uint32_t sdlQueuedMs;
    bool soundCardPlaying;
    uint64_t totalStarvations;
    uint64_t totalFramesPlayed;
    uint64_t totalPlcFrames;
    uint64_t totalOriginalDelivered;
    uint64_t totalDuplicateRescued;
    uint64_t totalDuplicateIgnored;
    uint64_t totalLateDiscarded;
    uint64_t totalPacketsRecv;

    // Window interval stats (for per-second reporting)
    uint32_t winRecvOrig;
    uint32_t winRecvDup;
    uint32_t winRescued;
    uint32_t winPlc;
    uint32_t winStarvation;

    pthread_mutex_t lock;
} CLIENT_STATE;

static CLIENT_STATE g_state;
static volatile bool g_running = true;

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (ts.tv_nsec / 1000);
}

static inline bool seq_less(uint16_t a, uint16_t b) {
    return (int16_t)(a - b) < 0;
}

static inline bool seq_greater(uint16_t a, uint16_t b) {
    return (int16_t)(a - b) > 0;
}

static void ingest_packet(const uint8_t* buf, size_t len, uint64_t nowUs) {
    if (len < sizeof(RTP_PACKET)) return;
    const RTP_PACKET* rtp = (const RTP_PACKET*)buf;
    if (rtp->packetType != 97) return;

    uint16_t seq = ntohs(rtp->sequenceNumber);
    uint32_t ts = ntohl(rtp->timestamp);

    pthread_mutex_lock(&g_state.lock);
    g_state.totalPacketsRecv++;

    // Anchor on first packet
    if (!g_state.anchored) {
        g_state.anchored = true;
        g_state.anchorSeq = seq;
        g_state.anchorLocalUs = nowUs + ((uint64_t)RECOVERY_WINDOW_MS + PCM_TARGET_QUEUE_MS) * 1000;
        g_state.nextExpectedSeq = seq;
        g_state.nextExpectedDeadlineUs = g_state.anchorLocalUs;
    }

    // Check if packet is late (past deadline)
    if (seq_less(seq, g_state.nextExpectedSeq)) {
        g_state.totalLateDiscarded++;
        pthread_mutex_unlock(&g_state.lock);
        return;
    }

    int idx = seq % BURST_SLOT_CAPACITY;
    AUDIO_SLOT* slot = &g_state.slots[idx];

    if (slot->state != SLOT_EMPTY && slot->seq == seq) {
        // Slot already filled by original packet -> ignore redundant duplicate
        g_state.totalDuplicateIgnored++;
        g_state.winRecvDup++;
    } else {
        // First arrival of this sequence number!
        // Calculate deadline
        int16_t seqDiff = (int16_t)(seq - g_state.anchorSeq);
        uint64_t deadline = g_state.anchorLocalUs + (uint64_t)seqDiff * PACKET_DURATION_MS * 1000;

        slot->seq = seq;
        slot->timestamp = ts;
        slot->deadlineUs = deadline;
        slot->len = (uint16_t)len;

        // Check if this was a delayed duplicate filling a gap!
        // If seq is behind what has arrived or if arrival time is > deadline - 80ms, it's a rescue!
        // More simply: if nowUs - (deadline - RECOVERY_WINDOW_MS * 1000) > 60000 us (60ms), it's a duplicate!
        uint64_t estimatedSendUs = deadline - ((uint64_t)RECOVERY_WINDOW_MS + PCM_TARGET_QUEUE_MS) * 1000;
        if (nowUs > estimatedSendUs + 60000) {
            slot->state = SLOT_RECEIVED_DUPLICATE_RESCUE;
            g_state.totalDuplicateRescued++;
            g_state.winRescued++;
            g_state.winRecvDup++;
        } else {
            slot->state = SLOT_RECEIVED_ORIGINAL;
            g_state.winRecvOrig++;
        }
    }

    pthread_mutex_unlock(&g_state.lock);
}

// Simulated Sound Card Playout Thread
// Ticks every 5ms (PACKET_DURATION_MS)
static void* soundcard_playout_worker(void* arg) {
    (void)arg;
    uint64_t nextTickUs = get_time_us();

    while (g_running) {
        uint64_t nowUs = get_time_us();
        if (nowUs < nextTickUs) {
            usleep(nextTickUs - nowUs);
            nowUs = get_time_us();
        }
        nextTickUs += PACKET_DURATION_MS * 1000;

        pthread_mutex_lock(&g_state.lock);
        if (!g_state.anchored) {
            pthread_mutex_unlock(&g_state.lock);
            continue;
        }

        // Drain finalized frames from Burst Recovery Queue
        while (nowUs >= g_state.nextExpectedDeadlineUs) {
            int idx = g_state.nextExpectedSeq % BURST_SLOT_CAPACITY;
            AUDIO_SLOT* slot = &g_state.slots[idx];

            if (slot->state != SLOT_EMPTY && slot->seq == g_state.nextExpectedSeq) {
                // Frame available (either original or duplicate rescue)
                if (slot->state == SLOT_RECEIVED_DUPLICATE_RESCUE) {
                    // Rescued by duplicate!
                }
                g_state.totalOriginalDelivered++;
                g_state.sdlQueuedMs += PACKET_DURATION_MS;
                slot->state = SLOT_EMPTY;
            } else {
                // Missing past deadline -> PLC synthesized frame!
                g_state.totalPlcFrames++;
                g_state.winPlc++;
                g_state.sdlQueuedMs += PACKET_DURATION_MS; // PLC supplies 5ms synthetic PCM
                slot->state = SLOT_EMPTY;
            }

            g_state.nextExpectedSeq++;
            g_state.nextExpectedDeadlineUs += PACKET_DURATION_MS * 1000;
        }

        // Fake Sound Card Hardware Consumption:
        // Sound card drains 5ms every 5ms if playing
        if (!g_state.soundCardPlaying) {
            if (g_state.sdlQueuedMs >= PCM_TARGET_QUEUE_MS) {
                g_state.soundCardPlaying = true;
            }
        }

        if (g_state.soundCardPlaying) {
            if (g_state.sdlQueuedMs >= PACKET_DURATION_MS) {
                g_state.sdlQueuedMs -= PACKET_DURATION_MS;
                g_state.totalFramesPlayed++;
            } else {
                // Buffer is empty! Sound card starvation / underrun!
                g_state.totalStarvations++;
                g_state.winStarvation++;
                g_state.sdlQueuedMs = 0;
                // Rebuffering
                g_state.soundCardPlaying = false;
            }
        }

        pthread_mutex_unlock(&g_state.lock);
    }
    return NULL;
}

int main(int argc, char** argv) {
    int port = 48000;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.lock, NULL);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    printf("===================================================================\n");
    printf("  iPad Audio BurstGuard Mock Client (iSH - M1 iPad Pro)\n");
    printf("  Listening on UDP port: %d\n", port);
    printf("  Audio Playout Config: Window=%dms, Target SDL Buffer=%dms\n",
           RECOVERY_WINDOW_MS, PCM_TARGET_QUEUE_MS);
    printf("===================================================================\n");

    pthread_t playout_th;
    pthread_create(&playout_th, NULL, soundcard_playout_worker, NULL);

    // Monitoring thread / loop
    uint64_t lastReportUs = get_time_us();

    // Set non-blocking recv with timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 20000; // 20ms
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t rx_buf[2048];

    while (g_running) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(sockfd, rx_buf, sizeof(rx_buf), 0, (struct sockaddr*)&from, &fromlen);
        uint64_t now = get_time_us();

        if (n > 0) {
            ingest_packet(rx_buf, n, now);
        }

        // Report every 1 second
        if (now - lastReportUs >= 1000000ULL) {
            pthread_mutex_lock(&g_state.lock);

            time_t rawtime;
            struct tm* timeinfo;
            char timeStr[16];
            time(&rawtime);
            timeinfo = localtime(&rawtime);
            strftime(timeStr, sizeof(timeStr), "%H:%M:%S", timeinfo);

            const char* statusStr;
            if (g_state.winStarvation > 0) {
                statusStr = "\033[1;31m[声卡欠载! 卡顿产生]\033[0m";
            } else if (g_state.winPlc > 0) {
                statusStr = "\033[1;33m[物理丢包 (PLC插值)]\033[0m";
            } else if (g_state.soundCardPlaying) {
                statusStr = "\033[1;32m[声卡连续播放: 正常 ✓]\033[0m";
            } else {
                statusStr = "\033[1;34m[等待音频包缓冲...]\033[0m";
            }

            printf("[%s] 收包: %-4llu (原: %-3u, 冗余: %-3u, 挽救: %-2u) | 假声卡水位: %2u ms | PLC: %-3llu | 欠载: %-2llu -> %s\n",
                   timeStr,
                   (unsigned long long)g_state.totalPacketsRecv,
                   g_state.winRecvOrig,
                   g_state.winRecvDup,
                   g_state.winRescued,
                   g_state.sdlQueuedMs,
                   (unsigned long long)g_state.totalPlcFrames,
                   (unsigned long long)g_state.totalStarvations,
                   statusStr);
            fflush(stdout);

            g_state.winRecvOrig = 0;
            g_state.winRecvDup = 0;
            g_state.winRescued = 0;
            g_state.winPlc = 0;
            g_state.winStarvation = 0;
            lastReportUs = now;

            pthread_mutex_unlock(&g_state.lock);
        }
    }

    close(sockfd);
    pthread_join(playout_th, NULL);
    return 0;
}

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
#define PCM_TARGET_QUEUE_MS 20
#define BURST_SLOT_CAPACITY 512
#define RX_RING 65536

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
    uint64_t anchorFirstArrivalUs;
    uint16_t nextExpectedSeq;
    uint64_t nextExpectedDeadlineUs;

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
    uint64_t totalFirstArrivals;
    uint16_t maxSeenSeq;

    uint32_t winRecvOrig;
    uint32_t winRecvDup;
    uint32_t winRescued;
    uint32_t winPlc;
    uint32_t winStarvation;

    // Arrival-lateness instrumentation (first arrival of each seq)
    uint8_t  rxFlags[RX_RING];      // bit0: seen
    uint16_t rxLateMs[RX_RING];     // first-arrival lateness in ms (capped)
    uint64_t latenessBucket[12];
    uint64_t lateBeyondWindow;      // arrived after deadline (buffer-only absorbable)

    pthread_mutex_t lock;
} CLIENT_STATE;

static CLIENT_STATE g_state;
static volatile bool g_running = true;
static uint32_t g_windowMs = 150;

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

static const uint32_t LATE_BUCKET_EDGES_MS[11] = {5, 25, 50, 75, 100, 150, 200, 300, 400, 500, 700};
static const char* LATE_BUCKET_NAMES[12] = {
    "<=5 (on-time)", "<=25", "<=50", "<=75", "<=100", "<=150",
    "<=200", "<=300", "<=400", "<=500", "<=700", ">700"
};

static void ingest_packet(const uint8_t* buf, size_t len, uint64_t nowUs) {
    if (len < sizeof(RTP_PACKET)) return;
    const RTP_PACKET* rtp = (const RTP_PACKET*)buf;
    if (rtp->packetType != 97) return;

    uint16_t seq = ntohs(rtp->sequenceNumber);
    uint32_t ts = ntohl(rtp->timestamp);

    pthread_mutex_lock(&g_state.lock);
    g_state.totalPacketsRecv++;

    if (!g_state.anchored) {
        g_state.anchored = true;
        g_state.anchorSeq = seq;
        g_state.anchorFirstArrivalUs = nowUs;
        g_state.anchorLocalUs = nowUs + ((uint64_t)g_windowMs + PCM_TARGET_QUEUE_MS) * 1000;
        g_state.nextExpectedSeq = seq;
        g_state.nextExpectedDeadlineUs = g_state.anchorLocalUs;
    }

    if (seq_less(seq, g_state.nextExpectedSeq)) {
        g_state.totalLateDiscarded++;
        pthread_mutex_unlock(&g_state.lock);
        return;
    }

    int idx = seq % BURST_SLOT_CAPACITY;
    AUDIO_SLOT* slot = &g_state.slots[idx];

    if (slot->state != SLOT_EMPTY && slot->seq == seq) {
        g_state.totalDuplicateIgnored++;
        g_state.winRecvDup++;
    } else {
        int16_t seqDiff = (int16_t)(seq - g_state.anchorSeq);
        uint64_t deadline = g_state.anchorLocalUs + (uint64_t)seqDiff * PACKET_DURATION_MS * 1000;
        uint64_t nominalSendUs = deadline - ((uint64_t)g_windowMs + PCM_TARGET_QUEUE_MS) * 1000;

        slot->seq = seq;
        slot->timestamp = ts;
        slot->deadlineUs = deadline;
        slot->len = (uint16_t)len;

        // ---- lateness instrumentation (first arrival only) ----
        uint64_t now2 = get_time_us();
        uint64_t lateUs = (now2 > nominalSendUs) ? (now2 - nominalSendUs) : 0;
        uint32_t lateMs = (lateUs > 60000000ULL) ? 60000 : (uint32_t)(lateUs / 1000);
        uint32_t ring = seq % RX_RING;
        if (!(g_state.rxFlags[ring] & 1)) {
            g_state.rxFlags[ring] |= 1;
            g_state.rxLateMs[ring] = (lateMs > 60000) ? 60000 : (uint16_t)lateMs;
            g_state.totalFirstArrivals++;
            if (g_state.totalFirstArrivals == 1 || seq_greater(seq, g_state.maxSeenSeq)) {
                g_state.maxSeenSeq = seq;
            }
            int b = 11;
            for (int i = 0; i < 11; i++) {
                if (lateMs <= LATE_BUCKET_EDGES_MS[i]) { b = i; break; }
            }
            g_state.latenessBucket[b]++;
            if (lateUs > ((uint64_t)g_windowMs + PCM_TARGET_QUEUE_MS) * 1000) {
                g_state.lateBeyondWindow++;
            }
        }

        uint64_t estimatedSendUs = deadline - ((uint64_t)g_windowMs + PCM_TARGET_QUEUE_MS) * 1000;
        if (now2 > estimatedSendUs + 60000) {
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

        while (nowUs >= g_state.nextExpectedDeadlineUs) {
            int idx = g_state.nextExpectedSeq % BURST_SLOT_CAPACITY;
            AUDIO_SLOT* slot = &g_state.slots[idx];

            if (slot->state != SLOT_EMPTY && slot->seq == g_state.nextExpectedSeq) {
                g_state.totalOriginalDelivered++;
                g_state.sdlQueuedMs += PACKET_DURATION_MS;
                slot->state = SLOT_EMPTY;
            } else {
                g_state.totalPlcFrames++;
                g_state.winPlc++;
                g_state.sdlQueuedMs += PACKET_DURATION_MS;
                slot->state = SLOT_EMPTY;
            }

            g_state.nextExpectedSeq++;
            g_state.nextExpectedDeadlineUs += PACKET_DURATION_MS * 1000;
        }

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
                g_state.totalStarvations++;
                g_state.winStarvation++;
                g_state.sdlQueuedMs = 0;
                g_state.soundCardPlaying = false;
            }
        }

        pthread_mutex_unlock(&g_state.lock);
    }
    return NULL;
}

int main(int argc, char** argv) {
    int port = 48000;
    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) g_windowMs = (uint32_t)atoi(argv[2]);
    uint32_t runSeconds = 0;
    if (argc > 3) runSeconds = (uint32_t)atoi(argv[3]);

    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.lock, NULL);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

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
    printf("  iPad Audio BurstGuard Mock Client v2 (arrival-lateness)\n");
    printf("  UDP port: %d | Window=%ums (+%dms pcm) | Run=%us\n",
           port, g_windowMs, PCM_TARGET_QUEUE_MS, runSeconds);
    printf("===================================================================\n");
    fflush(stdout);

    pthread_t playout_th;
    pthread_create(&playout_th, NULL, soundcard_playout_worker, NULL);

    uint64_t lastReportUs = get_time_us();
    uint64_t stopUs = runSeconds ? (get_time_us() + (uint64_t)runSeconds * 1000000ULL) : 0;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 20000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t rx_buf[2048];

    while (g_running) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(sockfd, rx_buf, sizeof(rx_buf), 0, (struct sockaddr*)&from, &fromlen);
        uint64_t now = get_time_us();

        if (n > 0) ingest_packet(rx_buf, n, now);

        if (stopUs && now >= stopUs) break;

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

            printf("[%s] 收包: %-5llu (原: %-3u, 冗余: %-3u, 挽救: %-2u) | 水位: %2u ms | PLC: %-4llu | 欠载: %-2llu | 迟到出窗: %-4llu -> %s\n",
                   timeStr,
                   (unsigned long long)g_state.totalPacketsRecv,
                   g_state.winRecvOrig,
                   g_state.winRecvDup,
                   g_state.winRescued,
                   g_state.sdlQueuedMs,
                   (unsigned long long)g_state.totalPlcFrames,
                   (unsigned long long)g_state.totalStarvations,
                   (unsigned long long)g_state.lateBeyondWindow,
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

    // ---- final summary ----
    g_running = false;
    usleep(600000); // let playout drain near-deadline frames

    pthread_mutex_lock(&g_state.lock);
    printf("\n==================== FINAL SUMMARY ====================\n");
    printf("Window=%ums(+%dms) | run=%us | wire pkts=%llu | first-arrivals=%llu\n",
           g_windowMs, PCM_TARGET_QUEUE_MS, runSeconds,
           (unsigned long long)g_state.totalPacketsRecv,
           (unsigned long long)g_state.totalFirstArrivals);

    uint64_t span = 0, neverArrived = 0;
    if (g_state.anchored) {
        uint16_t first = g_state.anchorSeq;
        uint16_t last = g_state.maxSeenSeq;
        span = (uint32_t)(last - first) + 1;
        for (uint32_t s = 0; s < span && s < RX_RING; s++) {
            uint16_t seq = (uint16_t)(first + s);
            if (!(g_state.rxFlags[seq % RX_RING] & 1)) neverArrived++;
        }
    }
    printf("expected frames=%llu | never arrived=%llu (%.2f%%)\n",
           (unsigned long long)span,
           (unsigned long long)neverArrived,
           span ? (neverArrived * 100.0 / span) : 0.0);
    printf("delivered at deadline=%llu | PLC synthesized=%llu (%.2f%%)\n",
           (unsigned long long)g_state.totalOriginalDelivered,
           (unsigned long long)g_state.totalPlcFrames,
           span ? (g_state.totalPlcFrames * 100.0 / span) : 0.0);
    printf("starvations=%llu\n", (unsigned long long)g_state.totalStarvations);

    printf("\n--- first-arrival lateness histogram (n=%llu) ---\n",
           (unsigned long long)g_state.totalFirstArrivals);
    for (int i = 0; i < 12; i++) {
        double pct = g_state.totalFirstArrivals ? (g_state.latenessBucket[i] * 100.0 / g_state.totalFirstArrivals) : 0.0;
        printf("  %-14s %8llu  %6.2f%%\n", LATE_BUCKET_NAMES[i],
               (unsigned long long)g_state.latenessBucket[i], pct);
    }
    printf("late beyond deadline (>=%ums): %llu (%.2f%%)  <- absorbable ONLY by bigger buffer\n",
           g_windowMs + PCM_TARGET_QUEUE_MS,
           (unsigned long long)g_state.lateBeyondWindow,
           g_state.totalFirstArrivals ? (g_state.lateBeyondWindow * 100.0 / g_state.totalFirstArrivals) : 0.0);
    printf("========================================================\n");
    fflush(stdout);
    pthread_mutex_unlock(&g_state.lock);

    close(sockfd);
    pthread_join(playout_th, NULL);
    return 0;
}

/* loadgen - fast GTP-U load generator and latency meter for gtpu-fwd.
 *
 * A sender thread transmits G-PDUs carrying a CLOCK_MONOTONIC timestamp; a
 * receiver thread collects the forwarded packets (the route must point back to
 * --listen) and computes loss, throughput and latency percentiles.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static struct {
    const char *fwd_ip; unsigned fwd_port, listen_port;
    uint32_t teid, expect_teid;
    unsigned long packets; unsigned size; unsigned long rate;
} cfg = { "127.0.0.1", 2152, 9000, 1000, 2000, 200000, 128, 0 };

static volatile int sending_done = 0;
static uint64_t *lat_ns;
static unsigned long got = 0, wrong_teid = 0;
static uint64_t first_rx = 0, last_rx = 0;
static int rx_fd;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void *receiver(void *arg)
{
    (void)arg;
    enum { B = 64 };
    static uint8_t bufs[B][2048];
    struct iovec iov[B];
    struct mmsghdr m[B];
    for (int i = 0; i < B; i++) {
        iov[i].iov_base = bufs[i]; iov[i].iov_len = sizeof bufs[i];
        memset(&m[i], 0, sizeof m[i]);
        m[i].msg_hdr.msg_iov = &iov[i]; m[i].msg_hdr.msg_iovlen = 1;
    }
    int idle = 0;
    while (!(sending_done && idle >= 3)) {
        int n = recvmmsg(rx_fd, m, B, MSG_WAITFORONE, NULL);
        uint64_t t = now_ns();
        if (n <= 0) { if (sending_done) idle++; continue; }
        idle = 0;
        if (!first_rx) first_rx = t;
        last_rx = t;
        for (int i = 0; i < n; i++) {
            uint8_t *p = bufs[i];
            if (m[i].msg_len < 8 + 8) continue;
            uint32_t teid = ((uint32_t)p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
            if (teid != cfg.expect_teid) { wrong_teid++; continue; }
            uint64_t sent; memcpy(&sent, p + 8, 8);
            if (got < cfg.packets) lat_ns[got] = t - sent;
            got++;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int o;
    while ((o = getopt(argc, argv, "f:p:l:t:e:n:s:r:")) != -1) {
        switch (o) {
        case 'f': cfg.fwd_ip = optarg; break;
        case 'p': cfg.fwd_port = (unsigned)atoi(optarg); break;
        case 'l': cfg.listen_port = (unsigned)atoi(optarg); break;
        case 't': cfg.teid = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'e': cfg.expect_teid = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'n': cfg.packets = strtoul(optarg, NULL, 10); break;
        case 's': cfg.size = (unsigned)atoi(optarg); break;
        case 'r': cfg.rate = strtoul(optarg, NULL, 10); break;
        default:
            fprintf(stderr, "usage: %s [-f ip] [-p fwd_port] [-l listen_port] [-t teid] "
                    "[-e expect_teid] [-n packets] [-s payload_bytes] [-r pps(0=max)]\n", argv[0]);
            return 2;
        }
    }
    if (cfg.size < 16 || cfg.size > 1400) { fprintf(stderr, "size must be 16..1400\n"); return 2; }

    lat_ns = malloc(cfg.packets * sizeof *lat_ns);
    if (!lat_ns) return 1;

    rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int rb = 16 * 1024 * 1024;
    setsockopt(rx_fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons((uint16_t)cfg.listen_port) };
    inet_pton(AF_INET, "127.0.0.1", &me.sin_addr);
    if (bind(rx_fd, (struct sockaddr *)&me, sizeof me) < 0) { perror("bind"); return 1; }
    struct timeval tv = { 0, 200000 };
    setsockopt(rx_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons((uint16_t)cfg.fwd_port) };
    inet_pton(AF_INET, cfg.fwd_ip, &dst.sin_addr);

    pthread_t th;
    pthread_create(&th, NULL, receiver, NULL);

    uint8_t pkt[2048];
    unsigned plen = cfg.size;
    pkt[0] = 0x30; pkt[1] = 0xFF; pkt[2] = (uint8_t)(plen >> 8); pkt[3] = (uint8_t)plen;
    pkt[4] = (uint8_t)(cfg.teid >> 24); pkt[5] = (uint8_t)(cfg.teid >> 16);
    pkt[6] = (uint8_t)(cfg.teid >> 8);  pkt[7] = (uint8_t)cfg.teid;
    memset(pkt + 16, 0xAB, plen - 8);

    uint64_t start = now_ns(), step = cfg.rate ? 1000000000ull / cfg.rate : 0, next = start;
    for (unsigned long i = 0; i < cfg.packets; i++) {
        if (step) { next += step; while (now_ns() < next) ; }
        uint64_t t = now_ns();
        memcpy(pkt + 8, &t, 8);
        while (sendto(tx, pkt, 8 + plen, 0, (struct sockaddr *)&dst, sizeof dst) < 0 && errno == ENOBUFS) ;
    }
    uint64_t send_end = now_ns();
    sending_done = 1;
    pthread_join(th, NULL);

    unsigned long n = got < cfg.packets ? got : cfg.packets;
    qsort(lat_ns, n, sizeof *lat_ns, cmp_u64);
    double send_s = (send_end - start) / 1e9;
    double rx_s = (last_rx > first_rx) ? (last_rx - first_rx) / 1e9 : send_s;
    printf("sent         : %lu packets (%u byte payload) in %.3f s = %.0f pps offered\n",
           cfg.packets, cfg.size, send_s, cfg.packets / send_s);
    printf("received     : %lu (loss %.2f%%), wrong TEID: %lu\n", got,
           100.0 * (double)(cfg.packets - (got < cfg.packets ? got : cfg.packets)) / cfg.packets, wrong_teid);
    printf("throughput   : %.0f packets/s forwarded\n", got / rx_s);
    if (n) {
        printf("latency (us) : p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f max=%.1f\n",
               lat_ns[n / 2] / 1e3, lat_ns[(size_t)(n * 0.90)] / 1e3,
               lat_ns[(size_t)(n * 0.99)] / 1e3, lat_ns[(size_t)(n * 0.999)] / 1e3,
               lat_ns[n - 1] / 1e3);
    }
    free(lat_ns);
    return 0;
}

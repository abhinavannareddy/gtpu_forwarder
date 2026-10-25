/* gtpu-fwd - user-space GTP-U forwarder (models a packet core user-plane hop).
 *
 * Receives UDP datagrams, validates the GTP-U header, looks up the tunnel
 * endpoint ID (TEID) in a route table, rewrites the TEID and sends the packet
 * to the configured next hop. Invalid or unknown packets are dropped and
 * counted per reason. Echo Requests are answered with Echo Responses.
 *
 * Optionally a control socket (-c) exposes counters and the route table as
 * JSON and allows runtime route changes; see ctl.h.
 */
#define _GNU_SOURCE
#include "ctl.h"
#include "gtpu.h"
#include "routes.h"
#include "stats.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BATCH   32
#define MAX_PKT 2048

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Parses a UDP port; returns 0 for anything that is not 1..65535. */
static unsigned parse_port(const char *s)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    return (*s && !*end && v >= 1 && v <= 65535) ? (unsigned)v : 0;
}

static int bind_udp(const char *ip, unsigned port, int rcvbuf)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    if (rcvbuf)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &local.sin_addr) != 1) {
        fprintf(stderr, "error: bad bind address %s\n", ip);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&local, sizeof local) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    return fd;
}

static void print_stats(const stats_t *s, FILE *out)
{
    fprintf(out, "rx=%llu forwarded=%llu echo_replied=%llu "
            "drop_no_route=%llu drop_unsupported_type=%llu send_errors=%llu",
            (unsigned long long)s->rx, (unsigned long long)s->forwarded,
            (unsigned long long)s->echo_replied,
            (unsigned long long)s->drop_no_route,
            (unsigned long long)s->drop_unsupported_type,
            (unsigned long long)s->send_errors);
    for (int i = 1; i < GTPU_ERR_COUNT; i++)
        fprintf(out, " drop_%s=%llu", gtpu_status_str((gtpu_status)i),
                (unsigned long long)s->drop_parse[i]);
    fputc('\n', out);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s -r routes.conf [-b bind_ip] [-p port] [-s stats_secs] [-c ctl_port] [-C ctl_ip]\n"
        "  -r  route file (in_teid out_teid ip:port per line)\n"
        "  -b  bind address (default 0.0.0.0)\n"
        "  -p  UDP port (default 2152, the registered GTP-U port)\n"
        "  -s  print statistics every N seconds (default: only at exit)\n"
        "  -c  control UDP port (default: disabled), protocol in src/ctl.h\n"
        "  -C  control bind address (default 127.0.0.1; the protocol is unauthenticated)\n",
        prog);
}

/* Answers one pending control request, if any. */
static void serve_control(int ctl_fd, stats_t *st, route_table *rt, double t_start)
{
    static char req[8192], reply[65000];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t rn = recvfrom(ctl_fd, req, sizeof req, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (rn < 0)
        return;
    ctl_ctx cc = { st, rt, now_s() - t_start };
    size_t rl = ctl_handle(&cc, req, (size_t)rn, reply, sizeof reply);
    sendto(ctl_fd, reply, rl, 0, (struct sockaddr *)&from, fl);
}

int main(int argc, char **argv)
{
    const char *route_file = NULL, *bind_ip = "0.0.0.0", *ctl_ip = "127.0.0.1";
    unsigned port = 2152;
    unsigned stats_secs = 0, ctl_port = 0;
    int opt;
    while ((opt = getopt(argc, argv, "r:b:p:s:c:C:h")) != -1) {
        switch (opt) {
        case 'r': route_file = optarg; break;
        case 'b': bind_ip = optarg; break;
        case 'p': port = parse_port(optarg); break;
        case 's': stats_secs = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'c':
            ctl_port = parse_port(optarg);
            if (!ctl_port) { usage(argv[0]); return 2; }
            break;
        case 'C': ctl_ip = optarg; break;
        default:  usage(argv[0]); return 2;
        }
    }
    if (!route_file || port == 0) { usage(argv[0]); return 2; }

    route_table rt;
    if (routes_init(&rt, 1024) != 0) { perror("routes_init"); return 1; }
    char err[200];
    int nroutes = routes_load(&rt, route_file, err, sizeof err);
    if (nroutes < 0) { fprintf(stderr, "error: %s\n", err); routes_free(&rt); return 1; }

    int fd = bind_udp(bind_ip, port, 4 * 1024 * 1024);
    if (fd < 0) { routes_free(&rt); return 1; }
    int ctl_fd = -1;
    if (ctl_port) {
        ctl_fd = bind_udp(ctl_ip, ctl_port, 0);
        if (ctl_fd < 0) { close(fd); routes_free(&rt); return 1; }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;           /* no SA_RESTART: poll wakes up */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    static uint8_t bufs[BATCH][MAX_PKT];
    struct iovec iov[BATCH];
    struct sockaddr_in peers[BATCH];
    struct mmsghdr msgs[BATCH];
    for (int i = 0; i < BATCH; i++) {
        iov[i].iov_base = bufs[i];
        iov[i].iov_len = MAX_PKT;
        memset(&msgs[i], 0, sizeof msgs[i]);
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &peers[i];
        msgs[i].msg_hdr.msg_namelen = sizeof peers[i];
    }

    stats_t st;
    memset(&st, 0, sizeof st);
    fprintf(stderr, "gtpu-fwd: listening on %s:%u with %d routes\n", bind_ip, port, nroutes);
    if (ctl_fd >= 0)
        fprintf(stderr, "gtpu-fwd: control socket on %s:%u\n", ctl_ip, ctl_port);
    const double t_start = now_s();
    time_t next_print = stats_secs ? time(NULL) + stats_secs : 0;
    int busy = 0;                        /* last batch was full: skip poll() */

    while (!g_stop) {
        if (!busy) {
            /* Wake at least every 200 ms so signals and the stats timer are noticed. */
            struct pollfd pfd[2] = { { fd, POLLIN, 0 }, { ctl_fd, POLLIN, 0 } };
            int pr = poll(pfd, ctl_fd >= 0 ? 2 : 1, 200);
            if (pr < 0 && errno != EINTR) { perror("poll"); break; }
            if (pr > 0 && ctl_fd >= 0 && (pfd[1].revents & POLLIN))
                serve_control(ctl_fd, &st, &rt, t_start);
            if (pr <= 0 || !(pfd[0].revents & POLLIN)) goto tick;
        }
        int n = recvmmsg(fd, msgs, BATCH, MSG_DONTWAIT, NULL);
        if (n < 0) {
            busy = 0;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) goto tick;
            perror("recvmmsg");
            break;
        }
        busy = (n == BATCH);
        if (busy && ctl_fd >= 0)
            serve_control(ctl_fd, &st, &rt, t_start);   /* stay responsive under load */

        for (int i = 0; i < n; i++) {
            uint8_t *pkt = bufs[i];
            size_t len = msgs[i].msg_len;
            st.rx++;
            st.bytes_rx += len;

            gtpu_info info;
            gtpu_status s = gtpu_parse(pkt, len, &info);
            if (s != GTPU_OK) { st.drop_parse[s]++; continue; }

            if (info.msg_type == GTPU_MSG_ECHO_REQUEST) {
                uint8_t resp[16];
                size_t rl = gtpu_make_echo_response(pkt, len, resp, sizeof resp);
                if (rl && sendto(fd, resp, rl, 0, (struct sockaddr *)&peers[i],
                                 sizeof peers[i]) == (ssize_t)rl)
                    st.echo_replied++;
                else
                    st.send_errors++;
                continue;
            }
            if (info.msg_type != GTPU_MSG_GPDU) { st.drop_unsupported_type++; continue; }

            route_entry *r = routes_lookup(&rt, info.teid);
            if (!r) { st.drop_no_route++; continue; }

            gtpu_set_teid(pkt, r->out_teid);
            if (sendto(fd, pkt, len, 0, (const struct sockaddr *)&r->next_hop,
                       sizeof r->next_hop) == (ssize_t)len) {
                st.forwarded++;
                st.bytes_forwarded += len;
                r->pkts++;
                r->bytes += len;
            } else {
                st.send_errors++;
            }
        }
    tick:
        if (next_print && time(NULL) >= next_print) {
            print_stats(&st, stderr);
            next_print = time(NULL) + stats_secs;
        }
    }

    print_stats(&st, stdout);
    close(fd);
    if (ctl_fd >= 0) close(ctl_fd);
    routes_free(&rt);
    return 0;
}

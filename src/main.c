/* gtpu-fwd - user-space GTP-U forwarder (models a packet core user-plane hop).
 *
 * Receives UDP datagrams, validates the GTP-U header, looks up the tunnel
 * endpoint ID (TEID) in a route table, rewrites the TEID and sends the packet
 * to the configured next hop (IPv4 or IPv6). Invalid or unknown packets are
 * dropped and counted per reason. Echo Requests are answered with Echo
 * Responses; unknown TEIDs optionally trigger an Error Indication (-e).
 *
 * Threading: -w N starts N workers, each with its own SO_REUSEPORT socket so
 * the kernel spreads flows across them (one flow always lands on one worker,
 * so packet order within a flow is preserved). The main thread serves the
 * optional control socket, the stats timer and shutdown.
 */
#define _GNU_SOURCE
#include "ctl.h"
#include "gtpu.h"
#include "routes.h"
#include "stats.h"
#include "version.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BATCH        32
#define MAX_PKT      2048
#define MAX_WORKERS  64
#define ERRIND_PER_SEC 1000          /* per worker: bounds reflection if sources are spoofed */

static _Atomic int g_stop = 0;           /* lock-free atomic: safe in the handler and across threads */
static void on_signal(int sig) { (void)sig; g_stop = 1; }

typedef struct {
    route_table *rt;
    int          strict_inner;
    int          error_ind;
    uint8_t      local_addr[16];     /* this endpoint, for the Error Indication peer address IE */
    size_t       local_len;
} shared_t;

typedef struct {
    shared_t    *sh;
    int          fd;                 /* receive socket (also used for Echo Responses) */
    int          family;             /* family of fd */
    int          fd_alt4, fd_alt6;   /* send-only sockets for next hops of the other family */
    stats_t      st;
    pthread_t    th;
} worker_t;

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

static unsigned parse_uint(const char *s, unsigned max, int *ok)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    *ok = (*s && !*end && v <= max);
    return (unsigned)v;
}

/* Binds a UDP socket to an IPv4 or IPv6 literal. "::" is dual-stack. */
static int bind_udp(const char *ip, unsigned port, int rcvbuf, int reuseport, int *family_out)
{
    struct sockaddr_storage ss;
    socklen_t sl;
    memset(&ss, 0, sizeof ss);
    int family = strchr(ip, ':') ? AF_INET6 : AF_INET;
    if (family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons((uint16_t)port);
        if (inet_pton(AF_INET6, ip, &a->sin6_addr) != 1) goto bad_addr;
        sl = sizeof *a;
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        a->sin_family = AF_INET;
        a->sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, ip, &a->sin_addr) != 1) goto bad_addr;
        sl = sizeof *a;
    }
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    if (family == AF_INET6) {
        int off = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);   /* "::" accepts IPv4 too */
    }
    if (reuseport)
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuseport, sizeof reuseport);
    if (rcvbuf)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    if (bind(fd, (struct sockaddr *)&ss, sl) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    *family_out = family;
    return fd;
bad_addr:
    fprintf(stderr, "error: bad bind address %s\n", ip);
    return -1;
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
    fprintf(out, " error_indications=%llu\n", (unsigned long long)s->error_indications);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "gtpu-fwd " GTPU_FWD_VERSION " - GTP-U user-plane forwarder\n"
        "usage: %s -r routes.conf [options]\n"
        "  -r FILE  route file: in_teid out_teid ipv4:port | [ipv6]:port per line\n"
        "  -b IP    bind address, IPv4 or IPv6 (default 0.0.0.0; \"::\" is dual-stack)\n"
        "  -p PORT  UDP port (default 2152, the registered GTP-U port)\n"
        "  -w N     worker threads with SO_REUSEPORT (default 1, max %d)\n"
        "  -i       strict mode: drop G-PDUs whose inner IPv4/IPv6 header is invalid\n"
        "  -e       answer G-PDUs for unknown TEIDs with an Error Indication (needs -b IP)\n"
        "  -s SECS  print statistics every N seconds (default: only at exit)\n"
        "  -c PORT  control UDP port (default: disabled), protocol in src/ctl.h\n"
        "  -C IP    control bind address (default 127.0.0.1)\n"
        "  -t TOKEN require this token on control requests (or set GTPU_CTL_TOKEN)\n"
        "  -V       print version\n", prog, MAX_WORKERS);
}

static int out_fd(const worker_t *w, int family)
{
    if (family == w->family) return w->fd;
    return family == AF_INET ? w->fd_alt4 : w->fd_alt6;
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    shared_t *sh = w->sh;
    route_table *rt = sh->rt;
    stats_t *st = &w->st;

    static __thread uint8_t bufs[BATCH][MAX_PKT];
    struct iovec iov[BATCH];
    struct sockaddr_storage peers[BATCH];
    struct mmsghdr msgs[BATCH];
    for (int i = 0; i < BATCH; i++) {
        iov[i].iov_base = bufs[i];
        iov[i].iov_len = MAX_PKT;
        memset(&msgs[i], 0, sizeof msgs[i]);
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &peers[i];
    }
    time_t ei_sec = 0;
    unsigned ei_used = 0;
    int busy = 0;                        /* last batch was full: skip poll() */

    while (!g_stop) {
        if (!busy) {
            struct pollfd pfd = { w->fd, POLLIN, 0 };
            int pr = poll(&pfd, 1, 200);  /* wake regularly so shutdown is noticed */
            if (pr <= 0) continue;
        }
        for (int i = 0; i < BATCH; i++)
            msgs[i].msg_hdr.msg_namelen = sizeof peers[i];
        int n = recvmmsg(w->fd, msgs, BATCH, MSG_DONTWAIT, NULL);
        if (n < 0) {
            busy = 0;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("recvmmsg");
            break;
        }
        busy = (n == BATCH);

        pthread_rwlock_rdlock(&rt->lock);        /* one lock round-trip per batch */
        for (int i = 0; i < n; i++) {
            uint8_t *pkt = bufs[i];
            size_t len = msgs[i].msg_len;
            STAT_INC(st->rx);
            STAT_ADD(st->bytes_rx, len);

            gtpu_info info;
            gtpu_status s = gtpu_parse(pkt, len, &info);
            if (s != GTPU_OK) { STAT_INC(st->drop_parse[s]); continue; }

            if (info.msg_type == GTPU_MSG_ECHO_REQUEST) {
                uint8_t resp[16];
                size_t rl = gtpu_make_echo_response(pkt, len, resp, sizeof resp);
                if (rl && sendto(w->fd, resp, rl, 0, (struct sockaddr *)&peers[i],
                                 msgs[i].msg_hdr.msg_namelen) == (ssize_t)rl)
                    STAT_INC(st->echo_replied);
                else
                    STAT_INC(st->send_errors);
                continue;
            }
            if (info.msg_type != GTPU_MSG_GPDU) { STAT_INC(st->drop_unsupported_type); continue; }

            if (sh->strict_inner && !gtpu_inner_ok(pkt + info.header_len, info.payload_len)) {
                STAT_INC(st->drop_parse[GTPU_ERR_INNER]);
                continue;
            }

            route_entry *r = routes_lookup(rt, info.teid);
            if (!r) {
                STAT_INC(st->drop_no_route);
                if (sh->error_ind) {
                    time_t now = time(NULL);
                    if (now != ei_sec) { ei_sec = now; ei_used = 0; }
                    uint8_t ei[40];
                    size_t el = gtpu_make_error_indication(info.teid, sh->local_addr, sh->local_len, ei, sizeof ei);
                    if (ei_used < ERRIND_PER_SEC && el &&
                        sendto(w->fd, ei, el, 0, (struct sockaddr *)&peers[i],
                               msgs[i].msg_hdr.msg_namelen) == (ssize_t)el) {
                        ei_used++;
                        STAT_INC(st->error_indications);
                    }
                }
                continue;
            }

            gtpu_set_teid(pkt, r->out_teid);
            int ofd = out_fd(w, r->next_hop.ss_family);
            if (ofd >= 0 && sendto(ofd, pkt, len, 0, (const struct sockaddr *)&r->next_hop,
                                   r->hop_len) == (ssize_t)len) {
                STAT_INC(st->forwarded);
                STAT_ADD(st->bytes_forwarded, len);
                __atomic_fetch_add(&r->pkts, 1, __ATOMIC_RELAXED);      /* shared between workers */
                __atomic_fetch_add(&r->bytes, len, __ATOMIC_RELAXED);
            } else {
                STAT_INC(st->send_errors);
            }
        }
        pthread_rwlock_unlock(&rt->lock);
    }
    return NULL;
}

static void collect(const worker_t *ws, int n, stats_t *total)
{
    memset(total, 0, sizeof *total);
    for (int i = 0; i < n; i++)
        stats_accumulate(total, &ws[i].st);
}

int main(int argc, char **argv)
{
    const char *route_file = NULL, *bind_ip = "0.0.0.0", *ctl_ip = "127.0.0.1";
    const char *token = getenv("GTPU_CTL_TOKEN");
    unsigned port = 2152, stats_secs = 0, ctl_port = 0, nworkers = 1;
    int strict_inner = 0, error_ind = 0, ok, opt;

    while ((opt = getopt(argc, argv, "r:b:p:w:ies:c:C:t:Vh")) != -1) {
        switch (opt) {
        case 'r': route_file = optarg; break;
        case 'b': bind_ip = optarg; break;
        case 'p': port = parse_port(optarg); break;
        case 'w':
            nworkers = parse_uint(optarg, MAX_WORKERS, &ok);
            if (!ok || nworkers < 1) { usage(argv[0]); return 2; }
            break;
        case 'i': strict_inner = 1; break;
        case 'e': error_ind = 1; break;
        case 's':
            stats_secs = parse_uint(optarg, 86400, &ok);
            if (!ok) { usage(argv[0]); return 2; }
            break;
        case 'c':
            ctl_port = parse_port(optarg);
            if (!ctl_port) { usage(argv[0]); return 2; }
            break;
        case 'C': ctl_ip = optarg; break;
        case 't': token = optarg; break;
        case 'V': puts("gtpu-fwd " GTPU_FWD_VERSION); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (!route_file || port == 0) { usage(argv[0]); return 2; }

    shared_t sh;
    memset(&sh, 0, sizeof sh);
    sh.strict_inner = strict_inner;
    sh.error_ind = error_ind;
    if (error_ind) {                             /* the peer address IE needs a concrete local address */
        unsigned char a[16];
        int v6 = strchr(bind_ip, ':') != NULL;
        if (inet_pton(v6 ? AF_INET6 : AF_INET, bind_ip, a) != 1) {
            fprintf(stderr, "error: bad bind address %s\n", bind_ip);
            return 2;
        }
        sh.local_len = v6 ? 16 : 4;
        memcpy(sh.local_addr, a, sh.local_len);
        int any = 1;
        for (size_t i = 0; i < sh.local_len; i++) any &= (a[i] == 0);
        if (any) {
            fprintf(stderr, "error: -e needs -b with a specific local address, not a wildcard\n");
            return 2;
        }
    }

    route_table rt;
    if (routes_init(&rt, 1024) != 0) { perror("routes_init"); return 1; }
    sh.rt = &rt;
    char err[200];
    int nroutes = routes_load(&rt, route_file, err, sizeof err);
    if (nroutes < 0) { fprintf(stderr, "error: %s\n", err); routes_free(&rt); return 1; }

    static worker_t workers[MAX_WORKERS];
    int started = 0, ctl_fd = -1, ctl_family = 0, rc = 1;
    for (unsigned i = 0; i < nworkers; i++) {
        worker_t *w = &workers[i];
        w->sh = &sh;
        w->fd_alt4 = w->fd_alt6 = -1;
        w->fd = bind_udp(bind_ip, port, 4 * 1024 * 1024, nworkers > 1, &w->family);
        if (w->fd < 0) goto cleanup_sockets;
        if (w->family != AF_INET)  w->fd_alt4 = socket(AF_INET, SOCK_DGRAM, 0);
        if (w->family != AF_INET6) w->fd_alt6 = socket(AF_INET6, SOCK_DGRAM, 0);   /* may be -1 without IPv6 */
        started++;
    }
    if (ctl_port) {
        ctl_fd = bind_udp(ctl_ip, ctl_port, 0, 0, &ctl_family);
        if (ctl_fd < 0) goto cleanup_sockets;
        if (!token || !*token) {
            if (strcmp(ctl_ip, "127.0.0.1") && strcmp(ctl_ip, "::1"))
                fprintf(stderr, "warning: control socket on %s has no token (-t / GTPU_CTL_TOKEN)\n", ctl_ip);
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;           /* no SA_RESTART: poll wakes up */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "gtpu-fwd %s: listening on %s:%u with %d routes, %u worker%s%s%s\n", GTPU_FWD_VERSION,
            bind_ip, port, nroutes, nworkers, nworkers == 1 ? "" : "s",
            strict_inner ? ", strict inner-IP check" : "", error_ind ? ", error indications" : "");
    if (ctl_fd >= 0)
        fprintf(stderr, "gtpu-fwd: control socket on %s:%u%s\n", ctl_ip, ctl_port,
                token && *token ? " (token required)" : "");

    int threads = 0;
    for (unsigned i = 0; i < nworkers; i++) {
        if (pthread_create(&workers[i].th, NULL, worker_main, &workers[i]) != 0) {
            fprintf(stderr, "error: cannot start worker %u\n", i);
            g_stop = 1;
            break;
        }
        threads++;
    }

    const double t_start = now_s();
    time_t next_print = stats_secs ? time(NULL) + stats_secs : 0;
    static char req[8192], reply[65000];
    while (!g_stop) {
        struct pollfd pfd = { ctl_fd, POLLIN, 0 };     /* fd -1 is ignored by poll */
        int pr = poll(&pfd, 1, 200);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            struct sockaddr_storage from;
            socklen_t fl = sizeof from;
            ssize_t rn = recvfrom(ctl_fd, req, sizeof req, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
            if (rn >= 0) {
                stats_t total;
                collect(workers, started, &total);
                ctl_ctx cc = { &total, &rt, now_s() - t_start, (int)nworkers, strict_inner, error_ind, token };
                size_t rl = ctl_handle(&cc, req, (size_t)rn, reply, sizeof reply);
                sendto(ctl_fd, reply, rl, 0, (struct sockaddr *)&from, fl);
            }
        }
        if (next_print && time(NULL) >= next_print) {
            stats_t total;
            collect(workers, started, &total);
            print_stats(&total, stderr);
            next_print = time(NULL) + stats_secs;
        }
    }

    for (int i = 0; i < threads; i++)
        pthread_join(workers[i].th, NULL);
    stats_t total;
    collect(workers, started, &total);
    print_stats(&total, stdout);
    rc = 0;

cleanup_sockets:
    for (int i = 0; i < started; i++) {
        close(workers[i].fd);
        if (workers[i].fd_alt4 >= 0) close(workers[i].fd_alt4);
        if (workers[i].fd_alt6 >= 0) close(workers[i].fd_alt6);
    }
    if (ctl_fd >= 0) close(ctl_fd);
    routes_free(&rt);
    return rc;
}

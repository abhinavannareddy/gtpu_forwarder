#include "ctl.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INSPECT 2048

/* Bounded appender: snprintf that never overruns and remembers truncation. */
typedef struct { char *buf; size_t cap, len; int full; } sb_t;

static void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sb_t *b, const char *fmt, ...)
{
    if (b->full) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) { b->full = 1; return; }
    b->len += (size_t)n;
}

/* snprintf returns the length it wanted; callers need the length it wrote. */
static size_t clamp(int n, size_t cap)
{
    return n < 0 ? 0 : (size_t)n >= cap ? cap - 1 : (size_t)n;
}

static size_t finish(sb_t *b)
{
    if (b->full)                         /* reply too large: replace with an error */
        b->len = clamp(snprintf(b->buf, b->cap, "{\"ok\":false,\"error\":\"reply too large\"}"), b->cap);
    return b->len;
}

static size_t err_reply(char *out, size_t cap, const char *msg)
{
    return clamp(snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", msg), cap);
}

#define U(x) ((unsigned long long)(x))

static void cmd_stats(ctl_ctx *c, sb_t *b)
{
    const stats_t *s = c->stats;
    sb_printf(b, "{\"ok\":true,\"uptime_s\":%.3f,\"routes\":%zu,\"rx\":%llu,\"forwarded\":%llu,"
              "\"echo_replied\":%llu,\"bytes_rx\":%llu,\"bytes_forwarded\":%llu,"
              "\"drop_no_route\":%llu,\"drop_unsupported_type\":%llu,\"send_errors\":%llu,\"drops\":{",
              c->uptime_s, c->routes->count, U(s->rx), U(s->forwarded), U(s->echo_replied),
              U(s->bytes_rx), U(s->bytes_forwarded), U(s->drop_no_route),
              U(s->drop_unsupported_type), U(s->send_errors));
    for (int i = 1; i < GTPU_ERR_COUNT; i++)
        sb_printf(b, "%s\"%s\":%llu", i > 1 ? "," : "", gtpu_status_str((gtpu_status)i),
                  U(s->drop_parse[i]));
    sb_printf(b, "}}");
}

static void route_json(sb_t *b, const route_entry *e)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &e->next_hop.sin_addr, ip, sizeof ip);
    sb_printf(b, "{\"in_teid\":%u,\"out_teid\":%u,\"next_hop\":\"%s:%u\",\"pkts\":%llu,\"bytes\":%llu}",
              e->in_teid, e->out_teid, ip, ntohs(e->next_hop.sin_port), U(e->pkts), U(e->bytes));
}

static void cmd_routes(ctl_ctx *c, const char *args, sb_t *b)
{
    unsigned long off = 0, limit = 200;
    if (args) {
        if (sscanf(args, "%lu %lu", &off, &limit) < 1) { off = 0; limit = 200; }
    }
    if (limit > 200) limit = 200;
    sb_printf(b, "{\"ok\":true,\"total\":%zu,\"offset\":%lu,\"routes\":[", c->routes->count, off);
    size_t cur = 0;
    unsigned long idx = 0, emitted = 0;
    const route_entry *e;
    while ((e = routes_next(c->routes, &cur)) != NULL) {
        if (idx++ < off) continue;
        if (emitted == limit) break;
        if (emitted) sb_printf(b, ",");
        route_json(b, e);
        emitted++;
    }
    sb_printf(b, "]}");
}

static int parse_hop(const char *s, struct sockaddr_in *sa)
{
    char ip[64];
    unsigned port;
    if (sscanf(s, "%63[^:]:%u", ip, &port) != 2 || port == 0 || port > 65535)
        return -1;
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, ip, &sa->sin_addr) == 1 ? 0 : -1;
}

static size_t cmd_add(ctl_ctx *c, const char *args, char *out, size_t cap)
{
    unsigned long in_t, out_t;
    char hop[80];
    if (!args || sscanf(args, "%lu %lu %79s", &in_t, &out_t, hop) != 3 ||
        in_t > 0xFFFFFFFFUL || out_t > 0xFFFFFFFFUL)
        return err_reply(out, cap, "usage: add <in_teid> <out_teid> <ip:port>");
    struct sockaddr_in sa;
    if (parse_hop(hop, &sa) != 0)
        return err_reply(out, cap, "bad next hop, expected ipv4:port");
    if (routes_lookup(c->routes, (uint32_t)in_t))
        return err_reply(out, cap, "TEID already routed");
    if (routes_add(c->routes, (uint32_t)in_t, (uint32_t)out_t, &sa) != 0)
        return err_reply(out, cap, "route table full");
    return (size_t)snprintf(out, cap, "{\"ok\":true}");
}

static size_t cmd_del(ctl_ctx *c, const char *args, char *out, size_t cap)
{
    unsigned long in_t;
    if (!args || sscanf(args, "%lu", &in_t) != 1 || in_t > 0xFFFFFFFFUL)
        return err_reply(out, cap, "usage: del <in_teid>");
    if (routes_remove(c->routes, (uint32_t)in_t) != 0)
        return err_reply(out, cap, "no such route");
    return (size_t)snprintf(out, cap, "{\"ok\":true}");
}

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Returns 0 on success (reply built in b), otherwise the length of an error
 * reply already written to out. */
static size_t cmd_inspect(ctl_ctx *c, const char *args, sb_t *b, char *out, size_t cap)
{
    uint8_t pkt[MAX_INSPECT];
    size_t n = 0;
    int hi = -1;
    for (const char *p = args ? args : ""; *p; p++) {
        if (isspace((unsigned char)*p)) continue;
        int v = hexval((unsigned char)*p);
        if (v < 0) return err_reply(out, cap, "non-hex character");
        if (hi < 0) { hi = v; continue; }
        if (n == sizeof pkt) return err_reply(out, cap, "packet too large");
        pkt[n++] = (uint8_t)(hi << 4 | v);
        hi = -1;
    }
    if (hi >= 0) return err_reply(out, cap, "odd number of hex digits");

    gtpu_info info;
    gtpu_status st = gtpu_parse(pkt, n, &info);
    sb_printf(b, "{\"ok\":true,\"length\":%zu,\"status\":\"%s\"", n, gtpu_status_str(st));
    if (st != GTPU_OK) {
        sb_printf(b, ",\"action\":\"drop\",\"reason\":\"%s\"}", gtpu_status_str(st));
        return 0;
    }
    sb_printf(b, ",\"msg_type\":%u,\"teid\":%u,\"header_len\":%zu,\"payload_len\":%zu,\"has_opt\":%d",
              info.msg_type, info.teid, info.header_len, info.payload_len, info.has_opt);
    if (info.msg_type == GTPU_MSG_ECHO_REQUEST) {
        sb_printf(b, ",\"action\":\"echo_response\"}");
    } else if (info.msg_type != GTPU_MSG_GPDU) {
        sb_printf(b, ",\"action\":\"drop\",\"reason\":\"unsupported_type\"}");
    } else {
        const route_entry *r = routes_lookup(c->routes, info.teid);
        if (!r) {
            sb_printf(b, ",\"action\":\"drop\",\"reason\":\"no_route\"}");
        } else {
            sb_printf(b, ",\"action\":\"forward\",\"route\":");
            route_json(b, r);
            sb_printf(b, "}");
        }
    }
    return 0;
}

size_t ctl_handle(ctl_ctx *c, const char *req, size_t req_len, char *out, size_t cap)
{
    char line[8192];
    if (req_len >= sizeof line)
        return err_reply(out, cap, "request too large");
    memcpy(line, req, req_len);
    line[req_len] = '\0';
    while (req_len && isspace((unsigned char)line[req_len - 1]))
        line[--req_len] = '\0';

    char *args = line;
    while (*args && !isspace((unsigned char)*args)) args++;
    if (*args) { *args++ = '\0'; while (isspace((unsigned char)*args)) args++; }
    if (!*args) args = NULL;

    sb_t b = { out, cap, 0, 0 };
    if (!strcmp(line, "stats"))        cmd_stats(c, &b);
    else if (!strcmp(line, "routes"))  cmd_routes(c, args, &b);
    else if (!strcmp(line, "add"))     return cmd_add(c, args, out, cap);
    else if (!strcmp(line, "del"))     return cmd_del(c, args, out, cap);
    else if (!strcmp(line, "inspect")) {
        size_t r = cmd_inspect(c, args, &b, out, cap);
        if (r) return r;
    }
    else return err_reply(out, cap, "unknown command");
    return finish(&b);
}

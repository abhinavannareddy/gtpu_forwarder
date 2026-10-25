#include "ctl.h"
#include "version.h"
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
    sb_printf(b, "{\"ok\":true,\"version\":\"" GTPU_FWD_VERSION "\",\"workers\":%d,\"strict_inner\":%s,"
              "\"error_indication\":%s,\"error_indications\":%llu,"
              "\"uptime_s\":%.3f,\"routes\":%zu,\"rx\":%llu,\"forwarded\":%llu,"
              "\"echo_replied\":%llu,\"bytes_rx\":%llu,\"bytes_forwarded\":%llu,"
              "\"drop_no_route\":%llu,\"drop_unsupported_type\":%llu,\"send_errors\":%llu,\"drops\":{",
              c->workers, c->strict_inner ? "true" : "false", c->error_ind ? "true" : "false",
              U(s->error_indications), c->uptime_s, c->routes->count, U(s->rx), U(s->forwarded), U(s->echo_replied),
              U(s->bytes_rx), U(s->bytes_forwarded), U(s->drop_no_route),
              U(s->drop_unsupported_type), U(s->send_errors));
    for (int i = 1; i < GTPU_ERR_COUNT; i++)
        sb_printf(b, "%s\"%s\":%llu", i > 1 ? "," : "", gtpu_status_str((gtpu_status)i),
                  U(s->drop_parse[i]));
    sb_printf(b, "}}");
}

static void route_json(sb_t *b, const route_entry *e)
{
    char hop[INET6_ADDRSTRLEN + 10];
    routes_format_hop(&e->next_hop, hop, sizeof hop);
    sb_printf(b, "{\"in_teid\":%u,\"out_teid\":%u,\"next_hop\":\"%s\",\"pkts\":%llu,\"bytes\":%llu}",
              e->in_teid, e->out_teid, hop,
              U(__atomic_load_n(&e->pkts, __ATOMIC_RELAXED)), U(__atomic_load_n(&e->bytes, __ATOMIC_RELAXED)));
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

static size_t cmd_add(ctl_ctx *c, const char *args, char *out, size_t cap)
{
    unsigned long in_t, out_t;
    char hop[80];
    if (!args || sscanf(args, "%lu %lu %79s", &in_t, &out_t, hop) != 3 ||
        in_t > 0xFFFFFFFFUL || out_t > 0xFFFFFFFFUL)
        return err_reply(out, cap, "usage: add <in_teid> <out_teid> <ip:port>");
    struct sockaddr_storage sa;
    socklen_t sl;
    if (routes_parse_hop(hop, &sa, &sl) != 0)
        return err_reply(out, cap, "bad next hop, expected ipv4:port or [ipv6]:port");
    pthread_rwlock_wrlock(&c->routes->lock);
    size_t r;
    if (routes_lookup(c->routes, (uint32_t)in_t))
        r = err_reply(out, cap, "TEID already routed");
    else if (routes_add(c->routes, (uint32_t)in_t, (uint32_t)out_t, &sa, sl) != 0)
        r = err_reply(out, cap, "route table full");
    else
        r = (size_t)snprintf(out, cap, "{\"ok\":true}");
    pthread_rwlock_unlock(&c->routes->lock);
    return r;
}

static size_t cmd_del(ctl_ctx *c, const char *args, char *out, size_t cap)
{
    unsigned long in_t;
    if (!args || sscanf(args, "%lu", &in_t) != 1 || in_t > 0xFFFFFFFFUL)
        return err_reply(out, cap, "usage: del <in_teid>");
    pthread_rwlock_wrlock(&c->routes->lock);
    int rc = routes_remove(c->routes, (uint32_t)in_t);
    pthread_rwlock_unlock(&c->routes->lock);
    if (rc != 0)
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
    } else if (c->strict_inner && !gtpu_inner_ok(pkt + info.header_len, info.payload_len)) {
        sb_printf(b, ",\"action\":\"drop\",\"reason\":\"bad_inner_ip\"}");
    } else {
        const route_entry *r = routes_lookup(c->routes, info.teid);
        if (!r) {
            sb_printf(b, ",\"action\":\"drop\",\"reason\":\"no_route\",\"error_indication\":%s}",
                      c->error_ind ? "true" : "false");
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

    char *cmd = line;
    if (c->token && *c->token) {            /* "<token> <command>" */
        size_t tl = strlen(c->token);
        int ok = req_len > tl && isspace((unsigned char)line[tl]);
        unsigned diff = 0;                  /* constant-time compare over the token length */
        for (size_t i = 0; i < tl; i++)
            diff |= (unsigned char)(i < req_len ? line[i] : 0) ^ (unsigned char)c->token[i];
        if (!ok || diff)
            return err_reply(out, cap, "unauthorized");
        cmd = line + tl;
        while (isspace((unsigned char)*cmd)) cmd++;
    }

    char *args = cmd;
    while (*args && !isspace((unsigned char)*args)) args++;
    if (*args) { *args++ = '\0'; while (isspace((unsigned char)*args)) args++; }
    if (!*args) args = NULL;

    sb_t b = { out, cap, 0, 0 };
    if (!strcmp(cmd, "add"))      return cmd_add(c, args, out, cap);
    if (!strcmp(cmd, "del"))      return cmd_del(c, args, out, cap);

    pthread_rwlock_rdlock(&c->routes->lock);        /* read-only commands */
    size_t early = 0;
    if (!strcmp(cmd, "stats"))        cmd_stats(c, &b);
    else if (!strcmp(cmd, "routes"))  cmd_routes(c, args, &b);
    else if (!strcmp(cmd, "inspect")) early = cmd_inspect(c, args, &b, out, cap);
    else early = err_reply(out, cap, "unknown command");
    pthread_rwlock_unlock(&c->routes->lock);
    return early ? early : finish(&b);
}

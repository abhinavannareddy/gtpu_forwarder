/* ctl.h - text control protocol over a UDP socket (one request per datagram,
 * one JSON reply per datagram). Pure function of its inputs so it can be unit
 * tested without sockets.
 *
 *   stats                       counters and uptime
 *   routes [offset] [limit]     page through the route table
 *   add <in> <out> <ip:port>    add a route at runtime
 *   del <in>                    remove a route
 *   inspect <hex>               dry-run a packet: parse + route lookup, no I/O
 *
 * If a token is configured every request must be "<token> <command>"; anything
 * else is answered with {"ok":false,"error":"unauthorized"}. Next hops are
 * "ipv4:port" or "[ipv6]:port".
 */
#ifndef CTL_H
#define CTL_H

#include "routes.h"
#include "stats.h"
#include <stddef.h>

typedef struct {
    const stats_t *stats;        /* total over all workers, built by the caller */
    route_table   *routes;       /* ctl_handle takes the table lock itself */
    double         uptime_s;
    int            workers;
    int            strict_inner; /* -i: inner IP validation enabled */
    int            error_ind;    /* -e: Error Indications enabled */
    const char    *token;        /* NULL or empty: no authentication */
} ctl_ctx;

/* Handles one request, writes a NUL-terminated JSON reply into out.
 * Returns the reply length (never 0, never more than cap - 1). */
size_t ctl_handle(ctl_ctx *c, const char *req, size_t req_len, char *out, size_t cap);

#endif

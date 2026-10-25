/* routes.h - TEID forwarding table (open addressing hash map).
 *
 * Thread model: the table carries a read-write lock for the worker threads
 * (readers) and the control thread (the only writer). The functions below do
 * not lock; callers hold the lock around lookups and mutations. Per-route
 * counters are updated with relaxed atomics by whoever holds the read lock. */
#ifndef ROUTES_H
#define ROUTES_H

#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

typedef struct {
    int      used;
    uint32_t in_teid;
    uint32_t out_teid;
    struct sockaddr_storage next_hop;   /* AF_INET or AF_INET6 */
    socklen_t hop_len;
    uint64_t pkts;                      /* packets forwarded through this route */
    uint64_t bytes;                     /* GTP-U bytes forwarded through this route */
} route_entry;

typedef struct {
    route_entry *slots;
    size_t       cap;     /* power of two */
    size_t       count;
    pthread_rwlock_t lock;
} route_table;

int  routes_init(route_table *t, size_t min_capacity);
void routes_free(route_table *t);
int  routes_add(route_table *t, uint32_t in_teid, uint32_t out_teid,
                const struct sockaddr_storage *next_hop, socklen_t hop_len);   /* 0 ok, -1 full/dup */
route_entry *routes_lookup(const route_table *t, uint32_t in_teid);
/* Remove a route (backward-shift deletion keeps probe chains intact).
 * Returns 0 if removed, -1 if the TEID was not present. */
int  routes_remove(route_table *t, uint32_t in_teid);
/* Iterate used entries: start with *cursor = 0, returns NULL when done. */
const route_entry *routes_next(const route_table *t, size_t *cursor);

/* "1.2.3.4:2152" or "[2001:db8::1]:2152" -> sockaddr. Returns 0 on success. */
int  routes_parse_hop(const char *text, struct sockaddr_storage *out, socklen_t *out_len);
/* Inverse of routes_parse_hop. */
void routes_format_hop(const struct sockaddr_storage *sa, char *buf, size_t cap);

/* File format, one route per line:  in_teid  out_teid  ip:port   (or [ipv6]:port)
 * Lines starting with '#' and blank lines are ignored.
 * Returns the number of routes loaded, or -1 on error (message in errbuf). */
int  routes_load(route_table *t, const char *path, char *errbuf, size_t errlen);

#endif

/* routes.h - TEID forwarding table (open addressing hash map). */
#ifndef ROUTES_H
#define ROUTES_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int      used;
    uint32_t in_teid;
    uint32_t out_teid;
    struct sockaddr_in next_hop;
} route_entry;

typedef struct {
    route_entry *slots;
    size_t       cap;     /* power of two */
    size_t       count;
} route_table;

int  routes_init(route_table *t, size_t min_capacity);
void routes_free(route_table *t);
int  routes_add(route_table *t, uint32_t in_teid, uint32_t out_teid,
                const struct sockaddr_in *next_hop);   /* 0 ok, -1 full/dup */
const route_entry *routes_lookup(const route_table *t, uint32_t in_teid);

/* File format, one route per line:  in_teid  out_teid  ip:port
 * Lines starting with '#' and blank lines are ignored.
 * Returns the number of routes loaded, or -1 on error (message in errbuf). */
int  routes_load(route_table *t, const char *path, char *errbuf, size_t errlen);

#endif

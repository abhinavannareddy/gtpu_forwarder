#include "routes.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

int routes_init(route_table *t, size_t min_capacity)
{
    size_t cap = 16;
    while (cap < min_capacity * 2)
        cap <<= 1;
    t->slots = calloc(cap, sizeof(route_entry));
    t->cap = cap;
    t->count = 0;
    return t->slots ? 0 : -1;
}

void routes_free(route_table *t)
{
    free(t->slots);
    t->slots = NULL;
    t->cap = t->count = 0;
}

int routes_add(route_table *t, uint32_t in_teid, uint32_t out_teid,
               const struct sockaddr_in *next_hop)
{
    if (t->count * 2 >= t->cap)
        return -1;
    size_t i = hash32(in_teid) & (t->cap - 1);
    while (t->slots[i].used) {
        if (t->slots[i].in_teid == in_teid)
            return -1;                       /* duplicate */
        i = (i + 1) & (t->cap - 1);
    }
    t->slots[i].used = 1;
    t->slots[i].in_teid = in_teid;
    t->slots[i].out_teid = out_teid;
    t->slots[i].next_hop = *next_hop;
    t->count++;
    return 0;
}

const route_entry *routes_lookup(const route_table *t, uint32_t in_teid)
{
    size_t i = hash32(in_teid) & (t->cap - 1);
    while (t->slots[i].used) {
        if (t->slots[i].in_teid == in_teid)
            return &t->slots[i];
        i = (i + 1) & (t->cap - 1);
    }
    return NULL;
}

int routes_load(route_table *t, const char *path, char *errbuf, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(errbuf, errlen, "cannot open %s", path);
        return -1;
    }
    char line[256];
    int n = 0, lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;
        unsigned long in_t, out_t;
        char ip[64];
        unsigned port;
        if (sscanf(p, "%lu %lu %63[^:]:%u", &in_t, &out_t, ip, &port) != 4 ||
            port == 0 || port > 65535 || in_t > 0xFFFFFFFFUL || out_t > 0xFFFFFFFFUL) {
            snprintf(errbuf, errlen, "%s:%d: bad route line", path, lineno);
            fclose(f);
            return -1;
        }
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
            snprintf(errbuf, errlen, "%s:%d: bad IPv4 address", path, lineno);
            fclose(f);
            return -1;
        }
        if (routes_add(t, (uint32_t)in_t, (uint32_t)out_t, &sa) != 0) {
            snprintf(errbuf, errlen, "%s:%d: duplicate TEID or table full", path, lineno);
            fclose(f);
            return -1;
        }
        n++;
    }
    fclose(f);
    return n;
}

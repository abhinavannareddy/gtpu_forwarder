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
    if (!t->slots)
        return -1;
    if (pthread_rwlock_init(&t->lock, NULL) != 0) {
        free(t->slots);
        t->slots = NULL;
        return -1;
    }
    return 0;
}

void routes_free(route_table *t)
{
    if (!t->slots)
        return;
    pthread_rwlock_destroy(&t->lock);
    free(t->slots);
    t->slots = NULL;
    t->cap = t->count = 0;
}

int routes_add(route_table *t, uint32_t in_teid, uint32_t out_teid,
               const struct sockaddr_storage *next_hop, socklen_t hop_len)
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
    t->slots[i].hop_len = hop_len;
    t->count++;
    return 0;
}

route_entry *routes_lookup(const route_table *t, uint32_t in_teid)
{
    size_t i = hash32(in_teid) & (t->cap - 1);
    while (t->slots[i].used) {
        if (t->slots[i].in_teid == in_teid)
            return &t->slots[i];
        i = (i + 1) & (t->cap - 1);
    }
    return NULL;
}

int routes_remove(route_table *t, uint32_t in_teid)
{
    size_t mask = t->cap - 1;
    size_t i = hash32(in_teid) & mask;
    while (t->slots[i].used && t->slots[i].in_teid != in_teid)
        i = (i + 1) & mask;
    if (!t->slots[i].used)
        return -1;
    /* Backward-shift: pull later members of the probe chain into the hole. */
    size_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (!t->slots[j].used)
            break;
        size_t home = hash32(t->slots[j].in_teid) & mask;
        /* Slot j may move to i unless its home lies cyclically in (i, j]. */
        if (((j - home) & mask) >= ((j - i) & mask)) {
            t->slots[i] = t->slots[j];
            i = j;
        }
    }
    memset(&t->slots[i], 0, sizeof t->slots[i]);
    t->count--;
    return 0;
}

const route_entry *routes_next(const route_table *t, size_t *cursor)
{
    while (*cursor < t->cap) {
        const route_entry *e = &t->slots[(*cursor)++];
        if (e->used)
            return e;
    }
    return NULL;
}

int routes_parse_hop(const char *text, struct sockaddr_storage *out, socklen_t *out_len)
{
    char host[64];
    const char *portp;
    int v6 = 0;
    if (text[0] == '[') {                       /* [v6]:port */
        const char *close = strchr(text, ']');
        if (!close || close[1] != ':' || (size_t)(close - text - 1) >= sizeof host)
            return -1;
        memcpy(host, text + 1, (size_t)(close - text - 1));
        host[close - text - 1] = '\0';
        portp = close + 2;
        v6 = 1;
    } else {                                    /* v4:port */
        const char *colon = strrchr(text, ':');
        if (!colon || (size_t)(colon - text) >= sizeof host)
            return -1;
        memcpy(host, text, (size_t)(colon - text));
        host[colon - text] = '\0';
        portp = colon + 1;
    }
    char *end;
    unsigned long port = strtoul(portp, &end, 10);
    if (!*portp || *end || port == 0 || port > 65535)
        return -1;

    memset(out, 0, sizeof *out);
    if (v6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)out;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons((uint16_t)port);
        if (inet_pton(AF_INET6, host, &a->sin6_addr) != 1)
            return -1;
        *out_len = sizeof *a;
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)out;
        a->sin_family = AF_INET;
        a->sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &a->sin_addr) != 1)
            return -1;
        *out_len = sizeof *a;
    }
    return 0;
}

void routes_format_hop(const struct sockaddr_storage *sa, char *buf, size_t cap)
{
    char ip[INET6_ADDRSTRLEN] = "?";
    if (sa->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
        snprintf(buf, cap, "[%s]:%u", ip, ntohs(a->sin6_port));
    } else {
        const struct sockaddr_in *a = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
        snprintf(buf, cap, "%s:%u", ip, ntohs(a->sin_port));
    }
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
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;
        unsigned long in_t, out_t;
        char hop[100];
        if (sscanf(p, "%lu %lu %99s", &in_t, &out_t, hop) != 3 ||
            in_t > 0xFFFFFFFFUL || out_t > 0xFFFFFFFFUL) {
            snprintf(errbuf, errlen, "%s:%d: bad route line", path, lineno);
            fclose(f);
            return -1;
        }
        struct sockaddr_storage sa;
        socklen_t sl;
        if (routes_parse_hop(hop, &sa, &sl) != 0) {
            snprintf(errbuf, errlen, "%s:%d: bad next hop (expected ipv4:port or [ipv6]:port)", path, lineno);
            fclose(f);
            return -1;
        }
        if (routes_add(t, (uint32_t)in_t, (uint32_t)out_t, &sa, sl) != 0) {
            snprintf(errbuf, errlen, "%s:%d: duplicate TEID or table full", path, lineno);
            fclose(f);
            return -1;
        }
        n++;
    }
    fclose(f);
    return n;
}

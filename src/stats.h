/* stats.h - forwarder counters, shared by the data path and the control socket.
 *
 * Each worker thread owns one stats_t and is its only writer. The control
 * thread reads all of them to build a total, so every access goes through
 * relaxed atomics (no torn reads, no locks on the hot path). */
#ifndef STATS_H
#define STATS_H

#include "gtpu.h"
#include <stdint.h>

typedef struct {
    uint64_t rx, forwarded, echo_replied, error_indications;
    uint64_t bytes_rx, bytes_forwarded;
    uint64_t drop_parse[GTPU_ERR_COUNT];
    uint64_t drop_no_route, drop_unsupported_type, send_errors;
} stats_t;

/* Single-writer increment: plain load + atomic store is enough and cheaper than a lock-prefixed add. */
#define STAT_ADD(field, n) \
    __atomic_store_n(&(field), __atomic_load_n(&(field), __ATOMIC_RELAXED) + (uint64_t)(n), __ATOMIC_RELAXED)
#define STAT_INC(field) STAT_ADD(field, 1)
#define STAT_GET(field) __atomic_load_n(&(field), __ATOMIC_RELAXED)

/* dst += src, reading src atomically. */
static inline void stats_accumulate(stats_t *dst, const stats_t *src)
{
    dst->rx += STAT_GET(src->rx);
    dst->forwarded += STAT_GET(src->forwarded);
    dst->echo_replied += STAT_GET(src->echo_replied);
    dst->error_indications += STAT_GET(src->error_indications);
    dst->bytes_rx += STAT_GET(src->bytes_rx);
    dst->bytes_forwarded += STAT_GET(src->bytes_forwarded);
    for (int i = 0; i < GTPU_ERR_COUNT; i++)
        dst->drop_parse[i] += STAT_GET(src->drop_parse[i]);
    dst->drop_no_route += STAT_GET(src->drop_no_route);
    dst->drop_unsupported_type += STAT_GET(src->drop_unsupported_type);
    dst->send_errors += STAT_GET(src->send_errors);
}

#endif

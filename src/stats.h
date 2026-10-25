/* stats.h - forwarder counters, shared by the data path and the control socket. */
#ifndef STATS_H
#define STATS_H

#include "gtpu.h"
#include <stdint.h>

typedef struct {
    uint64_t rx, forwarded, echo_replied;
    uint64_t bytes_rx, bytes_forwarded;
    uint64_t drop_parse[GTPU_ERR_COUNT];
    uint64_t drop_no_route, drop_unsupported_type, send_errors;
} stats_t;

#endif

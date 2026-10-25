/* gtpu.h - GTP-U (3GPP TS 29.281) header parsing and rewriting. */
#ifndef GTPU_H
#define GTPU_H

#include <stddef.h>
#include <stdint.h>

#define GTPU_MSG_ECHO_REQUEST  1
#define GTPU_MSG_ECHO_RESPONSE 2
#define GTPU_MSG_ERROR_INDICATION 26
#define GTPU_MSG_GPDU          255

#define GTPU_MIN_HDR 8   /* flags, type, length, TEID */
#define GTPU_OPT_HDR 4   /* sequence number, N-PDU number, next extension type */

typedef enum {
    GTPU_OK = 0,
    GTPU_ERR_SHORT,        /* datagram shorter than the mandatory header */
    GTPU_ERR_VERSION,      /* version field is not 1 */
    GTPU_ERR_PROTO_TYPE,   /* protocol type bit is not GTP (1) */
    GTPU_ERR_LENGTH,       /* length field does not match datagram size */
    GTPU_ERR_EXT_HDR,      /* malformed extension header chain */
    GTPU_ERR_INNER,        /* strict mode: inner IP header is invalid (set by the caller) */
    GTPU_ERR_COUNT
} gtpu_status;

typedef struct {
    uint8_t  msg_type;
    uint32_t teid;
    int      has_opt;        /* E, S or PN flag set: 4 optional bytes present */
    size_t   header_len;     /* bytes before the inner payload */
    size_t   payload_len;
} gtpu_info;

/* Parse and validate a datagram. Returns GTPU_OK and fills *out on success. */
gtpu_status gtpu_parse(const uint8_t *buf, size_t len, gtpu_info *out);

/* Overwrite the TEID in place (buffer must already be a valid GTP-U packet). */
void gtpu_set_teid(uint8_t *buf, uint32_t teid);

/* Build an Echo Response for a valid Echo Request into out (>= 14 bytes).
 * Returns the number of bytes written. */
size_t gtpu_make_echo_response(const uint8_t *req, size_t req_len,
                               uint8_t *out, size_t out_cap);

/* Validate the inner IP packet carried by a G-PDU: IPv4 (version, header length,
 * total length within the payload, header checksum) or IPv6 (version, payload
 * length within the payload). Returns 1 if plausible, 0 otherwise. */
int gtpu_inner_ok(const uint8_t *p, size_t len);

/* Build an Error Indication (TS 29.281 7.3.1) for a G-PDU received with an
 * unknown TEID: TEID Data I (type 16) plus GTP-U Peer Address (type 133).
 * local_addr is the raw 4 or 16 byte address of this endpoint. Returns the
 * number of bytes written, or 0 if out_cap (needs >= 36) or addr_len is wrong. */
size_t gtpu_make_error_indication(uint32_t teid_data, const uint8_t *local_addr,
                                  size_t addr_len, uint8_t *out, size_t out_cap);

const char *gtpu_status_str(gtpu_status s);

#endif

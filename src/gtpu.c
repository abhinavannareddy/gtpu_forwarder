#include "gtpu.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

gtpu_status gtpu_parse(const uint8_t *buf, size_t len, gtpu_info *out)
{
    if (len < GTPU_MIN_HDR)
        return GTPU_ERR_SHORT;

    uint8_t flags = buf[0];
    if ((flags >> 5) != 1)           /* version occupies the top 3 bits */
        return GTPU_ERR_VERSION;
    if (!(flags & 0x10))             /* PT bit must be 1 for GTP */
        return GTPU_ERR_PROTO_TYPE;

    uint16_t length = (uint16_t)((buf[2] << 8) | buf[3]);
    if ((size_t)length + GTPU_MIN_HDR != len)
        return GTPU_ERR_LENGTH;

    int has_opt = (flags & 0x07) != 0;   /* E, S or PN */
    size_t off = GTPU_MIN_HDR;

    if (has_opt) {
        if (len < off + GTPU_OPT_HDR)
            return GTPU_ERR_SHORT;
        uint8_t next = buf[off + 3];
        off += GTPU_OPT_HDR;
        /* Walk the extension header chain: each is N*4 bytes, first byte = N,
         * last byte = next extension type (0 = end). */
        while (next != 0) {
            if (off >= len)
                return GTPU_ERR_EXT_HDR;
            size_t ext_len = (size_t)buf[off] * 4;
            if (ext_len == 0 || off + ext_len > len)
                return GTPU_ERR_EXT_HDR;
            next = buf[off + ext_len - 1];
            off += ext_len;
        }
    }

    if (out) {
        out->msg_type    = buf[1];
        out->teid        = rd32(buf + 4);
        out->has_opt     = has_opt;
        out->header_len  = off;
        out->payload_len = len - off;
    }
    return GTPU_OK;
}

void gtpu_set_teid(uint8_t *buf, uint32_t teid)
{
    wr32(buf + 4, teid);
}

size_t gtpu_make_echo_response(const uint8_t *req, size_t req_len,
                               uint8_t *out, size_t out_cap)
{
    (void)req_len;
    /* Echo Response: header with S flag, TEID 0, sequence copied from the
     * request when present, plus a Recovery IE (type 14, value 0). */
    if (out_cap < 14)
        return 0;
    uint8_t seq_hi = 0, seq_lo = 0;
    if (req[0] & 0x02) { seq_hi = req[8]; seq_lo = req[9]; }
    out[0] = 0x32;                 /* version 1, PT 1, S flag */
    out[1] = GTPU_MSG_ECHO_RESPONSE;
    out[2] = 0; out[3] = 6;        /* length after the first 8 bytes */
    wr32(out + 4, 0);
    out[8] = seq_hi; out[9] = seq_lo;
    out[10] = 0;                   /* N-PDU number */
    out[11] = 0;                   /* no next extension */
    out[12] = 14;                  /* Recovery IE type */
    out[13] = 0;                   /* restart counter */
    /* length counts bytes after the mandatory 8: 4 optional + 2 IE = 6 */
    return 14;
}

int gtpu_inner_ok(const uint8_t *p, size_t len)
{
    if (len < 1)
        return 0;
    if ((p[0] >> 4) == 4) {
        if (len < 20)
            return 0;
        size_t ihl = (size_t)(p[0] & 0x0F) * 4;
        size_t total = (size_t)((p[2] << 8) | p[3]);
        if (ihl < 20 || ihl > len || total < ihl || total > len)
            return 0;
        uint32_t sum = 0;                       /* valid header sums to 0xFFFF */
        for (size_t i = 0; i < ihl; i += 2)
            sum += (uint32_t)((p[i] << 8) | p[i + 1]);
        while (sum >> 16)
            sum = (sum & 0xFFFF) + (sum >> 16);
        return sum == 0xFFFF;
    }
    if ((p[0] >> 4) == 6) {
        if (len < 40)
            return 0;
        size_t plen = (size_t)((p[4] << 8) | p[5]);
        return plen + 40 <= len;
    }
    return 0;
}

size_t gtpu_make_error_indication(uint32_t teid_data, const uint8_t *local_addr,
                                  size_t addr_len, uint8_t *out, size_t out_cap)
{
    if ((addr_len != 4 && addr_len != 16) || out_cap < 8 + 4 + 5 + 3 + addr_len)
        return 0;
    size_t body = 4 + 5 + 3 + addr_len;         /* optional header + two IEs */
    out[0] = 0x32;                              /* version 1, PT 1, S flag */
    out[1] = GTPU_MSG_ERROR_INDICATION;
    out[2] = (uint8_t)(body >> 8); out[3] = (uint8_t)body;
    wr32(out + 4, 0);                           /* TEID 0: path-level message */
    out[8] = out[9] = out[10] = out[11] = 0;    /* sequence 0, N-PDU 0, no extension */
    out[12] = 16;                               /* TEID Data I */
    wr32(out + 13, teid_data);
    out[17] = 133;                              /* GTP-U Peer Address */
    out[18] = (uint8_t)(addr_len >> 8); out[19] = (uint8_t)addr_len;
    memcpy(out + 20, local_addr, addr_len);
    return 8 + body;
}

const char *gtpu_status_str(gtpu_status s)
{
    switch (s) {
    case GTPU_OK:             return "ok";
    case GTPU_ERR_SHORT:      return "too_short";
    case GTPU_ERR_VERSION:    return "bad_version";
    case GTPU_ERR_PROTO_TYPE: return "bad_protocol_type";
    case GTPU_ERR_LENGTH:     return "length_mismatch";
    case GTPU_ERR_EXT_HDR:    return "bad_extension_header";
    case GTPU_ERR_INNER:      return "bad_inner_ip";
    default:                  return "unknown";
    }
}

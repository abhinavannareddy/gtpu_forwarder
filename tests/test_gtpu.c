/* Unit tests for the GTP-U parser and route table (no framework needed). */
#include "../src/gtpu.h"
#include "../src/routes.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* G-PDU with TEID and a payload of n bytes. */
static size_t make_gpdu(uint8_t *b, uint32_t teid, size_t n)
{
    b[0] = 0x30; b[1] = 0xFF;
    b[2] = (uint8_t)(n >> 8); b[3] = (uint8_t)n;
    b[4] = (uint8_t)(teid >> 24); b[5] = (uint8_t)(teid >> 16);
    b[6] = (uint8_t)(teid >> 8);  b[7] = (uint8_t)teid;
    for (size_t i = 0; i < n; i++) b[8 + i] = (uint8_t)i;
    return 8 + n;
}

static void test_valid(void)
{
    uint8_t b[64]; gtpu_info i;
    size_t len = make_gpdu(b, 0x11223344, 20);
    CHECK(gtpu_parse(b, len, &i) == GTPU_OK);
    CHECK(i.msg_type == GTPU_MSG_GPDU);
    CHECK(i.teid == 0x11223344);
    CHECK(i.header_len == 8 && i.payload_len == 20);
}

static void test_rejects(void)
{
    uint8_t b[64]; gtpu_info i;
    size_t len = make_gpdu(b, 1, 10);
    CHECK(gtpu_parse(b, 7, &i) == GTPU_ERR_SHORT);
    CHECK(gtpu_parse(b, 0, &i) == GTPU_ERR_SHORT);

    b[0] = 0x50;                                   /* version 2 */
    CHECK(gtpu_parse(b, len, &i) == GTPU_ERR_VERSION);
    b[0] = 0x20;                                   /* PT bit clear (GTP') */
    CHECK(gtpu_parse(b, len, &i) == GTPU_ERR_PROTO_TYPE);
    b[0] = 0x30;
    CHECK(gtpu_parse(b, len - 1, &i) == GTPU_ERR_LENGTH);   /* truncated */
    CHECK(gtpu_parse(b, len + 1, &i) == GTPU_ERR_LENGTH);   /* trailing bytes */
}

static void test_optional_and_extension(void)
{
    uint8_t b[64]; gtpu_info i;
    /* S flag: 4 optional bytes, no extension. */
    memset(b, 0, sizeof b);
    b[0] = 0x32; b[1] = 0xFF; b[3] = 4 + 3;
    CHECK(gtpu_parse(b, 8 + 4 + 3, &i) == GTPU_OK);
    CHECK(i.header_len == 12 && i.payload_len == 3);

    /* E flag with one 4-byte extension header, then end of chain. */
    memset(b, 0, sizeof b);
    b[0] = 0x34; b[1] = 0xFF; b[3] = 4 + 4 + 2;
    b[11] = 0x85;          /* next extension type present */
    b[12] = 1;             /* extension length = 1 unit (4 bytes) */
    b[15] = 0;             /* next = none */
    CHECK(gtpu_parse(b, 8 + 4 + 4 + 2, &i) == GTPU_OK);
    CHECK(i.header_len == 16 && i.payload_len == 2);

    /* Extension claims zero length: must be rejected (would loop forever). */
    b[12] = 0;
    CHECK(gtpu_parse(b, 8 + 4 + 4 + 2, &i) == GTPU_ERR_EXT_HDR);
    /* Extension runs past the end of the packet. */
    b[12] = 60;
    CHECK(gtpu_parse(b, 8 + 4 + 4 + 2, &i) == GTPU_ERR_EXT_HDR);
}

static void test_set_teid_and_echo(void)
{
    uint8_t b[64]; gtpu_info i;
    size_t len = make_gpdu(b, 7, 5);
    gtpu_set_teid(b, 0xA1B2C3D4);
    CHECK(gtpu_parse(b, len, &i) == GTPU_OK && i.teid == 0xA1B2C3D4);

    uint8_t req[12] = { 0x32, GTPU_MSG_ECHO_REQUEST, 0, 4, 0,0,0,0, 0x12, 0x34, 0, 0 };
    uint8_t resp[16];
    size_t rl = gtpu_make_echo_response(req, sizeof req, resp, sizeof resp);
    CHECK(rl == 14);
    CHECK(gtpu_parse(resp, rl, &i) == GTPU_OK);
    CHECK(i.msg_type == GTPU_MSG_ECHO_RESPONSE);
    CHECK(resp[8] == 0x12 && resp[9] == 0x34);     /* sequence copied */
    CHECK(gtpu_make_echo_response(req, sizeof req, resp, 8) == 0);
}

static void test_fuzz_never_crashes(void)
{
    /* Deterministic pseudo-random garbage: parser must never read out of bounds
     * (run under ASan/Valgrind in CI). */
    uint32_t x = 12345;
    uint8_t b[128];
    for (int round = 0; round < 20000; round++) {
        size_t len = (x >> 8) % sizeof b;
        for (size_t k = 0; k < len; k++) { x = x * 1664525u + 1013904223u; b[k] = (uint8_t)(x >> 24); }
        if (len >= 1 && (round & 1)) b[0] = (uint8_t)((b[0] & 0x07) | 0x30);  /* plausible flags */
        gtpu_info i;
        (void)gtpu_parse(b, len, &i);
    }
    CHECK(1);
}

static void test_routes(void)
{
    route_table t;
    CHECK(routes_init(&t, 8) == 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    CHECK(routes_add(&t, 100, 200, &a) == 0);
    CHECK(routes_add(&t, 100, 300, &a) == -1);     /* duplicate */
    const route_entry *r = routes_lookup(&t, 100);
    CHECK(r && r->out_teid == 200 && ntohs(r->next_hop.sin_port) == 9000);
    CHECK(routes_lookup(&t, 101) == NULL);
    for (uint32_t k = 1000; k < 1000 + 500; k++) {
        if (routes_add(&t, k, k + 1, &a) != 0) break;
    }
    CHECK(routes_lookup(&t, 1000) != NULL);
    routes_free(&t);
}

int main(void)
{
    test_valid();
    test_rejects();
    test_optional_and_extension();
    test_set_teid_and_echo();
    test_fuzz_never_crashes();
    test_routes();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("all unit tests passed");
    return 0;
}

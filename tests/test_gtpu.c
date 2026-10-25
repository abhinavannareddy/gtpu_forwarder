/* Unit tests for the GTP-U parser and route table (no framework needed). */
#include "../src/gtpu.h"
#include "../src/routes.h"
#include "../src/ctl.h"
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

static struct sockaddr_in hop(unsigned port)
{
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    return a;
}

static void test_routes_remove(void)
{
    /* Removing from a heavily colliding table must keep every other key findable. */
    route_table t;
    CHECK(routes_init(&t, 64) == 0);
    struct sockaddr_in a = hop(9000);
    enum { N = 60 };
    for (uint32_t k = 0; k < N; k++) CHECK(routes_add(&t, k * 7919u, k, &a) == 0);
    for (uint32_t k = 0; k < N; k += 2) CHECK(routes_remove(&t, k * 7919u) == 0);
    CHECK(routes_remove(&t, 123456789u) == -1);
    CHECK(t.count == N / 2);
    for (uint32_t k = 0; k < N; k++) {
        const route_entry *r = routes_lookup(&t, k * 7919u);
        CHECK((k % 2 == 0) ? r == NULL : (r && r->out_teid == k));
    }
    size_t cur = 0, seen = 0;
    while (routes_next(&t, &cur)) seen++;
    CHECK(seen == N / 2);
    CHECK(routes_add(&t, 0, 5, &a) == 0);              /* slot is reusable */
    routes_free(&t);
}

static int contains(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

static void test_ctl(void)
{
    route_table t; routes_init(&t, 16);
    struct sockaddr_in a = hop(9000);
    routes_add(&t, 100, 200, &a);
    stats_t st; memset(&st, 0, sizeof st);
    st.rx = 7; st.forwarded = 5; st.drop_parse[GTPU_ERR_VERSION] = 2;
    ctl_ctx c = { &st, &t, 1.5 };
    char out[4096];
#define CTL(str) ctl_handle(&c, str, strlen(str), out, sizeof out)

    CHECK(CTL("stats") == strlen(out));
    CHECK(contains(out, "\"rx\":7") && contains(out, "\"bad_version\":2") && contains(out, "\"routes\":1"));

    CHECK(CTL("routes") == strlen(out));
    CHECK(contains(out, "\"in_teid\":100") && contains(out, "127.0.0.1:9000"));

    CTL("add 101 201 127.0.0.1:9001");
    CHECK(contains(out, "\"ok\":true") && t.count == 2);
    CTL("add 101 202 127.0.0.1:9001");
    CHECK(contains(out, "\"ok\":false") && contains(out, "already"));
    CTL("add 1 2 not-an-address");
    CHECK(contains(out, "\"ok\":false"));
    CTL("add 4294967296 1 127.0.0.1:1");
    CHECK(contains(out, "\"ok\":false"));
    CTL("del 101");
    CHECK(contains(out, "\"ok\":true") && t.count == 1);
    CTL("del 101");
    CHECK(contains(out, "no such route"));

    /* inspect: forward, no route, echo, parse failure, malformed hex */
    CTL("inspect 30ff0003000000640a0b0c");
    CHECK(contains(out, "\"action\":\"forward\"") && contains(out, "\"out_teid\":200"));
    CTL("inspect 30ff000300000063 0a0b0c");
    CHECK(contains(out, "\"reason\":\"no_route\""));
    CTL("inspect 3201000400000000beef0000");
    CHECK(contains(out, "\"action\":\"echo_response\""));
    CTL("inspect 50ff000300000064 0a0b0c");
    CHECK(contains(out, "\"reason\":\"bad_version\""));
    CTL("inspect 30ff00");
    CHECK(contains(out, "\"reason\":\"too_short\""));
    CTL("inspect 3");
    CHECK(contains(out, "odd number"));
    CTL("inspect zz");
    CHECK(contains(out, "non-hex"));
    CTL("bogus");
    CHECK(contains(out, "unknown command"));
    CTL("");
    CHECK(contains(out, "unknown command"));

    /* Small output buffer must yield a well-formed error, never an overrun. */
    char tiny[40];
    size_t n = ctl_handle(&c, "stats", 5, tiny, sizeof tiny);
    CHECK(n < sizeof tiny && tiny[n] == '\0');
    routes_free(&t);
#undef CTL
}

int main(void)
{
    test_valid();
    test_rejects();
    test_optional_and_extension();
    test_set_teid_and_echo();
    test_fuzz_never_crashes();
    test_routes();
    test_routes_remove();
    test_ctl();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("all unit tests passed");
    return 0;
}

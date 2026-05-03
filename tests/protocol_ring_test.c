#include "protocol.h"
#include "ring.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL:%s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static int test_endian_helpers(void)
{
    uint8_t buf[8];

    opty_put_u16(buf, 0x1234u);
    REQUIRE(opty_get_u16(buf) == 0x1234u);

    opty_put_u64(buf, 0x0123456789abcdefull);
    REQUIRE(opty_get_u64(buf) == 0x0123456789abcdefull);
    return 0;
}

static int test_frame_roundtrip(void)
{
    int sv[2];
    struct opty_frame frame;
    const uint8_t data[] = { 'o', 'p', 't', 'y' };
    uint64_t seq = 0;
    uint64_t base_seq = 0;
    uint64_t next_seq = 0;
    uint16_t cols = 0;
    uint16_t rows = 0;
    char token[16];
    const uint8_t *stream;
    size_t stream_len = 0;

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    REQUIRE(opty_send_hello(sv[0], 120u, 40u, "token") == 0);
    REQUIRE(opty_recv_frame(sv[1], &frame) == 0);
    REQUIRE(frame.type == OPTY_MSG_HELLO);
    REQUIRE(opty_parse_client_intro(&frame, &seq, &cols, &rows, token, sizeof(token)) == 0);
    REQUIRE(seq == 0u);
    REQUIRE(cols == 120u);
    REQUIRE(rows == 40u);
    REQUIRE(strcmp(token, "token") == 0);
    opty_frame_free(&frame);

    REQUIRE(opty_send_reconnect(sv[0], 99u, 80u, 24u, "again") == 0);
    REQUIRE(opty_recv_frame(sv[1], &frame) == 0);
    REQUIRE(frame.type == OPTY_MSG_RECONNECT);
    REQUIRE(opty_parse_client_intro(&frame, &seq, &cols, &rows, token, sizeof(token)) == 0);
    REQUIRE(seq == 99u);
    REQUIRE(cols == 80u);
    REQUIRE(rows == 24u);
    REQUIRE(strcmp(token, "again") == 0);
    opty_frame_free(&frame);

    REQUIRE(opty_send_resize(sv[0], 200u, 60u) == 0);
    REQUIRE(opty_recv_frame(sv[1], &frame) == 0);
    REQUIRE(frame.type == OPTY_MSG_RESIZE);
    REQUIRE(opty_parse_resize(&frame, &cols, &rows) == 0);
    REQUIRE(cols == 200u);
    REQUIRE(rows == 60u);
    opty_frame_free(&frame);

    REQUIRE(opty_send_ack(sv[0], 1234u) == 0);
    REQUIRE(opty_recv_frame(sv[1], &frame) == 0);
    REQUIRE(frame.type == OPTY_MSG_ACK);
    REQUIRE(opty_parse_ack(&frame, &seq) == 0);
    REQUIRE(seq == 1234u);
    opty_frame_free(&frame);

    REQUIRE(opty_send_stdout(sv[0], 55u, data, sizeof(data)) == 0);
    REQUIRE(opty_recv_frame(sv[1], &frame) == 0);
    REQUIRE(frame.type == OPTY_MSG_STDOUT);
    REQUIRE(opty_parse_stream(&frame, &seq, &stream, &stream_len) == 0);
    REQUIRE(seq == 55u);
    REQUIRE(stream_len == sizeof(data));
    REQUIRE(memcmp(stream, data, sizeof(data)) == 0);
    opty_frame_free(&frame);

    REQUIRE(opty_send_welcome(sv[0], 10u, 42u) == 0);
    REQUIRE(opty_recv_frame(sv[1], &frame) == 0);
    REQUIRE(frame.type == OPTY_MSG_WELCOME);
    REQUIRE(opty_parse_welcome(&frame, &base_seq, &next_seq) == 0);
    REQUIRE(base_seq == 10u);
    REQUIRE(next_seq == 42u);
    opty_frame_free(&frame);

    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int test_ring_buffer(void)
{
    struct opty_ring ring;
    uint64_t start_seq = 0;
    uint64_t base = 0;
    uint64_t next = 0;
    uint64_t from = 0;
    size_t len = 0;
    uint8_t *snap;

    REQUIRE(opty_ring_init(&ring, 5u) == 0);

    opty_ring_append(&ring, (const uint8_t *)"abc", 3u, &start_seq);
    REQUIRE(start_seq == 0u);
    opty_ring_bounds(&ring, &base, &next);
    REQUIRE(base == 0u);
    REQUIRE(next == 3u);
    snap = opty_ring_snapshot_from(&ring, 0u, &from, &next, &len);
    REQUIRE(snap != NULL);
    REQUIRE(from == 0u);
    REQUIRE(next == 3u);
    REQUIRE(len == 3u);
    REQUIRE(memcmp(snap, "abc", 3u) == 0);
    free(snap);

    opty_ring_append(&ring, (const uint8_t *)"defghij", 7u, &start_seq);
    REQUIRE(start_seq == 3u);
    opty_ring_bounds(&ring, &base, &next);
    REQUIRE(base == 5u);
    REQUIRE(next == 10u);

    snap = opty_ring_snapshot_from(&ring, 2u, &from, &next, &len);
    REQUIRE(snap != NULL);
    REQUIRE(from == 5u);
    REQUIRE(next == 10u);
    REQUIRE(len == 5u);
    REQUIRE(memcmp(snap, "fghij", 5u) == 0);
    free(snap);

    snap = opty_ring_snapshot_from(&ring, 10u, &from, &next, &len);
    REQUIRE(snap == NULL);
    REQUIRE(from == 10u);
    REQUIRE(next == 10u);
    REQUIRE(len == 0u);

    opty_ring_destroy(&ring);
    return 0;
}

int main(void)
{
    REQUIRE(test_endian_helpers() == 0);
    REQUIRE(test_frame_roundtrip() == 0);
    REQUIRE(test_ring_buffer() == 0);
    return 0;
}

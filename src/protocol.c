#include "protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static int opty_token_payload(const char *token, uint8_t *out, size_t *len_out)
{
    size_t len = 0;

    if (token != NULL) {
        len = strlen(token);
        if (len > OPTY_MAX_TOKEN) {
            errno = EMSGSIZE;
            return -1;
        }
    }

    if (len > 0 && out != NULL) {
        memcpy(out, token, len);
    }

    if (len_out != NULL) {
        *len_out = len;
    }

    return 0;
}

ssize_t opty_read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t total = 0;

    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == 0) {
            if (total == 0) {
                return 0;
            }
            errno = ECONNRESET;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    return (ssize_t)total;
}

ssize_t opty_write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t total = 0;

    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    return (ssize_t)total;
}

static ssize_t opty_writev_full(int fd, const struct iovec *iov, int iovcnt)
{
    struct iovec local[3];
    struct iovec *cur = local;
    int count = iovcnt;
    size_t total = 0;

    if (iovcnt < 0 || iovcnt > (int)(sizeof(local) / sizeof(local[0]))) {
        errno = EINVAL;
        return -1;
    }
    if (iovcnt == 0) {
        return 0;
    }

    memcpy(local, iov, (size_t)iovcnt * sizeof(local[0]));
    while (count > 0) {
        ssize_t n = writev(fd, cur, count);
        if (n > 0) {
            size_t written = (size_t)n;

            total += written;
            while (count > 0 && written >= cur[0].iov_len) {
                written -= cur[0].iov_len;
                cur++;
                count--;
            }
            if (count > 0 && written > 0u) {
                cur[0].iov_base = (uint8_t *)cur[0].iov_base + written;
                cur[0].iov_len -= written;
            }
            continue;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    return (ssize_t)total;
}

int opty_send_frame(int fd, uint8_t type, const void *payload, size_t len)
{
    uint8_t prefix[4];
    uint8_t type_byte = type;
    uint32_t wire_len;
    struct iovec iov[3];
    int iovcnt = 2;

    if (len > (size_t)(OPTY_MAX_FRAME - 1u)) {
        errno = EMSGSIZE;
        return -1;
    }
    if (len > 0 && payload == NULL) {
        errno = EINVAL;
        return -1;
    }

    wire_len = (uint32_t)(len + 1u);
    prefix[0] = (uint8_t)((wire_len >> 24) & 0xffu);
    prefix[1] = (uint8_t)((wire_len >> 16) & 0xffu);
    prefix[2] = (uint8_t)((wire_len >> 8) & 0xffu);
    prefix[3] = (uint8_t)(wire_len & 0xffu);

    iov[0].iov_base = prefix;
    iov[0].iov_len = sizeof(prefix);
    iov[1].iov_base = &type_byte;
    iov[1].iov_len = 1u;
    if (len > 0) {
        iov[2].iov_base = (void *)payload;
        iov[2].iov_len = len;
        iovcnt = 3;
    }

    if (opty_writev_full(fd, iov, iovcnt) < 0) {
        return -1;
    }

    return 0;
}

int opty_recv_frame_limited(int fd, struct opty_frame *frame, size_t max_payload_len)
{
    uint8_t prefix[4];
    uint8_t type;
    uint32_t wire_len;
    size_t payload_len;
    size_t payload_cap = max_payload_len;

    if (frame == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (payload_cap > (size_t)(OPTY_MAX_FRAME - 1u)) {
        payload_cap = (size_t)(OPTY_MAX_FRAME - 1u);
    }

    frame->type = 0;
    frame->len = 0;
    frame->payload = NULL;

    ssize_t n = opty_read_full(fd, prefix, sizeof(prefix));
    if (n == 0) {
        return 1;
    }
    if (n < 0) {
        return -1;
    }

    wire_len = ((uint32_t)prefix[0] << 24) |
               ((uint32_t)prefix[1] << 16) |
               ((uint32_t)prefix[2] << 8) |
               (uint32_t)prefix[3];

    if (wire_len < 1u || wire_len > OPTY_MAX_FRAME || ((size_t)wire_len - 1u) > payload_cap) {
        errno = EMSGSIZE;
        return -1;
    }

    n = opty_read_full(fd, &type, 1u);
    if (n != 1) {
        errno = ECONNRESET;
        return -1;
    }

    payload_len = (size_t)wire_len - 1u;
    if (payload_len > 0) {
        frame->payload = malloc(payload_len);
        if (frame->payload == NULL) {
            errno = ENOMEM;
            return -1;
        }
        n = opty_read_full(fd, frame->payload, payload_len);
        if (n != (ssize_t)payload_len) {
            free(frame->payload);
            frame->payload = NULL;
            errno = ECONNRESET;
            return -1;
        }
    }

    frame->type = type;
    frame->len = payload_len;
    return 0;
}

int opty_recv_frame(int fd, struct opty_frame *frame)
{
    return opty_recv_frame_limited(fd, frame, (size_t)(OPTY_MAX_FRAME - 1u));
}

void opty_frame_free(struct opty_frame *frame)
{
    if (frame == NULL) {
        return;
    }
    free(frame->payload);
    frame->payload = NULL;
    frame->len = 0;
    frame->type = 0;
}

uint16_t opty_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

uint64_t opty_get_u64(const uint8_t *p)
{
    return ((uint64_t)p[0] << 56) |
           ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) |
           ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) |
           ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) |
           (uint64_t)p[7];
}

void opty_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

void opty_put_u64(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)((v >> 56) & 0xffu);
    p[1] = (uint8_t)((v >> 48) & 0xffu);
    p[2] = (uint8_t)((v >> 40) & 0xffu);
    p[3] = (uint8_t)((v >> 32) & 0xffu);
    p[4] = (uint8_t)((v >> 24) & 0xffu);
    p[5] = (uint8_t)((v >> 16) & 0xffu);
    p[6] = (uint8_t)((v >> 8) & 0xffu);
    p[7] = (uint8_t)(v & 0xffu);
}

static int opty_send_tokenized_intro(int fd, uint8_t type, uint64_t last_seq, uint16_t cols, uint16_t rows, const char *token, int with_seq)
{
    uint8_t payload[1u + 8u + 2u + 2u + 1u + OPTY_MAX_TOKEN];
    size_t off = 0;
    size_t token_len = 0;

    payload[off++] = (uint8_t)OPTY_VERSION;
    if (with_seq) {
        opty_put_u64(payload + off, last_seq);
        off += 8u;
    }
    opty_put_u16(payload + off, cols);
    off += 2u;
    opty_put_u16(payload + off, rows);
    off += 2u;

    if (opty_token_payload(token, payload + off + 1u, &token_len) < 0) {
        return -1;
    }
    payload[off++] = (uint8_t)token_len;
    off += token_len;

    return opty_send_frame(fd, type, payload, off);
}

int opty_send_hello(int fd, uint16_t cols, uint16_t rows, const char *token)
{
    return opty_send_tokenized_intro(fd, OPTY_MSG_HELLO, 0u, cols, rows, token, 0);
}

int opty_send_reconnect(int fd, uint64_t last_seq, uint16_t cols, uint16_t rows, const char *token)
{
    return opty_send_tokenized_intro(fd, OPTY_MSG_RECONNECT, last_seq, cols, rows, token, 1);
}

int opty_send_resize(int fd, uint16_t cols, uint16_t rows)
{
    uint8_t payload[4];

    opty_put_u16(payload, cols);
    opty_put_u16(payload + 2, rows);
    return opty_send_frame(fd, OPTY_MSG_RESIZE, payload, sizeof(payload));
}

int opty_send_ack(int fd, uint64_t seq)
{
    uint8_t payload[8];

    opty_put_u64(payload, seq);
    return opty_send_frame(fd, OPTY_MSG_ACK, payload, sizeof(payload));
}

int opty_send_stdin(int fd, const void *data, size_t len)
{
    return opty_send_frame(fd, OPTY_MSG_STDIN, data, len);
}

static int opty_send_stream_frame(int fd, uint8_t type, uint64_t seq, const void *data, size_t len)
{
    uint8_t *payload;
    int rc;

    if (len > 0 && data == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (len > SIZE_MAX - 8u) {
        errno = EMSGSIZE;
        return -1;
    }

    payload = malloc(8u + len);
    if (payload == NULL) {
        errno = ENOMEM;
        return -1;
    }
    opty_put_u64(payload, seq);
    if (len > 0) {
        memcpy(payload + 8u, data, len);
    }
    rc = opty_send_frame(fd, type, payload, 8u + len);
    free(payload);
    return rc;
}

int opty_send_stdout(int fd, uint64_t seq, const void *data, size_t len)
{
    return opty_send_stream_frame(fd, OPTY_MSG_STDOUT, seq, data, len);
}

int opty_send_replay(int fd, uint64_t from_seq, const void *data, size_t len)
{
    return opty_send_stream_frame(fd, OPTY_MSG_REPLAY, from_seq, data, len);
}

int opty_send_welcome(int fd, uint64_t base_seq, uint64_t next_seq)
{
    uint8_t payload[16];

    opty_put_u64(payload, base_seq);
    opty_put_u64(payload + 8, next_seq);
    return opty_send_frame(fd, OPTY_MSG_WELCOME, payload, sizeof(payload));
}

int opty_send_error(int fd, const char *message)
{
    size_t len = 0;

    if (message != NULL) {
        len = strlen(message);
    }
    if (len > (size_t)(OPTY_MAX_FRAME - 1u)) {
        errno = EMSGSIZE;
        return -1;
    }
    return opty_send_frame(fd, OPTY_MSG_ERROR, message, len);
}

int opty_send_control_shutdown(int fd, const char *token, const void *input, size_t input_len)
{
    uint8_t payload[1u + 1u + OPTY_MAX_TOKEN + 2u + OPTY_MAX_GRACEFUL_INPUT];
    size_t off = 0;
    size_t token_len = 0;

    if (input_len > OPTY_MAX_GRACEFUL_INPUT) {
        errno = EMSGSIZE;
        return -1;
    }
    if (input_len > 0u && input == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (opty_token_payload(token, payload + 2u, &token_len) < 0) {
        return -1;
    }

    payload[off++] = (uint8_t)OPTY_VERSION;
    payload[off++] = (uint8_t)token_len;
    off += token_len;
    opty_put_u16(payload + off, (uint16_t)input_len);
    off += 2u;
    if (input_len > 0u) {
        memcpy(payload + off, input, input_len);
        off += input_len;
    }

    return opty_send_frame(fd, OPTY_MSG_CONTROL_SHUTDOWN, payload, off);
}

static int opty_parse_intro_payload(const struct opty_frame *frame, uint64_t *last_seq, uint16_t *cols, uint16_t *rows, char *token, size_t token_cap, int has_seq)
{
    size_t need = has_seq ? 14u : 6u;
    size_t off = 0;
    uint8_t token_len;
    uint64_t seq = 0;

    if (frame == NULL || cols == NULL || rows == NULL || (token_cap > 0 && token == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (frame->len < need) {
        errno = EPROTO;
        return -1;
    }

    if (frame->payload[off++] != (uint8_t)OPTY_VERSION) {
        errno = EPROTO;
        return -1;
    }

    if (has_seq) {
        seq = opty_get_u64(frame->payload + off);
        off += 8u;
    }

    *cols = opty_get_u16(frame->payload + off);
    off += 2u;
    *rows = opty_get_u16(frame->payload + off);
    off += 2u;
    token_len = frame->payload[off++];

    if (frame->len != off + (size_t)token_len) {
        errno = EPROTO;
        return -1;
    }
    if (token_len + 1u > token_cap) {
        if (token_cap == 0u && token_len == 0u) {
            if (last_seq != NULL) {
                *last_seq = seq;
            }
            return 0;
        }
        errno = ENOSPC;
        return -1;
    }

    if (token_len > 0) {
        memcpy(token, frame->payload + off, token_len);
    }
    if (token_cap > 0) {
        token[token_len] = '\0';
    }
    if (last_seq != NULL) {
        *last_seq = seq;
    }
    return 0;
}

int opty_parse_client_intro(const struct opty_frame *frame, uint64_t *last_seq, uint16_t *cols, uint16_t *rows, char *token, size_t token_cap)
{
    if (frame == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (frame->type == OPTY_MSG_HELLO) {
        if (last_seq != NULL) {
            *last_seq = 0u;
        }
        return opty_parse_intro_payload(frame, last_seq, cols, rows, token, token_cap, 0);
    }
    if (frame->type == OPTY_MSG_RECONNECT) {
        return opty_parse_intro_payload(frame, last_seq, cols, rows, token, token_cap, 1);
    }
    errno = EPROTO;
    return -1;
}

int opty_parse_resize(const struct opty_frame *frame, uint16_t *cols, uint16_t *rows)
{
    if (frame == NULL || cols == NULL || rows == NULL || frame->type != OPTY_MSG_RESIZE || frame->len != 4u) {
        errno = EPROTO;
        return -1;
    }
    *cols = opty_get_u16(frame->payload);
    *rows = opty_get_u16(frame->payload + 2u);
    return 0;
}

int opty_parse_ack(const struct opty_frame *frame, uint64_t *seq)
{
    if (frame == NULL || seq == NULL || frame->type != OPTY_MSG_ACK || frame->len != 8u) {
        errno = EPROTO;
        return -1;
    }
    *seq = opty_get_u64(frame->payload);
    return 0;
}

int opty_parse_stream(const struct opty_frame *frame, uint64_t *seq, const uint8_t **data, size_t *len)
{
    if (frame == NULL || seq == NULL || data == NULL || len == NULL) {
        errno = EINVAL;
        return -1;
    }
    if ((frame->type != OPTY_MSG_STDOUT && frame->type != OPTY_MSG_REPLAY) || frame->len < 8u) {
        errno = EPROTO;
        return -1;
    }
    *seq = opty_get_u64(frame->payload);
    *data = frame->payload + 8u;
    *len = frame->len - 8u;
    return 0;
}

int opty_parse_welcome(const struct opty_frame *frame, uint64_t *base_seq, uint64_t *next_seq)
{
    if (frame == NULL || base_seq == NULL || next_seq == NULL || frame->type != OPTY_MSG_WELCOME || frame->len != 16u) {
        errno = EPROTO;
        return -1;
    }
    *base_seq = opty_get_u64(frame->payload);
    *next_seq = opty_get_u64(frame->payload + 8u);
    return 0;
}

int opty_parse_control_shutdown(const struct opty_frame *frame, char *token, size_t token_cap, const uint8_t **input, size_t *input_len)
{
    size_t off = 0;
    uint8_t token_len;
    uint16_t graceful_len;

    if (frame == NULL || token == NULL || token_cap == 0u || input == NULL || input_len == NULL ||
        frame->type != OPTY_MSG_CONTROL_SHUTDOWN || frame->len < 4u) {
        errno = EPROTO;
        return -1;
    }
    if (frame->payload[off++] != (uint8_t)OPTY_VERSION) {
        errno = EPROTO;
        return -1;
    }

    token_len = frame->payload[off++];
    if (token_len + 1u > token_cap || frame->len < off + (size_t)token_len + 2u) {
        errno = EPROTO;
        return -1;
    }
    if (token_len > 0u) {
        memcpy(token, frame->payload + off, token_len);
    }
    token[token_len] = '\0';
    off += token_len;

    graceful_len = opty_get_u16(frame->payload + off);
    off += 2u;
    if (graceful_len > OPTY_MAX_GRACEFUL_INPUT || frame->len != off + (size_t)graceful_len) {
        errno = EPROTO;
        return -1;
    }

    *input = frame->payload + off;
    *input_len = graceful_len;
    return 0;
}

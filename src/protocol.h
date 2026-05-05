#ifndef OPTY_PROTOCOL_H
#define OPTY_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define OPTY_VERSION 1
#define OPTY_DEFAULT_HOST "127.0.0.1"
#define OPTY_DEFAULT_PORT "6077"
#define OPTY_DEFAULT_RING_SIZE (1024u * 1024u)
#define OPTY_MAX_FRAME (16u * 1024u * 1024u)
#define OPTY_MAX_TOKEN 255u
#define OPTY_MAX_GRACEFUL_INPUT 4096u

enum opty_msg_type {
    OPTY_MSG_HELLO = 1,
    OPTY_MSG_RECONNECT = 2,
    OPTY_MSG_WELCOME = 3,
    OPTY_MSG_STDIN = 4,
    OPTY_MSG_STDOUT = 5,
    OPTY_MSG_RESIZE = 6,
    OPTY_MSG_ACK = 7,
    OPTY_MSG_REPLAY = 8,
    OPTY_MSG_ERROR = 9,
    OPTY_MSG_CONTROL_SHUTDOWN = 10
};

struct opty_frame {
    uint8_t type;
    size_t len;
    uint8_t *payload;
};

ssize_t opty_read_full(int fd, void *buf, size_t len);
ssize_t opty_write_full(int fd, const void *buf, size_t len);

int opty_send_frame(int fd, uint8_t type, const void *payload, size_t len);
int opty_recv_frame(int fd, struct opty_frame *frame);
void opty_frame_free(struct opty_frame *frame);

uint16_t opty_get_u16(const uint8_t *p);
uint64_t opty_get_u64(const uint8_t *p);
void opty_put_u16(uint8_t *p, uint16_t v);
void opty_put_u64(uint8_t *p, uint64_t v);

int opty_send_hello(int fd, uint16_t cols, uint16_t rows, const char *token);
int opty_send_reconnect(int fd, uint64_t last_seq, uint16_t cols, uint16_t rows, const char *token);
int opty_send_resize(int fd, uint16_t cols, uint16_t rows);
int opty_send_ack(int fd, uint64_t seq);
int opty_send_stdin(int fd, const void *data, size_t len);
int opty_send_stdout(int fd, uint64_t seq, const void *data, size_t len);
int opty_send_replay(int fd, uint64_t from_seq, const void *data, size_t len);
int opty_send_welcome(int fd, uint64_t base_seq, uint64_t next_seq);
int opty_send_error(int fd, const char *message);
int opty_send_control_shutdown(int fd, const char *token, const void *input, size_t input_len);

int opty_parse_client_intro(const struct opty_frame *frame, uint64_t *last_seq, uint16_t *cols, uint16_t *rows, char *token, size_t token_cap);
int opty_parse_resize(const struct opty_frame *frame, uint16_t *cols, uint16_t *rows);
int opty_parse_ack(const struct opty_frame *frame, uint64_t *seq);
int opty_parse_stream(const struct opty_frame *frame, uint64_t *seq, const uint8_t **data, size_t *len);
int opty_parse_welcome(const struct opty_frame *frame, uint64_t *base_seq, uint64_t *next_seq);
int opty_parse_control_shutdown(const struct opty_frame *frame, char *token, size_t token_cap, const uint8_t **input, size_t *input_len);

#endif

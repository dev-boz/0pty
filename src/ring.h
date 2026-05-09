#ifndef OPTY_RING_H
#define OPTY_RING_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

struct opty_ring {
    uint8_t *buf;
    size_t cap;
    uint64_t base_seq;
    uint64_t next_seq;
    pthread_mutex_t mu;
};

int opty_ring_init(struct opty_ring *ring, size_t capacity);
void opty_ring_destroy(struct opty_ring *ring);
void opty_ring_append(struct opty_ring *ring, const uint8_t *data, size_t len, uint64_t *start_seq);
uint8_t *opty_ring_snapshot_window(struct opty_ring *ring, uint64_t requested_seq, uint64_t *base_seq,
                                   uint64_t *from_seq, uint64_t *next_seq, size_t *len);
uint8_t *opty_ring_snapshot_from(struct opty_ring *ring, uint64_t requested_seq, uint64_t *from_seq, uint64_t *next_seq, size_t *len);
void opty_ring_bounds(struct opty_ring *ring, uint64_t *base_seq, uint64_t *next_seq);

#endif

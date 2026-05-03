#include "ring.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static size_t opty_min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

int opty_ring_init(struct opty_ring *ring, size_t capacity)
{
    int rc;

    if (ring == NULL || capacity == 0u) {
        errno = EINVAL;
        return -1;
    }

    ring->buf = malloc(capacity);
    if (ring->buf == NULL) {
        errno = ENOMEM;
        return -1;
    }

    rc = pthread_mutex_init(&ring->mu, NULL);
    if (rc != 0) {
        free(ring->buf);
        ring->buf = NULL;
        errno = rc;
        return -1;
    }

    ring->cap = capacity;
    ring->base_seq = 0u;
    ring->next_seq = 0u;
    return 0;
}

void opty_ring_destroy(struct opty_ring *ring)
{
    if (ring == NULL) {
        return;
    }

    if (ring->buf != NULL) {
        pthread_mutex_destroy(&ring->mu);
        free(ring->buf);
    }
    ring->buf = NULL;
    ring->cap = 0u;
    ring->base_seq = 0u;
    ring->next_seq = 0u;
}

void opty_ring_append(struct opty_ring *ring, const uint8_t *data, size_t len, uint64_t *start_seq)
{
    uint64_t old_next;
    uint64_t old_base;
    uint64_t new_next;
    uint64_t window;
    size_t keep;
    size_t src_off;
    size_t dest;
    size_t first;

    if (ring == NULL || ring->buf == NULL || ring->cap == 0u) {
        return;
    }
    if (pthread_mutex_lock(&ring->mu) != 0) {
        return;
    }

    old_next = ring->next_seq;
    old_base = ring->base_seq;
    if (start_seq != NULL) {
        *start_seq = old_next;
    }
    if (len == 0u || data == NULL) {
        pthread_mutex_unlock(&ring->mu);
        return;
    }

    new_next = old_next + (uint64_t)len;
    window = old_next - old_base;

    keep = opty_min_size(len, ring->cap);
    src_off = len - keep;
    dest = (size_t)((old_next + (uint64_t)src_off) % ring->cap);

    first = ring->cap - dest;
    first = opty_min_size(first, keep);
    memcpy(ring->buf + dest, data + src_off, first);
    if (keep > first) {
        memcpy(ring->buf, data + src_off + first, keep - first);
    }

    ring->next_seq = new_next;
    if (window + (uint64_t)len > (uint64_t)ring->cap) {
        ring->base_seq = new_next - (uint64_t)ring->cap;
    }

    pthread_mutex_unlock(&ring->mu);
}

uint8_t *opty_ring_snapshot_from(struct opty_ring *ring, uint64_t requested_seq, uint64_t *from_seq, uint64_t *next_seq, size_t *len)
{
    uint64_t from;
    uint64_t end;
    size_t out_len;
    uint8_t *out;
    size_t start_idx;
    size_t first;

    if (ring == NULL || ring->buf == NULL || ring->cap == 0u) {
        errno = EINVAL;
        return NULL;
    }
    if (pthread_mutex_lock(&ring->mu) != 0) {
        return NULL;
    }

    if (requested_seq < ring->base_seq) {
        from = ring->base_seq;
    } else if (requested_seq > ring->next_seq) {
        from = ring->next_seq;
    } else {
        from = requested_seq;
    }
    end = ring->next_seq;
    out_len = (size_t)(end - from);

    if (from_seq != NULL) {
        *from_seq = from;
    }
    if (next_seq != NULL) {
        *next_seq = end;
    }
    if (len != NULL) {
        *len = out_len;
    }
    if (out_len == 0u) {
        pthread_mutex_unlock(&ring->mu);
        return NULL;
    }

    out = malloc(out_len);
    if (out == NULL) {
        errno = ENOMEM;
        pthread_mutex_unlock(&ring->mu);
        return NULL;
    }

    start_idx = (size_t)(from % ring->cap);
    first = opty_min_size(ring->cap - start_idx, out_len);
    memcpy(out, ring->buf + start_idx, first);
    if (out_len > first) {
        memcpy(out + first, ring->buf, out_len - first);
    }

    pthread_mutex_unlock(&ring->mu);
    return out;
}

void opty_ring_bounds(struct opty_ring *ring, uint64_t *base_seq, uint64_t *next_seq)
{
    if (ring == NULL || ring->buf == NULL || ring->cap == 0u) {
        return;
    }
    if (pthread_mutex_lock(&ring->mu) != 0) {
        return;
    }
    if (base_seq != NULL) {
        *base_seq = ring->base_seq;
    }
    if (next_seq != NULL) {
        *next_seq = ring->next_seq;
    }
    pthread_mutex_unlock(&ring->mu);
}

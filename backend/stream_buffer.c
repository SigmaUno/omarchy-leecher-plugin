#include "stream_buffer.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct StreamBuffer {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned char *data;
    size_t size;
    size_t cap;
    size_t max_bytes;   /* 0 == unbounded */
    int complete;
    int failed;
};

StreamBuffer *stream_buffer_create(size_t max_bytes) {
    StreamBuffer *sb = calloc(1, sizeof(*sb));
    if (!sb) return NULL;
    if (pthread_mutex_init(&sb->mutex, NULL) != 0) { free(sb); return NULL; }
    if (pthread_cond_init(&sb->cond, NULL) != 0) {
        pthread_mutex_destroy(&sb->mutex); free(sb); return NULL;
    }
    sb->max_bytes = max_bytes;
    return sb;
}

void stream_buffer_destroy(StreamBuffer *sb) {
    if (!sb) return;
    pthread_mutex_destroy(&sb->mutex);
    pthread_cond_destroy(&sb->cond);
    free(sb->data);
    free(sb);
}

int stream_buffer_append(StreamBuffer *sb, const unsigned char *data, size_t size) {
    int ok = 1;
    if (!sb || (!data && size)) return 0;
    if (!size) return 1;
    pthread_mutex_lock(&sb->mutex);
    if (sb->failed) {
        ok = 0;
    } else if (sb->max_bytes && size > sb->max_bytes - sb->size) {
        sb->failed = 1;
        ok = 0;
    } else {
        if (size > sb->cap - sb->size) {
            size_t want = sb->cap ? sb->cap : (size_t)1 << 20;
            while (want < sb->size + size) {
                if (want > (size_t)-1 / 2) { want = sb->size + size; break; }
                want *= 2;
            }
            unsigned char *grown = realloc(sb->data, want);
            if (!grown) { sb->failed = 1; ok = 0; }
            else { sb->data = grown; sb->cap = want; }
        }
        if (ok) {
            memcpy(sb->data + sb->size, data, size);
            sb->size += size;
        }
    }
    pthread_cond_broadcast(&sb->cond);
    pthread_mutex_unlock(&sb->mutex);
    return ok;
}

void stream_buffer_set_complete(StreamBuffer *sb) {
    if (!sb) return;
    pthread_mutex_lock(&sb->mutex);
    sb->complete = 1;
    pthread_cond_broadcast(&sb->cond);
    pthread_mutex_unlock(&sb->mutex);
}

void stream_buffer_set_failed(StreamBuffer *sb) {
    if (!sb) return;
    pthread_mutex_lock(&sb->mutex);
    sb->failed = 1;
    pthread_cond_broadcast(&sb->cond);
    pthread_mutex_unlock(&sb->mutex);
}

size_t stream_buffer_size(StreamBuffer *sb) {
    size_t n;
    if (!sb) return 0;
    pthread_mutex_lock(&sb->mutex);
    n = sb->size;
    pthread_mutex_unlock(&sb->mutex);
    return n;
}


int stream_buffer_is_complete(StreamBuffer *sb) {
    int v;
    if (!sb) return 0;
    pthread_mutex_lock(&sb->mutex);
    v = sb->complete;
    pthread_mutex_unlock(&sb->mutex);
    return v;
}

int stream_buffer_is_failed(StreamBuffer *sb) {
    int v;
    if (!sb) return 0;
    pthread_mutex_lock(&sb->mutex);
    v = sb->failed;
    pthread_mutex_unlock(&sb->mutex);
    return v;
}

/* Caller holds the lock.  Wait until size >= need, or the stream is settled. */
static void wait_until(StreamBuffer *sb, size_t need) {
    while (sb->size < need && !sb->complete && !sb->failed)
        pthread_cond_wait(&sb->cond, &sb->mutex);
}

long long stream_buffer_wait_prebuffer(StreamBuffer *sb, size_t bytes) {
    size_t have;
    int short_fail;
    if (!sb) return -1;
    pthread_mutex_lock(&sb->mutex);
    wait_until(sb, bytes);
    have = sb->size;
    short_fail = sb->failed && have < bytes;
    pthread_mutex_unlock(&sb->mutex);
    return short_fail ? -1 : (long long)have;
}

long long stream_buffer_wait_complete(StreamBuffer *sb) {
    size_t have;
    int failed;
    if (!sb) return -1;
    pthread_mutex_lock(&sb->mutex);
    while (!sb->complete && !sb->failed)
        pthread_cond_wait(&sb->cond, &sb->mutex);
    have = sb->size;
    failed = sb->failed && !sb->complete;
    pthread_mutex_unlock(&sb->mutex);
    return failed ? -1 : (long long)have;
}

long long stream_buffer_read_at(StreamBuffer *sb, void *dst, size_t count, unsigned long long offset) {
    size_t avail;
    size_t need;
    if (!sb || !dst) return -1;
    if (!count) return 0;
    pthread_mutex_lock(&sb->mutex);
    /* Cap the wait target so a wild offset cannot overflow. */
    need = (offset > (unsigned long long)((size_t)-1 - count)) ? (size_t)-1 : (size_t)offset + count;
    wait_until(sb, need);
    if (offset >= sb->size) {
        int failed = sb->failed && !sb->complete;
        pthread_mutex_unlock(&sb->mutex);
        return failed ? -1 : 0;
    }
    avail = sb->size - (size_t)offset;
    if (avail > count) avail = count;
    memcpy(dst, sb->data + (size_t)offset, avail);
    pthread_mutex_unlock(&sb->mutex);
    return (long long)avail;
}

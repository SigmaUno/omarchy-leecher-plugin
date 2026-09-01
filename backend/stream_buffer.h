#ifndef STREAM_BUFFER_H
#define STREAM_BUFFER_H

#include <stddef.h>

/* A thread-safe, append-only byte buffer shared between a producer thread that
 * streams a remote file in and a consumer (the decoder) that reads it as it
 * grows.  The consumer's reads block at the download frontier until more bytes
 * arrive or the producer signals completion/failure, so a partially-transferred
 * file is never mistaken for a truncated one. */
typedef struct StreamBuffer StreamBuffer;

/* max_bytes caps the buffer; an append that would exceed it fails the buffer.
 * 0 means unbounded. */
StreamBuffer *stream_buffer_create(size_t max_bytes);
void stream_buffer_destroy(StreamBuffer *sb);

/* Producer side. */
int stream_buffer_append(StreamBuffer *sb, const unsigned char *data, size_t size); /* 1 ok, 0 rejected */
void stream_buffer_set_complete(StreamBuffer *sb);
void stream_buffer_set_failed(StreamBuffer *sb);

size_t stream_buffer_size(StreamBuffer *sb);
int stream_buffer_is_complete(StreamBuffer *sb);
int stream_buffer_is_failed(StreamBuffer *sb);

/* Block until at least `bytes` have arrived, or the stream completes or fails.
 * Returns the number of bytes available, or -1 if it failed short of `bytes`. */
long long stream_buffer_wait_prebuffer(StreamBuffer *sb, size_t bytes);

/* Block until the stream completes or fails; returns the final size, or -1 on
 * failure.  Used when the decoder must know the true end (e.g. a container that
 * seeks to EOF while parsing its header). */
long long stream_buffer_wait_complete(StreamBuffer *sb);

/* Consumer read: copy up to `count` bytes starting at `offset`.  Blocks until
 * `offset + count` bytes exist or the stream completes/fails, then returns the
 * bytes copied (a short count only at true end of stream), or -1 on failure
 * with nothing readable at `offset`. */
long long stream_buffer_read_at(StreamBuffer *sb, void *dst, size_t count, unsigned long long offset);

#endif

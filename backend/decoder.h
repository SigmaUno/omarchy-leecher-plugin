#ifndef DECODER_H
#define DECODER_H

#include <stddef.h>

#include "stream_buffer.h"

/* A streamable decode source.  Reads from a StreamBuffer that a producer thread
 * fills as the remote file transfers in, so decoding (and playback) can begin
 * once the header plus a small prebuffer have arrived rather than after the
 * whole file.  PCM is pulled a few seconds at a time, so memory stays bounded
 * regardless of track length. */
typedef struct DecoderSource DecoderSource;

/* Type used for frame counts; matches libsndfile's sf_count_t. */
typedef long long DecoderFrameCount;

/* Opens the audio file for streaming over `buffer` and fills *src.  May block
 * until enough of the stream has arrived to parse the header.  Returns 1 on
 * success, 0 if the stream is empty (no audio), and -1 on fatal errors (message
 * in `error`).  On success *src takes ownership of `buffer` and frees it in
 * decoder_close(); the caller must ensure the producer thread feeding `buffer`
 * has been joined before closing. */
int decoder_open(StreamBuffer *buffer, DecoderSource **src, char *error, size_t error_size);

/* Reads up to `frame_count` interleaved frames into `out`.  Returns the number
 * of frames read (0 at end of stream), or -1 on error. */
long long decoder_read_frames(DecoderSource *src, short *out, long long frame_count);

/* Seeks to the given frame.  Returns the new frame position, or -1 on error. */
long long decoder_seek(DecoderSource *src, long long frame);

long long decoder_total_frames(const DecoderSource *src);
int decoder_channels(const DecoderSource *src);
int decoder_rate(const DecoderSource *src);

/* Frees the source (including the assembled bytes and the open file). */
void decoder_close(DecoderSource *src);

#endif

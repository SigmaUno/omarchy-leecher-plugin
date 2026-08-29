#ifndef DECODER_H
#define DECODER_H

#include <stddef.h>

#include "assembler.h"

/* A streamable decode source.  Constructed from an assembled queue of source
 * bytes and decoded incrementally (a few seconds of PCM at a time) rather than
 * all at once, so memory stays bounded regardless of track length. */
typedef struct DecoderSource DecoderSource;

/* Type used for frame counts; matches libsndfile's sf_count_t. */
typedef long long DecoderFrameCount;

/* Assembles all queued source bytes, opens the audio file for streaming, and
 * fills *src.  Returns 1 on success, 0 if the queue is empty (no audio), and
 * -1 on fatal errors (message in `error`).  The queue is drained. */
int decoder_open(Assembler *assembler, DecoderSource **src, char *error, size_t error_size);

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

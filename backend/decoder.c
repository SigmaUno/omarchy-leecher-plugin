#include "decoder.h"

#include <sndfile.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct DecoderSource {
    StreamBuffer *buffer;
    sf_count_t offset;
    SF_INFO info;
    SNDFILE *file;
    sf_count_t frame;
    int opened_partial; /* opened before the transfer completed */
    int refreshed;      /* re-opened once the whole file was present */
};

static void set_error(char *error, size_t error_size, const char *format, ...) {
    va_list arguments;
    if (!error || !error_size) return;
    va_start(arguments, format); vsnprintf(error, error_size, format, arguments); va_end(arguments);
}

static sf_count_t vio_get_filelen(void *userdata) {
    DecoderSource *d = userdata;
    /* The bytes present so far.  While still transferring this is short of the
     * real length, but FLAC (the only format opened before completion) takes
     * its sample count from STREAMINFO, not the file size, and a read past the
     * frontier blocks in vio_read until the data arrives. */
    return (sf_count_t)stream_buffer_size(d->buffer);
}

static sf_count_t vio_seek(sf_count_t offset, int whence, void *userdata) {
    DecoderSource *d = userdata;
    sf_count_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = d->offset; break;
    case SEEK_END: {
        /* Some containers seek to EOF while parsing (e.g. Ogg): give them the
         * true end, waiting for the transfer to finish if need be. */
        long long total = stream_buffer_wait_complete(d->buffer);
        base = total < 0 ? (sf_count_t)stream_buffer_size(d->buffer) : (sf_count_t)total;
        break;
    }
    default: return -1;
    }
    if (base + offset < 0) return -1;
    d->offset = base + offset;
    return d->offset;
}

static sf_count_t vio_read(void *ptr, sf_count_t count, void *userdata) {
    DecoderSource *d = userdata;
    long long got;
    if (count <= 0) return 0;
    got = stream_buffer_read_at(d->buffer, ptr, (size_t)count, (unsigned long long)d->offset);
    if (got < 0) return 0;
    d->offset += got;
    return got;
}

static sf_count_t vio_tell(void *userdata) { return ((DecoderSource *)userdata)->offset; }

static const SF_VIRTUAL_IO decoder_vio = { vio_get_filelen, vio_seek, vio_read, NULL, vio_tell };

/* libsndfile parses the header once, at open; opening a compressed stream (FLAC
 * especially) from a partial download can leave its seek machinery permanently
 * broken even after the rest arrives.  So once the whole file is present, re-open
 * the handle on it and restore the play position.  Cheap: the bytes are all in
 * RAM by then. */
static void decoder_refresh(DecoderSource *d) {
    SF_INFO info = {0};
    SNDFILE *nf;
    sf_count_t saved_offset;
    if (d->refreshed || !d->opened_partial) return;
    if (!stream_buffer_is_complete(d->buffer)) return;   /* a failed transfer stays as-is */
    d->refreshed = 1;
    saved_offset = d->offset;
    d->offset = 0;
    nf = sf_open_virtual((SF_VIRTUAL_IO *)&decoder_vio, SFM_READ, &info, d);
    if (!nf) { d->offset = saved_offset; return; }       /* keep the working handle intact */
    sf_close(d->file);
    d->file = nf;
    d->info = info;
    if (d->frame > 0 && sf_seek(d->file, d->frame, SEEK_SET) < 0) {
        sf_seek(d->file, 0, SEEK_SET);
        d->frame = 0;
    }
}

int decoder_open(StreamBuffer *buffer, DecoderSource **src, char *error, size_t error_size) {
    DecoderSource *d;
    SF_VIRTUAL_IO io = { vio_get_filelen, vio_seek, vio_read, NULL, vio_tell };
    if (!buffer || !src) { set_error(error, error_size, "buffer and output are required"); return -1; }
    *src = NULL;
    d = calloc(1, sizeof(*d));
    if (!d) { set_error(error, error_size, "out of memory"); return -1; }
    d->buffer = buffer;
    d->file = sf_open_virtual(&io, SFM_READ, &d->info, d);
    if (!d->file) {
        if (stream_buffer_is_failed(buffer)) {
            set_error(error, error_size, "source transfer failed");
            free(d);
            return -1;
        }
        if (stream_buffer_is_complete(buffer) && stream_buffer_size(buffer) == 0) {
            free(d);
            return 0;
        }
        set_error(error, error_size, "decoder: %s", sf_strerror(NULL));
        free(d);
        return -1;
    }
    d->opened_partial = !stream_buffer_is_complete(buffer);
    *src = d;
    return 1;
}

long long decoder_read_frames(DecoderSource *d, short *out, long long frame_count) {
    if (!d || !d->file || !out || frame_count <= 0) return -1;
    decoder_refresh(d);
    if (frame_count > d->info.frames - d->frame) frame_count = d->info.frames - d->frame;
    if (frame_count <= 0) return 0;
    sf_count_t read = sf_readf_short(d->file, out, frame_count);
    if (read < 0) return -1;
    d->frame += read;
    return read;
}

long long decoder_seek(DecoderSource *d, long long frame) {
    if (!d || !d->file) return -1;
    decoder_refresh(d);
    if (frame < 0) frame = 0;
    if (frame > d->info.frames) frame = d->info.frames;
    if (sf_seek(d->file, frame, SEEK_SET) < 0) return -1;
    d->frame = frame;
    return frame;
}

long long decoder_total_frames(const DecoderSource *d) { return d ? d->info.frames : 0; }
int decoder_channels(const DecoderSource *d) { return d ? (int)d->info.channels : 0; }
int decoder_rate(const DecoderSource *d) { return d ? d->info.samplerate : 0; }

void decoder_close(DecoderSource *d) {
    if (!d) return;
    if (d->file) sf_close(d->file);
    stream_buffer_destroy(d->buffer);
    free(d);
}

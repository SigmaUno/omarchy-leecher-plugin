#include "decoder.h"

#include <sndfile.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const unsigned char *data; sf_count_t size; sf_count_t offset; } MemoryAudio;

struct DecoderSource {
    unsigned char *bytes;
    sf_count_t size;
    MemoryAudio audio;
    SF_INFO info;
    SNDFILE *file;
    sf_count_t frame;
};

static void set_error(char *error, size_t error_size, const char *format, ...) {
    va_list arguments;
    if (!error || !error_size) return;
    va_start(arguments, format); vsnprintf(error, error_size, format, arguments); va_end(arguments);
}
static sf_count_t memory_length(void *userdata) { return ((MemoryAudio *)userdata)->size; }
static sf_count_t memory_seek(sf_count_t offset, int whence, void *userdata) {
    MemoryAudio *audio = userdata;
    sf_count_t position = whence == SEEK_SET ? offset : whence == SEEK_CUR ? audio->offset + offset : whence == SEEK_END ? audio->size + offset : -1;
    if (position < 0 || position > audio->size) return -1;
    audio->offset = position; return position;
}
static sf_count_t memory_read(void *destination, sf_count_t count, void *userdata) {
    MemoryAudio *audio = userdata;
    if (count > audio->size - audio->offset) count = audio->size - audio->offset;
    memcpy(destination, audio->data + audio->offset, (size_t)count);
    audio->offset += count; return count;
}
static sf_count_t memory_tell(void *userdata) { return ((MemoryAudio *)userdata)->offset; }

int decoder_open(Assembler *assembler, DecoderSource **src, char *error, size_t error_size) {
    AssemblerPiece piece = {0};
    unsigned char *bytes = NULL;
    size_t size = 0;
    DecoderSource *d;
    SF_VIRTUAL_IO io = { memory_length, memory_seek, memory_read, NULL, memory_tell };
    if (!assembler || !src) { set_error(error, error_size, "assembler and output are required"); return -1; }
    *src = NULL;
    while (assembler_pop(assembler, &piece) == 1) {
        unsigned char *grown;
        if (piece.size > SIZE_MAX - size) { assembler_piece_destroy(&piece); free(bytes); set_error(error, error_size, "assembled audio is too large"); return -1; }
        grown = realloc(bytes, size + piece.size);
        if (!grown) { assembler_piece_destroy(&piece); free(bytes); set_error(error, error_size, "out of memory"); return -1; }
        bytes = grown; memcpy(bytes + size, piece.data, piece.size); size += piece.size;
        assembler_piece_destroy(&piece);
    }
    if (!size) { set_error(error, error_size, "assembler queue is empty"); return 0; }
    d = calloc(1, sizeof(*d));
    if (!d) { free(bytes); set_error(error, error_size, "out of memory"); return -1; }
    d->bytes = bytes;
    d->size = (sf_count_t)size;
    d->audio.data = bytes; d->audio.size = d->size; d->audio.offset = 0;
    d->file = sf_open_virtual(&io, SFM_READ, &d->info, &d->audio);
    if (!d->file) {
        set_error(error, error_size, "decoder: %s", sf_strerror(NULL));
        decoder_close(d);
        return -1;
    }
    *src = d;
    return 1;
}

long long decoder_read_frames(DecoderSource *d, short *out, long long frame_count) {
    if (!d || !d->file || !out || frame_count <= 0) return -1;
    if (frame_count > d->info.frames - d->frame) frame_count = d->info.frames - d->frame;
    if (frame_count <= 0) return 0;
    sf_count_t read = sf_readf_short(d->file, out, frame_count);
    if (read < 0) return -1;
    d->frame += read;
    return read;
}

long long decoder_seek(DecoderSource *d, long long frame) {
    if (!d || !d->file) return -1;
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
    free(d->bytes);
    free(d);
}

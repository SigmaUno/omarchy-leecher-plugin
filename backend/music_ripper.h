#ifndef MUSIC_RIPPER_H
#define MUSIC_RIPPER_H

#include <stddef.h>

#include "library_handler.h"

typedef int (*MusicRipperWriteFn)(const unsigned char *data, size_t size, void *userdata);
typedef int (*MusicRipperRemoteFn)(const LibrarySource *source, MusicRipperWriteFn write,
                                   void *write_userdata, void *transport_userdata);

typedef struct {
    MusicRipperRemoteFn ssh;
    MusicRipperRemoteFn https;
    MusicRipperRemoteFn network;
    void *userdata;
} MusicRipperTransports;

typedef struct {
    const LibraryHandler *library;
    MusicRipperTransports transports;
} MusicRipper;

/* NULL preferred_kind uses the first valid source in library order. */
int music_ripper_play_next(const MusicRipper *ripper, const LibrarySongQuery *song,
                           const LibrarySourceKind *preferred_kind, MusicRipperWriteFn write,
                           void *write_userdata, char *error, size_t error_size);

#endif

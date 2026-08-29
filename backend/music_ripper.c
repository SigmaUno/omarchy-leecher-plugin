#include "music_ripper.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (!error || !error_size) return;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

static int has_required_fields(const LibrarySource *source) {
    switch (source->kind) {
    case LIBRARY_SOURCE_LOCAL: return source->path && *source->path;
    case LIBRARY_SOURCE_SSH: return source->path && *source->path && source->username && *source->username && source->ip && *source->ip;
    case LIBRARY_SOURCE_HTTPS: return source->url && *source->url;
    case LIBRARY_SOURCE_NETWORK: return source->path && *source->path && source->username && *source->username && source->ip && *source->ip;
    }
    return 0;
}

static int stream_local(const char *path, MusicRipperWriteFn write, void *userdata, char *error, size_t error_size) {
    unsigned char buffer[64 * 1024];
    FILE *file = fopen(path, "rb");
    size_t bytes;
    if (!file) { set_error(error, error_size, "cannot open local file: %s", path); return -1; }
    while ((bytes = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        if (!write(buffer, bytes, userdata)) { fclose(file); set_error(error, error_size, "app rejected audio data"); return -1; }
    }
    if (ferror(file)) { fclose(file); set_error(error, error_size, "cannot read local file: %s", path); return -1; }
    fclose(file);
    return 0;
}

int music_ripper_play_next(const MusicRipper *ripper, const LibrarySongQuery *song,
                           const LibrarySourceKind *preferred_kind, MusicRipperWriteFn write,
                           void *write_userdata, char *error, size_t error_size) {
    LibraryTrack track = {0};
    const LibrarySource *source = NULL;
    size_t i;
    int found, result;
    if (!ripper || !ripper->library || !song || !write) { set_error(error, error_size, "ripper, library, song, and write callback are required"); return -1; }
    found = library_handler_resolve(ripper->library, song, &track, error, error_size);
    if (found != 1) return found == 0 ? 0 : -1;
    for (i = 0; i < track.source_count; i++) {
        if ((!preferred_kind || track.sources[i].kind == *preferred_kind) && has_required_fields(&track.sources[i])) { source = &track.sources[i]; break; }
    }
    if (!source) { library_handler_track_destroy(&track); set_error(error, error_size, "track has no usable source"); return -1; }
    switch (source->kind) {
    case LIBRARY_SOURCE_LOCAL:
        result = stream_local(source->path, write, write_userdata, error, error_size);
        break;
    case LIBRARY_SOURCE_SSH:
        if (!ripper->transports.ssh) { set_error(error, error_size, "SSH transport is not configured"); result = -1; }
        else {
            result = ripper->transports.ssh(source, write, write_userdata, ripper->transports.userdata);
            if (result) set_error(error, error_size, "SSH transfer failed");
        }
        break;
    case LIBRARY_SOURCE_HTTPS:
        if (!ripper->transports.https) { set_error(error, error_size, "HTTPS transport is not configured"); result = -1; }
        else {
            result = ripper->transports.https(source, write, write_userdata, ripper->transports.userdata);
            if (result) set_error(error, error_size, "HTTPS transfer failed");
        }
        break;
    case LIBRARY_SOURCE_NETWORK:
        if (!ripper->transports.network) { set_error(error, error_size, "network transport is not configured"); result = -1; }
        else result = ripper->transports.network(source, write, write_userdata, ripper->transports.userdata);
        break;
    default:
        set_error(error, error_size, "unknown source kind"); result = -1;
    }
    library_handler_track_destroy(&track);
    return result == 0 ? 1 : -1;
}

#ifndef LIBRARY_HANDLER_H
#define LIBRARY_HANDLER_H

#include <stddef.h>

typedef enum {
    LIBRARY_SOURCE_LOCAL,
    LIBRARY_SOURCE_SSH,
    LIBRARY_SOURCE_HTTPS,
    LIBRARY_SOURCE_NETWORK
} LibrarySourceKind;

typedef struct {
    LibrarySourceKind kind;
    char *path;      /* JSON: PATH */
    char *username;  /* JSON: USERNAME */
    char *url;       /* JSON: URL */
    char *ip;        /* JSON: IP */
} LibrarySource;

typedef struct {
    const char *id;
    const char *title;
    const char *artist;
    const char *album;
    /* Optional. Only written when a NEW track entry is created; merging a
     * source into an existing track never clobbers the cover it already has. */
    const char *cover;
} LibrarySongQuery;

typedef struct {
    char *id;
    char *title;
    char *artist;
    char *album;
    char *cover;     /* JSON: cover -- absolute path to a user-chosen image,
                      * NULL when the art should come from the audio file */
    LibrarySource *sources;
    size_t source_count;
} LibraryTrack;

typedef struct LibraryHandler LibraryHandler;

/* Loads and validates the JSON library. error may be NULL. */
LibraryHandler *library_handler_open(const char *library_path, char *error, size_t error_size);
void library_handler_close(LibraryHandler *handler);

size_t library_handler_track_count(const LibraryHandler *handler);
/* Returns 1 with a copied track, 0 for an out-of-range index, or -1 on error. */
int library_handler_track_at(const LibraryHandler *handler, size_t index, LibraryTrack *track,
                             char *error, size_t error_size);

/* Atomically appends a source to a matching title/artist track, or creates it. */
int library_handler_add_source(const char *library_path, const LibrarySongQuery *song,
                               const LibrarySource *source, char *error, size_t error_size);

/* Rewrites the title/artist/album/cover of the track at tracks[track_index] and
 * saves atomically. A NULL value leaves that field untouched; a field absent
 * from the object is inserted. Returns 1 on success. */
int library_handler_update_track(const char *library_path, size_t track_index,
                                 const char *title, const char *artist, const char *album,
                                 const char *cover, char *error, size_t error_size);

/* Removes the track at tracks[track_index] and saves atomically. Returns 1 on
 * success, 0 for an out-of-range index, or -1 on error. */
int library_handler_remove_track(const char *library_path, size_t track_index,
                                 char *error, size_t error_size);

/* Returns 1 when a matching track is found, 0 when absent, or -1 on error. */
int library_handler_resolve(const LibraryHandler *handler, const LibrarySongQuery *query,
                            LibraryTrack *track, char *error, size_t error_size);
void library_handler_track_destroy(LibraryTrack *track);

#endif

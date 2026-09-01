#ifndef METADATA_H
#define METADATA_H

#include <stddef.h>

typedef struct {
    char *title;
    char *artist;
    char *album;
} AudioMetadata;

/* Sets extra `ssh` command-line options (used for SSH connection multiplexing)
 * spliced into every remote metadata probe.  Pass NULL or "" to disable. */
void metadata_set_ssh_opts(const char *opts);

/* Extracts metadata from a local audio file.
 * Returns 1 on success, 0 if file not found, -1 on error.
 * The caller must free the returned struct fields with free(). */
int metadata_extract_from_file(const char *filepath, AudioMetadata *metadata, char *error, size_t error_size);

/* Extracts metadata from a remote audio file via SSH.
 * Returns 1 on success, 0 if file not found, -1 on error.
 * The caller must free the returned struct fields with free(). */
int metadata_extract_from_ssh(const char *username, const char *ip, const char *filepath,
                              AudioMetadata *metadata, char *error, size_t error_size);

/* Extracts metadata from a remote audio stream via its HTTP(S) URL.
 * Returns 1 on success, 0 if nothing could be read (e.g. URL not reachable),
 * -1 on error.
 * The caller must free the returned struct fields with free(). */
int metadata_extract_from_url(const char *url, AudioMetadata *metadata, char *error, size_t error_size);

/* Free allocated fields in metadata struct */
void metadata_destroy(AudioMetadata *metadata);

#endif

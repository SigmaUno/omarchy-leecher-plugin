#ifndef METADATA_H
#define METADATA_H

#include <stddef.h>

typedef struct {
    char *title;
    char *artist;
    char *album;
} AudioMetadata;

/* Extracts metadata from a local audio file.
 * Returns 1 on success, 0 if file not found, -1 on error.
 * The caller must free the returned struct fields with free(). */
int metadata_extract_from_file(const char *filepath, AudioMetadata *metadata, char *error, size_t error_size);

/* Extracts metadata from a remote audio file via SSH.
 * Returns 1 on success, 0 if file not found, -1 on error.
 * The caller must free the returned struct fields with free(). */
int metadata_extract_from_ssh(const char *username, const char *ip, const char *filepath,
                              AudioMetadata *metadata, char *error, size_t error_size);

/* Free allocated fields in metadata struct */
void metadata_destroy(AudioMetadata *metadata);

#endif

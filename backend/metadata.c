/*
 * metadata.c
 *
 * Extracts metadata from local audio files using external tools (mediainfo or ffprobe).
 * Falls back gracefully if tools are unavailable.
 */

#define _POSIX_C_SOURCE 200809L

#include "metadata.h"
#include "ssh_opts.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void set_error(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (!error || !error_size) return;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

/* Extra `ssh` options (SSH connection multiplexing) shared with the rest of the
 * app; empty until the caller opts in via metadata_set_ssh_opts(). */
static char ssh_opts[512];

void metadata_set_ssh_opts(const char *opts) {
    snprintf(ssh_opts, sizeof(ssh_opts), "%s", opts ? opts : "");
}

static void trim_string(char *str) {
    char *start, *end;
    size_t leading;
    if (!str || !*str) return;
    start = str;
    while (*start && isspace((unsigned char)*start)) start++;
    leading = (size_t)(start - str);
    if (leading) memmove(str, start, strlen(start) + 1);
    if (!*str) return;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) *end-- = '\0';
}

/* Validate SSH username/IP format (prevent command injection) */
static int valid_ssh_name(const char *value, int allow_colon) {
    const unsigned char *p = (const unsigned char *)value;
    if (!p || !*p) return 0;
    for (; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '-' && *p != '_' &&
            !(allow_colon && *p == ':')) return 0;
    }
    return 1;
}

/* Build a remote shell command with a safely single-quoted path argument. */
static char *remote_command_with_path(const char *prefix, const char *path) {
    const char *cursor;
    char *command, *out;
    size_t length = strlen(prefix) + 3; /* prefix, enclosing quotes, final NUL */
    for (cursor = path; *cursor; cursor++) length += *cursor == '\'' ? 4 : 1;
    command = malloc(length);
    if (!command) return NULL;
    out = command;
    memcpy(out, prefix, strlen(prefix)); out += strlen(prefix);
    *out++ = '\'';
    for (cursor = path; *cursor; cursor++) {
        if (*cursor == '\'') { memcpy(out, "'\\''", 4); out += 4; }
        else *out++ = *cursor;
    }
    *out++ = '\'';
    *out = '\0';
    return command;
}

/* Quote one argument for the local shell that invokes ssh. */
static char *shell_quote(const char *value) {
    return remote_command_with_path("", value);
}

/* Build a local shell command: `prefix`, a properly single-quoted `path`, then
 * `suffix`. Embedded single quotes in `path` are escaped ('\''), so a filename
 * can never terminate the quoted argument early and inject into the shell.
 * Returns a malloc'd string the caller must free, or NULL on allocation
 * failure. */
static char *local_command_with_path(const char *prefix, const char *path, const char *suffix) {
    char *command = remote_command_with_path(prefix, path);
    char *grown;
    size_t total;
    if (!command) return NULL;
    total = strlen(command) + strlen(suffix) + 1;
    grown = realloc(command, total);
    if (!grown) { free(command); return NULL; }
    strcat(grown, suffix);
    return grown;
}

/* Parse key=value format output from mediainfo */
static void parse_mediainfo_line(const char *line, AudioMetadata *metadata) {
    char key[256], value[256];
    if (sscanf(line, "%255[^=]=%255[^\n]", key, value) != 2) return;
    trim_string(key);
    trim_string(value);
    if (strcmp(key, "Title") == 0 && !metadata->title) {
        metadata->title = malloc(strlen(value) + 1);
        if (metadata->title) strcpy(metadata->title, value);
    } else if (strcmp(key, "Performer") == 0 && !metadata->artist) {
        metadata->artist = malloc(strlen(value) + 1);
        if (metadata->artist) strcpy(metadata->artist, value);
    } else if (strcmp(key, "Album") == 0 && !metadata->album) {
        metadata->album = malloc(strlen(value) + 1);
        if (metadata->album) strcpy(metadata->album, value);
    }
}

static void parse_ffprobe_line(const char *line, AudioMetadata *metadata) {
    char key[256], value[256];
    const char *tag;

    if (sscanf(line, "%255[^=]=%255[^\n]", key, value) != 2) return;
    trim_string(key);
    trim_string(value);
    tag = strncasecmp(key, "TAG:", 4) == 0 ? key + 4 : key;
    if (strcasecmp(tag, "title") == 0 && !metadata->title) {
        metadata->title = strdup(value);
    } else if (strcasecmp(tag, "artist") == 0 && !metadata->artist) {
        metadata->artist = strdup(value);
    } else if (strcasecmp(tag, "album") == 0 && !metadata->album) {
        metadata->album = strdup(value);
    }
}

/* Extract metadata using mediainfo command */
static int extract_with_mediainfo(const char *filepath, AudioMetadata *metadata, char *error, size_t error_size) {
    FILE *pipe;
    char *command;
    char line[512];

    /* --Output uses %Title% etc. inside single quotes; the path is escaped so
     * an embedded single quote cannot break out of the argument. */
    command = local_command_with_path(
        "mediainfo --Output='General;Title=%Title% \\nPerformer=%Performer% \\nAlbum=%Album%' -- ",
        filepath, " 2>/dev/null");
    if (!command) {
        set_error(error, error_size, "Command path too long");
        return -1;
    }

    pipe = popen(command, "r");
    free(command);
    if (!pipe) {
        set_error(error, error_size, "Could not execute mediainfo");
        return -1;
    }

    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (*line) parse_mediainfo_line(line, metadata);
    }
    pclose(pipe);
    return (metadata->title || metadata->artist || metadata->album) ? 1 : 0;
}

/* Extract metadata using ffprobe command */
static int extract_with_ffprobe(const char *filepath, AudioMetadata *metadata, char *error, size_t error_size) {
    FILE *pipe;
    char *command;
    char line[512];

    command = local_command_with_path(
        "ffprobe -v error -show_entries format_tags -of default=noprint_wrappers=1 ",
        filepath, " 2>/dev/null");
    if (!command) {
        set_error(error, error_size, "Command path too long");
        return -1;
    }

    pipe = popen(command, "r");
    free(command);
    if (!pipe) {
        set_error(error, error_size, "Could not execute ffprobe");
        return -1;
    }

    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (*line) parse_ffprobe_line(line, metadata);
    }
    pclose(pipe);
    return (metadata->title || metadata->artist || metadata->album) ? 1 : 0;
}

int metadata_extract_from_file(const char *filepath, AudioMetadata *metadata, char *error, size_t error_size) {
    int result;

    if (!filepath || !metadata) {
        set_error(error, error_size, "Invalid parameters");
        return -1;
    }

    if (access(filepath, F_OK) != 0) {
        set_error(error, error_size, "File not found");
        return 0;
    }

    memset(metadata, 0, sizeof(*metadata));

    /* Try mediainfo first (more reliable for various formats) */
    result = extract_with_mediainfo(filepath, metadata, error, error_size);
    if (result > 0) return 1;

    /* Fall back to ffprobe */
    memset(metadata, 0, sizeof(*metadata));
    result = extract_with_ffprobe(filepath, metadata, error, error_size);
    if (result > 0) return 1;

    set_error(error, error_size, "Could not extract metadata (mediainfo and ffprobe not available)");
    return -1;
}

/* Extract metadata from remote file via SSH */
static int extract_ssh_metadata_with_tool(const char *username, const char *ip, const char *filepath,
                                          AudioMetadata *metadata, const char *tool, char *error, size_t error_size) {
    FILE *pipe;
    char command[2048];
    char line[512];
    int result;
    char *remote_cmd = NULL;
    char *quoted_remote_cmd = NULL;

    /* Verify SSH credentials format */
    if (!username || !*username || !ip || !*ip || !filepath || !*filepath) {
        set_error(error, error_size, "Invalid SSH parameters");
        return -1;
    }

    /* Check if username/ip are valid for shell (prevent injection) */
    if (!valid_ssh_name(username, 0) || !valid_ssh_name(ip, 1)) {
        set_error(error, error_size, "Invalid SSH username or IP format");
        return -1;
    }

    /* Both tools read the remote file by path over ssh (the "--" is theirs). */
    if (strcmp(tool, "mediainfo") == 0) {
        remote_cmd = remote_command_with_path(
            "mediainfo --Output='General;Title=%Title% \\nPerformer=%Performer% \\nAlbum=%Album%' -- ",
            filepath);
    } else if (strcmp(tool, "ffprobe") == 0) {
        remote_cmd = remote_command_with_path(
            "ffprobe -v error -show_entries format_tags -of default=noprint_wrappers=1 -- ",
            filepath);
    } else {
        set_error(error, error_size, "Unknown metadata tool");
        return -1;
    }
    if (!remote_cmd) {
        set_error(error, error_size, "Path too long");
        return -1;
    }

    quoted_remote_cmd = shell_quote(remote_cmd);
    if (!quoted_remote_cmd) {
        free(remote_cmd);
        set_error(error, error_size, "Command too long");
        return -1;
    }

    result = snprintf(command, sizeof(command),
        "ssh -F /dev/null" SSH_HARDENING_OPTS_STR "%s -- %s@%s %s 2>/dev/null",
        ssh_opts, username, ip, quoted_remote_cmd);
    free(quoted_remote_cmd);

    if (result >= (int)sizeof(command)) {
        free(remote_cmd);
        set_error(error, error_size, "Command too long");
        return -1;
    }

    pipe = popen(command, "r");
    if (remote_cmd) free(remote_cmd);
    if (!pipe) {
        set_error(error, error_size, "Could not execute SSH command");
        return -1;
    }

    if (strcmp(tool, "mediainfo") == 0) {
        while (fgets(line, sizeof(line), pipe)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (*line) parse_mediainfo_line(line, metadata);
        }
    } else {
        while (fgets(line, sizeof(line), pipe)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (*line) parse_ffprobe_line(line, metadata);
        }
    }

    pclose(pipe);
    return (metadata->title || metadata->artist || metadata->album) ? 1 : 0;
}

int metadata_extract_from_ssh(const char *username, const char *ip, const char *filepath,
                              AudioMetadata *metadata, char *error, size_t error_size) {
    int result;
    char tool_error[256] = {0};

    if (!username || !ip || !filepath || !metadata) {
        set_error(error, error_size, "Invalid parameters");
        return -1;
    }

    memset(metadata, 0, sizeof(*metadata));

    /* Try mediainfo first */
    result = extract_ssh_metadata_with_tool(username, ip, filepath, metadata, "mediainfo", tool_error, sizeof(tool_error));
    if (result > 0) return 1;

    /* Fall back to ffprobe */
    memset(metadata, 0, sizeof(*metadata));
    result = extract_ssh_metadata_with_tool(username, ip, filepath, metadata, "ffprobe", tool_error, sizeof(tool_error));
    if (result > 0) return 1;

    /* Provide detailed error based on last tool attempt */
    if (strstr(tool_error, "Invalid SSH") || strstr(tool_error, "Invalid parameters")) {
        set_error(error, error_size, "%s", tool_error);
    } else if (strstr(tool_error, "Command too long") || strstr(tool_error, "Path too long")) {
        set_error(error, error_size, "%s", tool_error);
    } else if (strstr(tool_error, "Could not execute")) {
        set_error(error, error_size, "SSH connection failed. Check SSH key, username, IP, and firewall.");
    } else {
        set_error(error, error_size, "Remote metadata extraction failed. Ensure mediainfo/ffprobe installed on remote system and file is readable.");
    }
    return -1;
}

int metadata_extract_from_url(const char *url, AudioMetadata *metadata, char *error, size_t error_size) {
    FILE *pipe;
    char *command;
    char line[512];

    if (!url || !metadata) {
        set_error(error, error_size, "Invalid parameters");
        return -1;
    }

    /* Only HTTP(S) URLs can be probed directly. */
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        set_error(error, error_size, "Only http:// or https:// URLs are supported");
        return -1;
    }

    memset(metadata, 0, sizeof(*metadata));

    command = local_command_with_path(
        /* 5s socket timeout: adding an https source runs this inline on the
         * main loop, so a slow/unreachable URL must not stall the widget. */
        "ffprobe -v error -rw_timeout 5000000 -timeout 5000000 -show_entries format_tags -of default=noprint_wrappers=1 ",
        url, " 2>/dev/null");
    if (!command) {
        set_error(error, error_size, "Command path too long");
        return -1;
    }

    pipe = popen(command, "r");
    free(command);
    if (!pipe) {
        set_error(error, error_size, "Could not execute ffprobe");
        return -1;
    }

    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (*line) parse_ffprobe_line(line, metadata);
    }
    pclose(pipe);
    return (metadata->title || metadata->artist || metadata->album) ? 1 : 0;
}

void metadata_destroy(AudioMetadata *metadata) {
    if (!metadata) return;
    free(metadata->title);
    free(metadata->artist);
    free(metadata->album);
    memset(metadata, 0, sizeof(*metadata));
}

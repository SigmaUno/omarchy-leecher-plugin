/*
 * library_handler.c
 *
 * A small JSON-lines service that resolves song metadata to entries in a
 * library JSON file.  It deliberately has no UI or network dependencies so
 * both the SDL app and music-ripper can use the same protocol.
 *
 * Build: cc -std=c11 -Wall -Wextra -Wpedantic -O2 library_handler.c -o library-handler
 * Run:   ./library-handler --library library.json
 */

#define _POSIX_C_SOURCE 200809L

#include "library_handler.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Minimal embedded JSON tokenizer (jsmn, MIT licensed). */
typedef enum { JSMN_UNDEFINED, JSMN_OBJECT, JSMN_ARRAY, JSMN_STRING, JSMN_PRIMITIVE } jsmntype_t;
typedef struct { jsmntype_t type; int start; int end; int size; int parent; } jsmntok_t;
typedef struct { unsigned int pos; unsigned int toknext; int toksuper; } jsmn_parser;

struct LibraryHandler {
    char *json;
    jsmntok_t *tokens;
    int token_count;
    int tracks_token;
};

static void jsmn_init(jsmn_parser *parser) { parser->pos = 0; parser->toknext = 0; parser->toksuper = -1; }
static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens, size_t count) {
    jsmntok_t *token;
    if (parser->toknext >= count) return NULL;
    token = &tokens[parser->toknext++];
    token->start = token->end = -1; token->size = 0; token->parent = -1; token->type = JSMN_UNDEFINED;
    return token;
}
static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type, int start, int end) {
    token->type = type; token->start = start; token->end = end; token->size = 0;
}
static int jsmn_parse_primitive(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, size_t count) {
    int start = (int)parser->pos;
    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        if (c == '\t' || c == '\r' || c == '\n' || c == ' ' || c == ',' || c == ']' || c == '}') {
            jsmntok_t *token = jsmn_alloc_token(parser, tokens, count);
            if (!token) { parser->pos = (unsigned int)start; return -1; }
            jsmn_fill_token(token, JSMN_PRIMITIVE, start, (int)parser->pos); token->parent = parser->toksuper;
            parser->pos--; return 0;
        }
        if ((unsigned char)c < 32 || (unsigned char)c >= 127) { parser->pos = (unsigned int)start; return -2; }
    }
    jsmntok_t *token = jsmn_alloc_token(parser, tokens, count);
    if (!token) { parser->pos = (unsigned int)start; return -1; }
    jsmn_fill_token(token, JSMN_PRIMITIVE, start, (int)parser->pos); token->parent = parser->toksuper;
    parser->pos--; return 0;
}
static int jsmn_parse_string(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, size_t count) {
    int start = (int)parser->pos++;
    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        if (c == '"') {
            jsmntok_t *token = jsmn_alloc_token(parser, tokens, count);
            if (!token) { parser->pos = (unsigned int)start; return -1; }
            jsmn_fill_token(token, JSMN_STRING, start + 1, (int)parser->pos); token->parent = parser->toksuper;
            return 0;
        }
        if (c == '\\') {
            if (++parser->pos >= len) break;
            c = js[parser->pos];
            if (c == '"' || c == '/' || c == '\\' || c == 'b' || c == 'f' || c == 'r' || c == 'n' || c == 't') continue;
            if (c == 'u') { unsigned int i; for (i = 0; i < 4 && ++parser->pos < len && isxdigit((unsigned char)js[parser->pos]); i++) {} if (i == 4) { parser->pos--; continue; } }
            parser->pos = (unsigned int)start; return -2;
        }
    }
    parser->pos = (unsigned int)start; return -2;
}
static int jsmn_parse(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, size_t count) {
    unsigned int i;
    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos]; jsmntok_t *token;
        switch (c) {
        case '{': case '[':
            token = jsmn_alloc_token(parser, tokens, count); if (!token) return -1;
            if (parser->toksuper != -1) tokens[parser->toksuper].size++;
            token->type = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY); token->start = (int)parser->pos; token->parent = parser->toksuper; parser->toksuper = (int)parser->toknext - 1; break;
        case '}': case ']':
            for (i = parser->toknext; i > 0; i--) { token = &tokens[i - 1]; if (token->start != -1 && token->end == -1) { if ((token->type == JSMN_OBJECT && c == '}') || (token->type == JSMN_ARRAY && c == ']')) { token->end = (int)parser->pos + 1; parser->toksuper = token->parent; break; } return -2; } }
            if (i == 0) return -2;
            break;
        case '"':
            if (jsmn_parse_string(parser, js, len, tokens, count) < 0) return -1;
            if (parser->toksuper != -1) tokens[parser->toksuper].size++;
            break;
        case '\t': case '\r': case '\n': case ' ': case ':': case ',': break;
        default:
            if (jsmn_parse_primitive(parser, js, len, tokens, count) < 0) return -1;
            if (parser->toksuper != -1) tokens[parser->toksuper].size++;
            break;
        }
    }
    for (i = parser->toknext; i > 0; i--) if (tokens[i - 1].start != -1 && tokens[i - 1].end == -1) return -2;
    return (int)parser->toknext;
}

static int token_skip(const jsmntok_t *tokens, int count, int index) {
    int end = tokens[index].end;
    index++;
    while (index < count && tokens[index].start < end) index++;
    return index;
}
static int token_equals(const char *json, const jsmntok_t *token, const char *text) {
    size_t len = strlen(text);
    return token->type == JSMN_STRING && (size_t)(token->end - token->start) == len && !strncmp(json + token->start, text, len);
}
static int object_value(const char *json, const jsmntok_t *tokens, int count, int object, const char *key) {
    int i;
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT) return -1;
    for (i = object + 1; i < count && tokens[i].start < tokens[object].end; i = token_skip(tokens, count, i + 1)) {
        if (i + 1 < count && token_equals(json, &tokens[i], key)) return i + 1;
    }
    return -1;
}
static char *token_string(const char *json, const jsmntok_t *token) {
    size_t in_len, out_len = 0, i;
    char *out;
    if (!token || token->type != JSMN_STRING) return NULL;
    in_len = (size_t)(token->end - token->start);
    out = malloc(in_len + 1); if (!out) return NULL;
    for (i = 0; i < in_len; i++) {
        char c = json[token->start + (int)i];
        if (c == '\\' && i + 1 < in_len) {
            c = json[token->start + (int)++i];
            if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t';
            else if (c == 'b') c = '\b'; else if (c == 'f') c = '\f';
            else if (c == 'u') { i += 4; c = '?'; }
        }
        out[out_len++] = c;
    }
    out[out_len] = '\0'; return out;
}
static int string_score(const char *json, const jsmntok_t *token, const char *query) {
    char *value, *a, *b; int score = 0;
    if (!query || !*query || !token || token->type != JSMN_STRING) return 0;
    value = token_string(json, token); if (!value) return 0;
    for (a = value; *a; a++) *a = (char)tolower((unsigned char)*a);
    b = strdup(query); if (!b) { free(value); return 0; }
    for (a = b; *a; a++) *a = (char)tolower((unsigned char)*a);
    if (!strcmp(value, b)) score = 100;
    else if (strstr(value, b)) score = 70;
    else if (strstr(b, value)) score = 50;
    free(b); free(value); return score;
}
#ifdef LIBRARY_HANDLER_STANDALONE
static void print_json_string(const char *value) {
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    putchar('"');
    for (; *p; p++) { if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); } else if (*p == '\n') fputs("\\n", stdout); else if (*p == '\r') fputs("\\r", stdout); else if (*p == '\t') fputs("\\t", stdout); else if (*p < 32) printf("\\u%04x", *p); else putchar(*p); }
    putchar('"');
}
#endif
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb"); long size; char *buf;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    buf = malloc((size_t)size + 1); if (!buf) { fclose(file); return NULL; }
    if (fread(buf, 1, (size_t)size, file) != (size_t)size) { free(buf); fclose(file); return NULL; }
    buf[size] = '\0'; fclose(file); return buf;
}
static void set_error(char *error, size_t error_size, const char *message) {
    if (!error || !error_size) return;
    snprintf(error, error_size, "%s", message);
}
static char *object_string(const char *json, const jsmntok_t *tokens, int count, int object, const char *key) {
    int value = object_value(json, tokens, count, object, key);
    return value >= 0 ? token_string(json, &tokens[value]) : NULL;
}
static int source_kind(const char *kind, LibrarySourceKind *result) {
    if (!kind) return 0;
    if (!strcmp(kind, "local")) *result = LIBRARY_SOURCE_LOCAL;
    else if (!strcmp(kind, "ssh")) *result = LIBRARY_SOURCE_SSH;
    else if (!strcmp(kind, "https")) *result = LIBRARY_SOURCE_HTTPS;
    else if (!strcmp(kind, "network")) *result = LIBRARY_SOURCE_NETWORK;
    else return 0;
    return 1;
}
void library_handler_track_destroy(LibraryTrack *track) {
    size_t i;
    if (!track) return;
    free(track->id); free(track->title); free(track->artist); free(track->album);
    for (i = 0; i < track->source_count; i++) {
        free(track->sources[i].path); free(track->sources[i].username);
        free(track->sources[i].url); free(track->sources[i].ip);
    }
    free(track->sources);
    memset(track, 0, sizeof(*track));
}

LibraryHandler *library_handler_open(const char *library_path, char *error, size_t error_size) {
    LibraryHandler *handler;
    jsmn_parser parser;
    size_t capacity;
    if (!library_path) { set_error(error, error_size, "library path is required"); return NULL; }
    handler = calloc(1, sizeof(*handler));
    if (!handler) { set_error(error, error_size, "out of memory"); return NULL; }
    handler->json = read_file(library_path);
    if (!handler->json) { set_error(error, error_size, "cannot read library file"); free(handler); return NULL; }
    capacity = strlen(handler->json) + 16;
    handler->tokens = calloc(capacity, sizeof(*handler->tokens));
    if (!handler->tokens) { set_error(error, error_size, "out of memory"); library_handler_close(handler); return NULL; }
    jsmn_init(&parser);
    handler->token_count = jsmn_parse(&parser, handler->json, strlen(handler->json), handler->tokens, capacity);
    if (handler->token_count < 1 || handler->tokens[0].type != JSMN_OBJECT) { set_error(error, error_size, "library is not a valid JSON object"); library_handler_close(handler); return NULL; }
    handler->tracks_token = object_value(handler->json, handler->tokens, handler->token_count, 0, "tracks");
    if (handler->tracks_token < 0 || handler->tokens[handler->tracks_token].type != JSMN_ARRAY) { set_error(error, error_size, "library must contain a tracks array"); library_handler_close(handler); return NULL; }
    return handler;
}

void library_handler_close(LibraryHandler *handler) {
    if (!handler) return;
    free(handler->tokens); free(handler->json); free(handler);
}

static int copy_track(const LibraryHandler *handler, int best, LibraryTrack *track, char *error, size_t error_size) {
    int i;
    int sources;
    size_t source_count = 0, source_index = 0;
    memset(track, 0, sizeof(*track));
    track->id = object_string(handler->json, handler->tokens, handler->token_count, best, "id");
    track->title = object_string(handler->json, handler->tokens, handler->token_count, best, "title");
    track->artist = object_string(handler->json, handler->tokens, handler->token_count, best, "artist");
    track->album = object_string(handler->json, handler->tokens, handler->token_count, best, "album");
    sources = object_value(handler->json, handler->tokens, handler->token_count, best, "sources");
    if (sources < 0 || handler->tokens[sources].type != JSMN_ARRAY) return 1;
    for (i = sources + 1; i < handler->token_count && handler->tokens[i].start < handler->tokens[sources].end; i = token_skip(handler->tokens, handler->token_count, i)) if (handler->tokens[i].parent == sources && handler->tokens[i].type == JSMN_OBJECT) source_count++;
    track->sources = calloc(source_count, sizeof(*track->sources));
    if (source_count && !track->sources) { library_handler_track_destroy(track); set_error(error, error_size, "out of memory"); return -1; }
    track->source_count = source_count;
    for (i = sources + 1; i < handler->token_count && handler->tokens[i].start < handler->tokens[sources].end; i = token_skip(handler->tokens, handler->token_count, i)) {
        char *kind;
        LibrarySource *source;
        if (handler->tokens[i].parent != sources || handler->tokens[i].type != JSMN_OBJECT) continue;
        source = &track->sources[source_index++];
        kind = object_string(handler->json, handler->tokens, handler->token_count, i, "kind");
        if (!source_kind(kind, &source->kind)) { free(kind); library_handler_track_destroy(track); set_error(error, error_size, "source has an unknown kind"); return -1; }
        free(kind);
        source->path = object_string(handler->json, handler->tokens, handler->token_count, i, "PATH");
        source->username = object_string(handler->json, handler->tokens, handler->token_count, i, "USERNAME");
        source->url = object_string(handler->json, handler->tokens, handler->token_count, i, "URL");
        source->ip = object_string(handler->json, handler->tokens, handler->token_count, i, "IP");
    }
    return 1;
}

size_t library_handler_track_count(const LibraryHandler *handler) {
    int i;
    size_t count = 0;
    if (!handler) return 0;
    for (i = handler->tracks_token + 1; i < handler->token_count && handler->tokens[i].start < handler->tokens[handler->tracks_token].end; i = token_skip(handler->tokens, handler->token_count, i)) if (handler->tokens[i].parent == handler->tracks_token && handler->tokens[i].type == JSMN_OBJECT) count++;
    return count;
}

int library_handler_track_at(const LibraryHandler *handler, size_t index, LibraryTrack *track,
                             char *error, size_t error_size) {
    int i;
    size_t current = 0;
    if (!handler || !track) { set_error(error, error_size, "handler and track are required"); return -1; }
    for (i = handler->tracks_token + 1; i < handler->token_count && handler->tokens[i].start < handler->tokens[handler->tracks_token].end; i = token_skip(handler->tokens, handler->token_count, i)) {
        if (handler->tokens[i].parent != handler->tracks_token || handler->tokens[i].type != JSMN_OBJECT) continue;
        if (current++ == index) return copy_track(handler, i, track, error, error_size);
    }
    return 0;
}

int library_handler_resolve(const LibraryHandler *handler, const LibrarySongQuery *query,
                            LibraryTrack *track, char *error, size_t error_size) {
    int i, best = -1, best_score = 0;
    if (!handler || !query || !track) { set_error(error, error_size, "handler, query, and track are required"); return -1; }
    if ((!query->id || !*query->id) && (!query->title || !*query->title)) { set_error(error, error_size, "song requires id or title"); return -1; }
    for (i = handler->tracks_token + 1; i < handler->token_count && handler->tokens[i].start < handler->tokens[handler->tracks_token].end; i = token_skip(handler->tokens, handler->token_count, i)) {
        int score = 0, value;
        if (handler->tokens[i].parent != handler->tracks_token || handler->tokens[i].type != JSMN_OBJECT) continue;
        value = object_value(handler->json, handler->tokens, handler->token_count, i, "id"); if (query->id && value >= 0 && string_score(handler->json, &handler->tokens[value], query->id) == 100) score += 1000;
        value = object_value(handler->json, handler->tokens, handler->token_count, i, "title"); score += string_score(handler->json, value >= 0 ? &handler->tokens[value] : NULL, query->title);
        value = object_value(handler->json, handler->tokens, handler->token_count, i, "artist"); score += string_score(handler->json, value >= 0 ? &handler->tokens[value] : NULL, query->artist);
        value = object_value(handler->json, handler->tokens, handler->token_count, i, "album"); score += string_score(handler->json, value >= 0 ? &handler->tokens[value] : NULL, query->album);
        if (score > best_score) { best_score = score; best = i; }
    }
    return best < 0 ? 0 : copy_track(handler, best, track, error, error_size);
}

static int append_text(char *buffer, size_t capacity, size_t *length, const char *text) {
    size_t text_length = strlen(text);
    if (*length > capacity - text_length - 1) return 0;
    memcpy(buffer + *length, text, text_length);
    *length += text_length;
    buffer[*length] = '\0';
    return 1;
}
static int append_json_string(char *buffer, size_t capacity, size_t *length, const char *value) {
    const unsigned char *p = (const unsigned char *)value;
    if (!append_text(buffer, capacity, length, "\"")) return 0;
    for (; *p; p++) {
        char escaped[7];
        if (*p == '"' || *p == '\\') { escaped[0] = '\\'; escaped[1] = (char)*p; escaped[2] = '\0'; }
        else if (*p < 32) { snprintf(escaped, sizeof(escaped), "\\u%04x", *p); }
        else { escaped[0] = (char)*p; escaped[1] = '\0'; }
        if (!append_text(buffer, capacity, length, escaped)) return 0;
    }
    return append_text(buffer, capacity, length, "\"");
}
static int append_json_value(char *buffer, size_t capacity, size_t *length, const char *value) {
    return value ? append_json_string(buffer, capacity, length, value) : append_text(buffer, capacity, length, "null");
}
static const char *source_kind_name(LibrarySourceKind kind) {
    switch (kind) {
    case LIBRARY_SOURCE_LOCAL: return "local";
    case LIBRARY_SOURCE_SSH: return "ssh";
    case LIBRARY_SOURCE_HTTPS: return "https";
    case LIBRARY_SOURCE_NETWORK: return "network";
    }
    return NULL;
}
static int source_is_complete(const LibrarySource *source) {
    if (!source) return 0;
    switch (source->kind) {
    case LIBRARY_SOURCE_LOCAL: return source->path && *source->path;
    case LIBRARY_SOURCE_SSH: return source->path && *source->path && source->username && *source->username && source->ip && *source->ip;
    case LIBRARY_SOURCE_HTTPS: return source->url && *source->url;
    case LIBRARY_SOURCE_NETWORK: return source->path && *source->path && source->username && *source->username && source->ip && *source->ip;
    }
    return 0;
}
static char *source_json(const LibrarySource *source) {
    size_t capacity, length = 0;
    char *json;
    const char *kind = source_kind_name(source->kind);
    if (!kind) return NULL;
    capacity = 256 + 6 * ((source->path ? strlen(source->path) : 0) + (source->username ? strlen(source->username) : 0) + (source->url ? strlen(source->url) : 0) + (source->ip ? strlen(source->ip) : 0));
    json = calloc(capacity, 1); if (!json) return NULL;
    if (!append_text(json, capacity, &length, "{\"kind\":") || !append_json_string(json, capacity, &length, kind) || !append_text(json, capacity, &length, ",\"PATH\":") || !append_json_value(json, capacity, &length, source->path) || !append_text(json, capacity, &length, ",\"USERNAME\":") || !append_json_value(json, capacity, &length, source->username) || !append_text(json, capacity, &length, ",\"URL\":") || !append_json_value(json, capacity, &length, source->url) || !append_text(json, capacity, &length, ",\"IP\":") || !append_json_value(json, capacity, &length, source->ip) || !append_text(json, capacity, &length, "}")) { free(json); return NULL; }
    return json;
}
static int write_atomic(const char *path, const char *data, size_t size) {
    size_t template_length = strlen(path) + 16;
    char *temporary_path = malloc(template_length);
    FILE *file;
    int fd, ok;
    if (!temporary_path) return 0;
    snprintf(temporary_path, template_length, "%s.tmp.XXXXXX", path);
    fd = mkstemp(temporary_path);
    if (fd < 0) { free(temporary_path); return 0; }
    file = fdopen(fd, "wb");
    if (!file) { close(fd); unlink(temporary_path); free(temporary_path); return 0; }
    ok = fwrite(data, 1, size, file) == size;
    if (ok) ok = fflush(file) == 0;
    if (ok) ok = fsync(fd) == 0;
    if (fclose(file) != 0) ok = 0;
    if (!ok) { unlink(temporary_path); free(temporary_path); return 0; }
    ok = rename(temporary_path, path) == 0;
    if (!ok) unlink(temporary_path);
    free(temporary_path);
    return ok;
}
int library_handler_add_source(const char *library_path, const LibrarySongQuery *song,
                               const LibrarySource *source, char *error, size_t error_size) {
    LibraryHandler *handler;
    char *entry, *new_json;
    int i, match = -1, insert_at, array_has_entries = 0;
    size_t new_size;
    if (!library_path || !song || !song->title || !*song->title || !song->artist || !*song->artist || !song->album || !*song->album || !source) { set_error(error, error_size, "song title, artist, album, and source are required"); return -1; }
    if (!source_is_complete(source)) { set_error(error, error_size, "source is missing a required transport field"); return -1; }
    entry = source_json(source);
    if (!entry) { set_error(error, error_size, "invalid source or out of memory"); return -1; }
    handler = library_handler_open(library_path, error, error_size);
    if (!handler) { free(entry); return -1; }
    for (i = handler->tracks_token + 1; i < handler->token_count && handler->tokens[i].start < handler->tokens[handler->tracks_token].end; i = token_skip(handler->tokens, handler->token_count, i)) {
        int title, artist;
        if (handler->tokens[i].parent != handler->tracks_token || handler->tokens[i].type != JSMN_OBJECT) continue;
        title = object_value(handler->json, handler->tokens, handler->token_count, i, "title");
        artist = object_value(handler->json, handler->tokens, handler->token_count, i, "artist");
        {
            int album = object_value(handler->json, handler->tokens, handler->token_count, i, "album");
            if (title >= 0 && artist >= 0 && album >= 0 && string_score(handler->json, &handler->tokens[title], song->title) == 100 && string_score(handler->json, &handler->tokens[artist], song->artist) == 100 && string_score(handler->json, &handler->tokens[album], song->album) == 100) { match = i; break; }
        }
    }
    if (match >= 0) {
        int sources = object_value(handler->json, handler->tokens, handler->token_count, match, "sources");
        if (sources < 0 || handler->tokens[sources].type != JSMN_ARRAY) { set_error(error, error_size, "matching track has no sources array"); library_handler_close(handler); free(entry); return -1; }
        insert_at = handler->tokens[sources].end - 1;
        array_has_entries = handler->tokens[sources].size > 0;
        new_size = strlen(handler->json) + strlen(entry) + (array_has_entries ? 1 : 0) + 1;
        new_json = malloc(new_size);
        if (new_json) snprintf(new_json, new_size, "%.*s%s%s%s", insert_at, handler->json, array_has_entries ? "," : "", entry, handler->json + insert_at);
    } else {
        const jsmntok_t *tracks = &handler->tokens[handler->tracks_token];
        char *track_entry;
        size_t capacity = strlen(entry) + 6 * (strlen(song->title) + (song->artist ? strlen(song->artist) : 0) + (song->album ? strlen(song->album) : 0)) + 128;
        track_entry = calloc(capacity, 1);
        if (!track_entry) { library_handler_close(handler); free(entry); set_error(error, error_size, "out of memory"); return -1; }
        { size_t length = 0; append_text(track_entry, capacity, &length, "{\"title\":"); append_json_string(track_entry, capacity, &length, song->title); append_text(track_entry, capacity, &length, ",\"artist\":"); append_json_value(track_entry, capacity, &length, song->artist); append_text(track_entry, capacity, &length, ",\"album\":"); append_json_value(track_entry, capacity, &length, song->album); append_text(track_entry, capacity, &length, ",\"sources\":["); append_text(track_entry, capacity, &length, entry); append_text(track_entry, capacity, &length, "]}"); }
        insert_at = tracks->end - 1; array_has_entries = tracks->size > 0;
        new_size = strlen(handler->json) + strlen(track_entry) + (array_has_entries ? 1 : 0) + 1;
        new_json = malloc(new_size);
        if (new_json) snprintf(new_json, new_size, "%.*s%s%s%s", insert_at, handler->json, array_has_entries ? "," : "", track_entry, handler->json + insert_at);
        free(track_entry);
    }
    library_handler_close(handler); free(entry);
    if (!new_json) { set_error(error, error_size, "out of memory"); return -1; }
    if (!write_atomic(library_path, new_json, strlen(new_json))) { free(new_json); set_error(error, error_size, "could not atomically write library"); return -1; }
    free(new_json); return 1;
}

static char *json_quote_alloc(const char *value) {
    size_t capacity = 2 + strlen(value == NULL ? "" : value) * 6 + 1, length = 0;
    char *buffer = calloc(capacity, 1);
    if (!buffer) return NULL;
    append_json_string(buffer, capacity, &length, value == NULL ? "" : value);
    return buffer;
}

typedef struct { int start; int end; char *slice; } TrackEdit;

static void track_edits_free(TrackEdit *edits, int how_many) {
    int i;
    for (i = 0; i < how_many; i++) free(edits[i].slice);
}

static void track_edits_sort(TrackEdit *edits, int how_many) {
    int a, b;
    for (a = 1; a < how_many; a++) {
        TrackEdit key = edits[a];
        for (b = a; b > 0 && edits[b - 1].start > key.start; b--) edits[b] = edits[b - 1];
        edits[b] = key;
    }
}

int library_handler_update_track(const char *library_path, size_t track_index,
                                 const char *title, const char *artist, const char *album,
                                 char *error, size_t error_size) {
    LibraryHandler *handler;
    int count, object, track_object = -1;
    size_t current = 0, cursor = 0, how_many = 0, i;
    char *new_json;
    TrackEdit edits[3];
    const char *fields[3] = { title, artist, album };
    const char *keys[3] = { "title", "artist", "album" };
    if (!library_path) { set_error(error, error_size, "library path is required"); return -1; }
    if (!title && !artist && !album) { set_error(error, error_size, "nothing to update"); return -1; }
    handler = library_handler_open(library_path, error, error_size);
    if (!handler) return -1;
    count = handler->token_count; object = handler->tracks_token;
    for (i = (size_t)object + 1; i < (size_t)count && handler->tokens[i].start < handler->tokens[object].end; i = (size_t)token_skip(handler->tokens, count, (int)i)) {
        if (handler->tokens[i].parent != object || handler->tokens[i].type != JSMN_OBJECT) continue;
        if (current++ == track_index) { track_object = (int)i; break; }
    }
    if (track_object < 0) { set_error(error, error_size, "track index out of range"); library_handler_close(handler); return -1; }
    {
        char *insert_combined = NULL; /* combined `,"k":"v","k2":"v2"` for missing fields */
        size_t insert_len = 0, insert_cap = 0, j;
        for (i = 0; i < 3; i++) {
            int value_token;
            if (!fields[i]) continue;
            value_token = object_value(handler->json, handler->tokens, count, track_object, keys[i]);
            if (value_token >= 0) {
                edits[how_many].slice = json_quote_alloc(fields[i]);
                if (!edits[how_many].slice) { track_edits_free(edits, (int)how_many); library_handler_close(handler); set_error(error, error_size, "out of memory"); return -1; }
                edits[how_many].start = handler->tokens[value_token].start - 1;
                edits[how_many].end = handler->tokens[value_token].end + 1;
                how_many++;
            } else {
                /* Missing field: append `,"key":"value"` to the combined insertion block. */
                char *value = json_quote_alloc(fields[i]);
                size_t add = strlen(keys[i]) + (value ? strlen(value) : 0) + 4; /* ":" plus comma + quotes around key */
                if (!value) { track_edits_free(edits, (int)how_many); free(insert_combined); library_handler_close(handler); set_error(error, error_size, "out of memory"); return -1; }
                if (insert_len + add + 1 > insert_cap) { char *grown; insert_cap = (insert_len + add + 1) * 2 + 8; grown = realloc(insert_combined, insert_cap); if (!grown) { free(value); track_edits_free(edits, (int)how_many); free(insert_combined); library_handler_close(handler); set_error(error, error_size, "out of memory"); return -1; } insert_combined = grown; }
                append_text(insert_combined, insert_cap, &insert_len, ",");
                append_text(insert_combined, insert_cap, &insert_len, "\"");
                append_text(insert_combined, insert_cap, &insert_len, keys[i]);
                append_text(insert_combined, insert_cap, &insert_len, "\":");
                append_text(insert_combined, insert_cap, &insert_len, value);
                free(value);
            }
        }
        if (insert_combined) {
            /* All missing fields inserted in one edit just before the closing brace. */
            edits[how_many].slice = insert_combined;
            edits[how_many].start = handler->tokens[track_object].end - 1;
            edits[how_many].end = handler->tokens[track_object].end - 1;
            how_many++;
        }
        track_edits_sort(edits, (int)how_many);
        {
            size_t total = strlen(handler->json) + 1;
            for (j = 0; j < how_many; j++) total += strlen(edits[j].slice);
            new_json = calloc(total, 1);
            if (!new_json) { track_edits_free(edits, (int)how_many); library_handler_close(handler); set_error(error, error_size, "out of memory"); return -1; }
        }
    }
    {
        size_t out = 0, j;
            for (j = 0; j < how_many; j++) {
                size_t gap = (size_t)edits[j].start - cursor;
                memcpy(new_json + out, handler->json + cursor, gap); out += gap;
                memcpy(new_json + out, edits[j].slice, strlen(edits[j].slice)); out += strlen(edits[j].slice);
                cursor = (size_t)edits[j].end;
            }
            strcpy(new_json + out, handler->json + cursor);
    }
    track_edits_free(edits, (int)how_many);
    library_handler_close(handler);
    if (!write_atomic(library_path, new_json, strlen(new_json))) { free(new_json); set_error(error, error_size, "could not atomically write library"); return -1; }
    free(new_json);
    return 1;
}

int library_handler_remove_track(const char *library_path, size_t track_index,
                                 char *error, size_t error_size) {
    LibraryHandler *handler;
    int count, object, track_object = -1;
    size_t current = 0, i;
    int remove_start, remove_end, k;
    char *new_json;
    if (!library_path) { set_error(error, error_size, "library path is required"); return -1; }
    handler = library_handler_open(library_path, error, error_size);
    if (!handler) return -1;
    count = handler->token_count; object = handler->tracks_token;
    for (i = (size_t)object + 1; i < (size_t)count && handler->tokens[i].start < handler->tokens[object].end; i = (size_t)token_skip(handler->tokens, count, (int)i)) {
        if (handler->tokens[i].parent != object || handler->tokens[i].type != JSMN_OBJECT) continue;
        if (current++ == track_index) { track_object = (int)i; break; }
    }
    if (track_object < 0) { set_error(error, error_size, "track index out of range"); library_handler_close(handler); return -1; }
    remove_start = handler->tokens[track_object].start;
    remove_end = handler->tokens[track_object].end;
    /* Consume the comma separating this element from the previous one, if any. */
    for (k = remove_start - 1; k >= 0 && (handler->json[k] == ' ' || handler->json[k] == '\t' || handler->json[k] == '\n' || handler->json[k] == '\r'); k--) {}
    if (k >= 0 && handler->json[k] == ',') remove_start = k;
    else {
        /* This was the first element: instead consume the trailing comma so the
         * following element does not end up with a leading comma. */
        k = remove_end;
        while (handler->json[k] == ' ' || handler->json[k] == '\t' || handler->json[k] == '\n' || handler->json[k] == '\r') k++;
        if (handler->json[k] == ',') remove_end = k + 1;
    }
    {
        size_t head = (size_t)remove_start;
        const char *tail = handler->json + remove_end;
        size_t total = head + strlen(tail) + 1;
        new_json = malloc(total);
        if (!new_json) { library_handler_close(handler); set_error(error, error_size, "out of memory"); return -1; }
        memcpy(new_json, handler->json, head);
        strcpy(new_json + head, tail);
    }
    library_handler_close(handler);
    if (!write_atomic(library_path, new_json, strlen(new_json))) { free(new_json); set_error(error, error_size, "could not atomically write library"); return -1; }
    free(new_json);
    return 1;
}

#ifdef LIBRARY_HANDLER_STANDALONE
static void respond_error(const char *request_id, const char *message) {
    fputs("{\"request_id\":", stdout); print_json_string(request_id); fputs(",\"ok\":false,\"error\":", stdout); print_json_string(message); puts("}");
}

static void handle_request(const char *library, const jsmntok_t *lt, int lc, const char *request) {
    jsmn_parser parser; jsmntok_t *rt; int rc, action_i, id_i, song_i, tracks_i, i, best = -1, best_score = 0;
    char *action = NULL, *request_id = NULL, *query_id = NULL, *title = NULL, *artist = NULL, *album = NULL;
    size_t capacity = strlen(request) + 16;
    rt = calloc(capacity, sizeof(*rt)); if (!rt) { respond_error(NULL, "out of memory"); return; }
    jsmn_init(&parser); rc = jsmn_parse(&parser, request, strlen(request), rt, capacity);
    if (rc < 1 || rt[0].type != JSMN_OBJECT) { respond_error(NULL, "request must be a JSON object"); free(rt); return; }
    id_i = object_value(request, rt, rc, 0, "request_id"); if (id_i >= 0) request_id = token_string(request, &rt[id_i]);
    action_i = object_value(request, rt, rc, 0, "action"); if (action_i >= 0) action = token_string(request, &rt[action_i]);
    song_i = object_value(request, rt, rc, 0, "song");
    if (!action || strcmp(action, "resolve") || song_i < 0 || rt[song_i].type != JSMN_OBJECT) { respond_error(request_id, "expected action=resolve and a song object"); goto done; }
    i = object_value(request, rt, rc, song_i, "id"); if (i >= 0) query_id = token_string(request, &rt[i]);
    i = object_value(request, rt, rc, song_i, "title"); if (i >= 0) title = token_string(request, &rt[i]);
    i = object_value(request, rt, rc, song_i, "artist"); if (i >= 0) artist = token_string(request, &rt[i]);
    i = object_value(request, rt, rc, song_i, "album"); if (i >= 0) album = token_string(request, &rt[i]);
    if ((!query_id || !*query_id) && (!title || !*title)) { respond_error(request_id, "song requires id or title"); goto done; }
    tracks_i = object_value(library, lt, lc, 0, "tracks");
    if (tracks_i < 0 || lt[tracks_i].type != JSMN_ARRAY) { respond_error(request_id, "library must contain a tracks array"); goto done; }
    for (i = tracks_i + 1; i < lc && lt[i].start < lt[tracks_i].end; i = token_skip(lt, lc, i)) {
        int score = 0, value;
        if (lt[i].parent != tracks_i || lt[i].type != JSMN_OBJECT) continue;
        value = object_value(library, lt, lc, i, "id"); if (query_id && value >= 0 && string_score(library, &lt[value], query_id) == 100) score += 1000;
        value = object_value(library, lt, lc, i, "title"); score += string_score(library, value >= 0 ? &lt[value] : NULL, title);
        value = object_value(library, lt, lc, i, "artist"); score += string_score(library, value >= 0 ? &lt[value] : NULL, artist);
        value = object_value(library, lt, lc, i, "album"); score += string_score(library, value >= 0 ? &lt[value] : NULL, album);
        if (score > best_score) { best_score = score; best = i; }
    }
    fputs("{\"request_id\":", stdout); print_json_string(request_id); fputs(",\"ok\":true,\"found\":", stdout);
    if (best < 0) { puts("false}"); goto done; }
    fputs("true,\"track\":", stdout); fwrite(library + lt[best].start, 1, (size_t)(lt[best].end - lt[best].start), stdout); puts("}");
done:
    free(action); free(request_id); free(query_id); free(title); free(artist); free(album); free(rt);
}
#endif

#ifdef LIBRARY_HANDLER_STANDALONE
int main(int argc, char **argv) {
    char *library, *line = NULL; size_t line_cap = 0; ssize_t line_len; jsmn_parser parser; jsmntok_t *tokens; int count; size_t capacity;
    if (argc != 3 || strcmp(argv[1], "--library")) { fprintf(stderr, "Usage: %s --library library.json\\n", argv[0]); return 64; }
    library = read_file(argv[2]); if (!library) { fprintf(stderr, "Cannot read %s: %s\\n", argv[2], strerror(errno)); return 66; }
    capacity = strlen(library) + 16; tokens = calloc(capacity, sizeof(*tokens)); if (!tokens) { free(library); return 70; }
    jsmn_init(&parser); count = jsmn_parse(&parser, library, strlen(library), tokens, capacity);
    if (count < 1 || tokens[0].type != JSMN_OBJECT) { fprintf(stderr, "Library is not a valid JSON object\\n"); free(tokens); free(library); return 65; }
    while ((line_len = getline(&line, &line_cap, stdin)) >= 0) {
        if (line_len > 0 && line[line_len - 1] == '\n') line[line_len - 1] = '\0';
        if (*line) handle_request(library, tokens, count, line);
        fflush(stdout);
    }
    free(line); free(tokens); free(library); return 0;
}
#endif

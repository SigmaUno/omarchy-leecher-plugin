#define _DEFAULT_SOURCE
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_RENDERER_IMPLEMENTATION

#include "library_handler.h"
#include "music_ripper.h"
#include "assembler.h"
#include "decoder.h"
#include "metadata.h"
#include "third_party/nuklear/nuklear.h"
#include "third_party/nuklear/nuklear_sdl_renderer.h"

#include <SDL.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef enum { SOURCE_LOCAL, SOURCE_SSH, SOURCE_HTTPS, SOURCE_NETWORK } SourceMethod;

#define LIBRARY_NAME_MAX      96    /* playlist stem, incl. NUL (names capped at 64) */
#define LIBRARY_DIR_MAX       512   /* library directory path, incl. NUL */
#define LIBRARY_PATH_MAX      640   /* "<dir>/<name>.json" always fits */
#define LIBRARY_MAX_PLAYLISTS 64

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    char path[512];
    char username[128];
    char url[512];
    char ip[64];
    char status[256];
    SourceMethod method;
    size_t selected_track;
    unsigned long last_cmd_id;
    SDL_AudioDeviceID audio_device;
    DecoderSource *decoder;
    Uint64 audio_queued_frame;
    Uint64 audio_total_frames;
    int audio_channels;
    int audio_rate;
    int is_playing;
    Uint32 position_ms;
    Uint32 last_position_ms;
    int position_stalls;
    Uint32 last_audio_retry_ms;
    int audio_retries;
    int no_output_ticks;
    Uint32 duration_ms;
    int track_ended;
    int stream_open;
    int stream_eof;
    Uint32 stream_target_ms;  /* when set, stream_audio() buffers this far ahead
                               * instead of the default -- used to cover a
                               * blocking fetch-thread join without an audio gap */
    pid_t ssh_agent_pid;
    char ssh_agent_dir[PATH_MAX];
    char ssh_agent_socket[PATH_MAX];
    char library_path[LIBRARY_PATH_MAX];  /* JSON file of the playlist VIEWED */
    char library_dir[LIBRARY_DIR_MAX];    /* directory holding the playlist .json files */
    char viewed_playlist[LIBRARY_NAME_MAX];  /* stem of the playlist shown in the widget */
    char playing_playlist[LIBRARY_NAME_MAX]; /* stem of the playlist playback runs over */
    char playlists[LIBRARY_MAX_PLAYLISTS][LIBRARY_NAME_MAX]; /* stems found in library_dir */
    int playlist_count;
    LibraryHandler *play_lib;     /* handle for playing_playlist (next/autoplay) */
    char cover_file[512];
    int autoplay;
    int shuffle;      /* autoplay/next picks a random track instead of the next */
    int repeat_one;   /* autoplay replays the current track instead of advancing */
    int volume;       /* software output gain, 0..100 */
    int muted;        /* force silence without losing the volume setting */
    char audio_device_name[128];  /* chosen SDL output; empty = system default */
    Uint32 resume_position_ms;    /* one-shot: seek here on the next commit_fetch */
    int resume_paused;            /* one-shot: start that track paused */
    Uint32 last_resume_write_ms;  /* throttle the resume-state file writes */
    MusicRipperTransports transports;
    pthread_mutex_t fetch_mutex;
    volatile int fetch_cancel;
    int fetch_active;
    int fetch_ready;
    size_t fetch_index;
    pthread_t fetch_thread;
    int fetch_thread_valid;
    DecoderSource *fetch_decoder;
    char fetch_title[128];
    char fetch_artist[128];
    char fetch_album[128];
    char fetch_cover[512];
    char fetch_error[256];  /* why the last fetch failed, for the skip notice */
    int immediate_pending;
    size_t immediate_index;
    int pending_valid;
    size_t pending_index;
    int autoplay_advancing;
    size_t play_queue[64];   /* ad-hoc "play next" order, consumed before library order */
    int play_queue_len;
} AppState;

/* Joins any in-flight background fetch thread before the library handler it
 * was given can be freed out from under it (see cancel_fetch's definition). */
static void cancel_fetch(AppState *s);
static void stream_audio(AppState *s);
static void join_fetch_thread(AppState *s);
static void resync_play_lib(AppState *s);

/* Per-user private IPC directory.  All status / control / cover / ssh-agent
 * files live here so that a multi-user host cannot read or clobber another
 * user's state through the old world-predictable /tmp paths.  Prefer
 * $XDG_RUNTIME_DIR (0700, owned by us) and fall back to a 0700 dir in /tmp
 * keyed by uid when it is unset (e.g. some headless launches). */
/* IPC paths are bounded and short: XDG_RUNTIME_DIR is at most a few dozen
 * bytes plus our fixed subdirectory, so 320 comfortably fits every consumer
 * buffer (the 512-byte cover/save buffers, PATH_MAX ssh-agent buffers, etc.). */
#define IPC_PATH_MAX 320
static char ipc_dir[IPC_PATH_MAX];
static char status_file[IPC_PATH_MAX];
static char control_file[IPC_PATH_MAX];
static char cover_file[IPC_PATH_MAX];

/* Monotonic per-process token so each committed cover file gets a unique name.
 * A name that depends only on the (stable) track index would alias two
 * different tracks that ever occupy the same index, showing stale art. */
static unsigned long cover_nonce;

static const char *ipc_path(char *buf, size_t size, const char *name) {
    const char *const dir = ipc_dir;
    snprintf(buf, size, "%s/%s", dir, name);
    return buf;
}

static void init_ipc_dir(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    struct stat st;
    char fallback[IPC_PATH_MAX];

    fallback[0] = '\0';
    if (runtime && runtime[0]) {
        const char *const base = runtime;
        snprintf(ipc_dir, sizeof(ipc_dir), "%s/leecher", base);
    } else {
        snprintf(ipc_dir, sizeof(ipc_dir), "/tmp/leecher-%lu", (unsigned long)getuid());
    }

    snprintf(fallback, sizeof(fallback), "/tmp/leecher-%lu-XXXXXX", (unsigned long)getuid());

    if (mkdir(ipc_dir, 0700) == 0) {
        /* Pre-existing directory owned by another user (or a symlink to one)
         * must never be trusted: fall back to a uniquely-named directory. */
        if (lstat(ipc_dir, &st) != 0 ||
            !S_ISDIR(st.st_mode) ||
            st.st_uid != getuid() ||
            (st.st_mode & 077) != 0) {
            if (mkdtemp(fallback)) {
                const char *const dir = fallback;
                snprintf(ipc_dir, sizeof(ipc_dir), "%s", dir);
            }
            else ipc_dir[0] = '\0';
        }
    } else if (errno != EEXIST || lstat(ipc_dir, &st) != 0 ||
               !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
               (st.st_mode & 077) != 0) {
        if (mkdtemp(fallback)) {
            const char *const dir = fallback;
            snprintf(ipc_dir, sizeof(ipc_dir), "%s", dir);
        }
        else { perror("leecher: cannot create IPC directory"); ipc_dir[0] = '\0'; }
    }

    ipc_path(status_file, sizeof(status_file), "status.json");
    ipc_path(control_file, sizeof(control_file), "control");
    ipc_path(cover_file, sizeof(cover_file), "cover.jpg");
}

/* Remove every committed cover file (cover-*.jpg, not the cover.jpg scratch)
 * from the IPC directory.  Called before writing a new one so the directory
 * holds at most a single cover file and never leaks stale art across sessions
 * or library edits. */
static void remove_cover_files(void) {
    DIR *dir = opendir(ipc_dir);
    struct dirent *entry;
    if (!dir) return;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "cover-", 6) == 0 &&
            strstr(entry->d_name, ".jpg") != NULL) {
            char path[IPC_PATH_MAX + 256];
            snprintf(path, sizeof(path), "%s/%s", ipc_dir, entry->d_name);
            unlink(path);
        }
    }
    closedir(dir);
}

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

typedef struct { unsigned char r, g, b; } RGB;
typedef struct {
    RGB background, darker, lighter, selection;
    RGB foreground, accent, muted, urgent;
} ThemePalette;

static char *json_escape(const char *value) {
    size_t cap = (value ? strlen(value) : 0) * 6 + 1, len = 0;
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    for (; *p; p++) {
        char esc[7];
        if (*p == '"' || *p == '\\') { esc[0] = '\\'; esc[1] = (char)*p; esc[2] = '\0'; }
        else if (*p == '\n') { esc[0] = '\\'; esc[1] = 'n'; esc[2] = '\0'; }
        else if (*p == '\r') { esc[0] = '\\'; esc[1] = 'r'; esc[2] = '\0'; }
        else if (*p == '\t') { esc[0] = '\\'; esc[1] = 't'; esc[2] = '\0'; }
        else if (*p < 32) { snprintf(esc, sizeof(esc), "\\u%04x", *p); }
        else { esc[0] = (char)*p; esc[1] = '\0'; }
        len += strlen(esc);
        if (len >= cap) { cap = len * 2 + 8; out = realloc(out, cap); if (!out) return NULL; }
        strcat(out + (len - strlen(esc)), esc);
    }
    out[len] = '\0';
    return out;
}

/* Write a file atomically: write to a temp inode then rename(2) over the
 * target.  A concurrent reader (the widget cat's + parses this file every
 * second) sees either the old complete content or the new complete content,
 * never a partially-written buffer. */
static int atomic_write(const char *path, const char *data, size_t size) {
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

static void write_status(const AppState *state) {
    /* mkstemp creates the temp file 0600; rename preserves it, so the status
     * file stays private even when the first write races with the unlink. */
    char *title = json_escape(state->title), *artist = json_escape(state->artist),
         *album = json_escape(state->album), *library = json_escape(state->library_path),
         *cover = json_escape(state->cover_file[0] ? state->cover_file : NULL),
         *status = json_escape(state->status),
         *output = json_escape(state->audio_device_name[0] ? state->audio_device_name : NULL);
    char *dir = json_escape(state->library_dir[0] ? state->library_dir : NULL),
         *viewed = json_escape(state->viewed_playlist),
         *playing = json_escape(state->playing_playlist);
    char tmp[16384];
    char queue[16 * 64 + 4];  /* "[" + up to 64 "NNNNN," + "]" */
    char outputs[2560];       /* enumerated output device names, JSON array */
    char playlists[64 * 100 + 4];  /* enumerated playlist stems, JSON array */
    int qn = 0, qi;
    int on = 0, od, ocount;
    int pn = 0, pi;
    int n;
    queue[qn++] = '[';
    for (qi = 0; qi < state->play_queue_len && qn < (int)sizeof(queue) - 16; qi++)
        qn += snprintf(queue + qn, sizeof(queue) - (size_t)qn, "%s%zu",
                       qi ? "," : "", state->play_queue[qi]);
    queue[qn++] = ']';
    queue[qn] = '\0';
    outputs[on++] = '[';
    ocount = SDL_GetNumAudioDevices(0);
    for (od = 0; od < ocount && on < (int)sizeof(outputs) - 260; od++) {
        const char *dn = SDL_GetAudioDeviceName(od, 0);
        char *de;
        if (!dn) continue;
        de = json_escape(dn);
        on += snprintf(outputs + on, sizeof(outputs) - (size_t)on, "%s\"%s\"",
                       od ? "," : "", de ? de : "");
        free(de);
    }
    outputs[on++] = ']';
    outputs[on] = '\0';
    playlists[pn++] = '[';
    for (pi = 0; pi < state->playlist_count && pn < (int)sizeof(playlists) - 200; pi++) {
        char *pe = json_escape(state->playlists[pi]);
        pn += snprintf(playlists + pn, sizeof(playlists) - (size_t)pn, "%s\"%s\"",
                       pi ? "," : "", pe ? pe : "");
        free(pe);
    }
    playlists[pn++] = ']';
    playlists[pn] = '\0';
    n = snprintf(tmp, sizeof(tmp),
                 "{\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
                 "\"position_ms\":%u,\"duration_ms\":%u,\"is_playing\":%s,\"track_index\":%zu,\"library\":\"%s\",\"autoplay\":%s,"
                 "\"shuffle\":%s,\"repeat_one\":%s,\"volume\":%d,\"muted\":%s,\"queue\":%s,"
                 "\"output\":\"%s\",\"outputs\":%s,\"cover\":\"%s\","
                 "\"library_dir\":\"%s\",\"playlists\":%s,\"viewed_playlist\":\"%s\",\"playing_playlist\":\"%s\","
                 "\"status\":\"%s\",\"cmd_id\":%lu}\n",
                 title ? title : "", artist ? artist : "", album ? album : "",
                 state->position_ms, state->duration_ms,
                 state->is_playing ? "true" : "false", state->selected_track,
                 library ? library : "", state->autoplay ? "true" : "false",
                 state->shuffle ? "true" : "false", state->repeat_one ? "true" : "false",
                 state->volume, state->muted ? "true" : "false", queue,
                 output ? output : "", outputs,
                 cover ? cover : "",
                 dir ? dir : "", playlists, viewed ? viewed : "", playing ? playing : "",
                 status ? status : "", state->last_cmd_id);
    free(title); free(artist); free(album); free(library); free(cover); free(status); free(output);
    free(dir); free(viewed); free(playing);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return; /* oversized; leave old status intact */
    atomic_write(status_file, tmp, (size_t)n);
}

/* Resume state persists across a backend restart, so it lives in the library
 * directory (a data-home path), not in the volatile IPC dir. The leading dot
 * keeps it from being picked up as a playlist by scan_playlists(). */
static char resume_file[LIBRARY_DIR_MAX + 32];

static void init_resume_path(const char *library_dir) {
    snprintf(resume_file, sizeof(resume_file), "%s/.resume.json", library_dir);
}

/* ---- Multi-playlist library directory ---------------------------------- */

static const char EMPTY_LIBRARY_JSON[] = "{\n  \"version\": 1,\n  \"tracks\": []\n}\n";

/* The auto-collecting playlist: every source added to any other playlist is
 * also added here. Seeded at startup; users cannot create or name it. */
#define STAR_PLAYLIST "*"

/* A playlist name doubles as a filename component, so keep it to a safe set:
 * 1..64 chars of [A-Za-z0-9 _-], no leading/trailing space. This rules out
 * '/', '.' and '..' outright, so it can never escape library_dir. The "*"
 * playlist is reserved and seeded separately, so it never passes here. */
static int valid_playlist_name(const char *name) {
    size_t n = name ? strlen(name) : 0, i;
    if (n == 0 || n > 64) return 0;
    if (name[0] == ' ' || name[n - 1] == ' ') return 0;
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (!(isalnum((unsigned char)c) || c == ' ' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* library_dir fits LIBRARY_DIR_MAX and name is a validated stem (<64 chars),
 * so "<dir>/<name>.json" always fits an LIBRARY_PATH_MAX (or larger) buffer. */
static void playlist_file_path(const AppState *s, const char *name, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s.json", s->library_dir, name);
}

static int playlist_known(const AppState *s, const char *name) {
    int i;
    for (i = 0; i < s->playlist_count; i++)
        if (!strcmp(s->playlists[i], name)) return 1;
    return 0;
}

/* "home" sorts first, the auto-collect "*" playlist sorts last, everything
 * between is case-insensitive alphabetical. */
static int playlist_cmp(const void *a, const void *b) {
    const char *x = (const char *)a, *y = (const char *)b;
    int xh = !strcmp(x, "home"), yh = !strcmp(y, "home");
    int xs = !strcmp(x, STAR_PLAYLIST), ys = !strcmp(y, STAR_PLAYLIST);
    if (xh != yh) return yh - xh;
    if (xs != ys) return xs - ys;
    return strcasecmp(x, y);
}

static int dir_has_playlist_json(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.') continue;
        if (n > 5 && !strcmp(e->d_name + n - 5, ".json")) { found = 1; break; }
    }
    closedir(d);
    return found;
}

static void scan_playlists(AppState *s) {
    DIR *d = opendir(s->library_dir);
    struct dirent *e;
    s->playlist_count = 0;
    if (!d) return;
    while ((e = readdir(d)) && s->playlist_count < (int)(sizeof(s->playlists) / sizeof(s->playlists[0]))) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.') continue;               /* skips .resume.json */
        if (n <= 5 || strcmp(e->d_name + n - 5, ".json")) continue;
        if (n - 5 >= sizeof(s->playlists[0])) continue;
        snprintf(s->playlists[s->playlist_count], sizeof(s->playlists[0]),
                 "%.*s", (int)(n - 5), e->d_name);
        s->playlist_count++;
    }
    closedir(d);
    qsort(s->playlists, (size_t)s->playlist_count, sizeof(s->playlists[0]), playlist_cmp);
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out;
    char buf[8192];
    size_t got;
    int ok = 1;
    if (!in) return 0;
    out = fopen(to, "wb");
    if (!out) { fclose(in); return 0; }
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, got, out) != got) { ok = 0; break; }
    if (fclose(out) != 0) ok = 0;
    fclose(in);
    if (!ok) unlink(to);
    return ok;
}

/* Turn the backend argument into a library directory. Historically the arg was
 * a `library.json` file; keep accepting that (its sibling `library/` becomes
 * the directory and the file migrates to `home.json`). A path without a
 * `.json` suffix is taken as the directory itself. */
static void resolve_library_dir(const char *arg, AppState *s, char *legacy, size_t legacy_size) {
    size_t n = strlen(arg);
    legacy[0] = '\0';
    if (n > 5 && !strcmp(arg + n - 5, ".json")) {
        const char *slash = strrchr(arg, '/');
        long dlen = slash ? (long)(slash - arg) : -1;
        if (dlen >= 0 && dlen < (long)sizeof(s->library_dir) - 16)
            snprintf(s->library_dir, sizeof(s->library_dir), "%.*s/library", (int)dlen, arg);
        else
            snprintf(s->library_dir, sizeof(s->library_dir), "library");
        snprintf(legacy, legacy_size, "%s", arg);
    } else {
        snprintf(s->library_dir, sizeof(s->library_dir), "%.*s", (int)sizeof(s->library_dir) - 1, arg);
    }
    mkdir(s->library_dir, 0700);
    { char abs[PATH_MAX];
      if (realpath(s->library_dir, abs) && strlen(abs) < sizeof(s->library_dir))
          snprintf(s->library_dir, sizeof(s->library_dir), "%s", abs); }
    /* First run: bring an existing single-file library across, else seed an
     * empty home.json so there is always at least one playlist. */
    if (!dir_has_playlist_json(s->library_dir)) {
        char home[PATH_MAX];
        snprintf(home, sizeof(home), "%s/home.json", s->library_dir);
        if (!(legacy[0] && copy_file(legacy, home)))
            atomic_write(home, EMPTY_LIBRARY_JSON, sizeof(EMPTY_LIBRARY_JSON) - 1);
    }
    /* Ensure the auto-collect playlist exists (also on pre-existing installs). */
    { char star[PATH_MAX];
      snprintf(star, sizeof(star), "%s/%s.json", s->library_dir, STAR_PLAYLIST);
      if (access(star, F_OK) != 0)
          atomic_write(star, EMPTY_LIBRARY_JSON, sizeof(EMPTY_LIBRARY_JSON) - 1); }
    scan_playlists(s);
}

/* Remember the current track and position so a restarted backend can pick the
 * song back up where it left off. Throttled by the caller. */
static void write_resume(const AppState *s) {
    char tmp[320];
    char *pl = json_escape(s->playing_playlist);
    int n;
    if (!resume_file[0] || s->selected_track == (size_t)-1 || s->duration_ms == 0) { free(pl); return; }
    n = snprintf(tmp, sizeof(tmp),
                 "{\"playlist\":\"%s\",\"track_index\":%zu,\"position_ms\":%u,\"is_playing\":%s}\n",
                 pl ? pl : "", s->selected_track, s->position_ms, s->is_playing ? "true" : "false");
    free(pl);
    if (n > 0 && (size_t)n < sizeof(tmp)) atomic_write(resume_file, tmp, (size_t)n);
}

/* Parse the resume file written above. Returns 1 and fills the outputs when it
 * holds a usable track_index + position_ms. `playlist` may be NULL; when given
 * it receives the stem of the playlist that was playing (empty if unset). */
static int read_resume(char *playlist, size_t playlist_size,
                       size_t *track_index, Uint32 *position_ms, int *was_playing) {
    char buf[512];
    FILE *f = resume_file[0] ? fopen(resume_file, "r") : NULL;
    size_t got;
    char *p;
    long ti = -1, pos = -1;
    if (playlist && playlist_size) playlist[0] = '\0';
    if (!f) return 0;
    got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    if ((p = strstr(buf, "\"track_index\":"))) ti = strtol(p + 14, NULL, 10);
    if ((p = strstr(buf, "\"position_ms\":"))) pos = strtol(p + 14, NULL, 10);
    if (ti < 0 || pos < 0) return 0;
    *track_index = (size_t)ti;
    *position_ms = (Uint32)pos;
    p = strstr(buf, "\"is_playing\":");
    *was_playing = !(p && strncmp(p + 13, "false", 5) == 0);
    if (playlist && playlist_size && (p = strstr(buf, "\"playlist\":\""))) {
        p += 12;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < playlist_size) playlist[i++] = *p++;
        playlist[i] = '\0';
    }
    return 1;
}

/* Zenity is a separate GTK process.  Do not let it inherit SDL, SSH-agent, or
 * picker pipe descriptors: GLib validates inherited descriptors at startup and
 * reports a warning when one is no longer valid. */
static void close_extra_fds(void) {
    struct rlimit limit;
    int fd;
    rlim_t maximum = 1024;

    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
        maximum = limit.rlim_cur;
    if (maximum > 65536) maximum = 65536;
    for (fd = STDERR_FILENO + 1; (rlim_t)fd < maximum; fd++) close(fd);
}

static void launch_credential_agent(const char *program, AppState *state, const char *message) {
    pid_t pid = fork();
    if (pid == 0) { execlp(program, program, (char *)NULL); _exit(127); }
    snprintf(state->status, sizeof(state->status), "%s", pid < 0 ? "Could not start the system credential agent." : message);
}

static int start_ssh_agent(AppState *state) {
    pid_t pid;
    int attempt;

    if (!state->ssh_agent_dir[0])
        snprintf(state->ssh_agent_dir, sizeof(state->ssh_agent_dir), "%s/ssh-agent", ipc_dir);
    if (mkdir(state->ssh_agent_dir, 0700) != 0 && errno != EEXIST) {
        snprintf(state->status, sizeof(state->status), "Could not create a private directory for the SSH agent.");
        state->ssh_agent_dir[0] = '\0';
        return 0;
    }
    snprintf(state->ssh_agent_socket, sizeof(state->ssh_agent_socket), "%s/socket", state->ssh_agent_dir);
    pid = fork();
    if (pid == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) { dup2(null_fd, STDOUT_FILENO); dup2(null_fd, STDERR_FILENO); close(null_fd); }
        execlp("ssh-agent", "ssh-agent", "-D", "-a", state->ssh_agent_socket, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        rmdir(state->ssh_agent_dir);
        state->ssh_agent_dir[0] = '\0';
        snprintf(state->status, sizeof(state->status), "Could not start an SSH agent.");
        return 0;
    }
    state->ssh_agent_pid = pid;
    for (attempt = 0; attempt < 100; attempt++) {
        if (access(state->ssh_agent_socket, F_OK) == 0) {
            setenv("SSH_AUTH_SOCK", state->ssh_agent_socket, 1);
            return 1;
        }
        if (waitpid(pid, NULL, WNOHANG) == pid) break;
        SDL_Delay(10);
    }
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    unlink(state->ssh_agent_socket);
    rmdir(state->ssh_agent_dir);
    state->ssh_agent_pid = 0;
    state->ssh_agent_dir[0] = '\0';
    state->ssh_agent_socket[0] = '\0';
    snprintf(state->status, sizeof(state->status), "The SSH agent did not become ready.");
    return 0;
}

static void stop_ssh_agent(AppState *state) {
    const char *socket = getenv("SSH_AUTH_SOCK");
    if (state->ssh_agent_pid > 0) {
        kill(state->ssh_agent_pid, SIGTERM);
        waitpid(state->ssh_agent_pid, NULL, 0);
    }
    if (state->ssh_agent_socket[0]) unlink(state->ssh_agent_socket);
    if (state->ssh_agent_dir[0]) rmdir(state->ssh_agent_dir);
    if (socket && strcmp(socket, state->ssh_agent_socket) == 0) unsetenv("SSH_AUTH_SOCK");
}

static void unlock_ssh_agent(AppState *state) {
    const char *socket = getenv("SSH_AUTH_SOCK");

    if (!socket || !socket[0]) if (!start_ssh_agent(state)) return;
    launch_credential_agent("ssh-add", state,
                            "SSH agent is ready; add your key in the system prompt.");
}

static const char *method_name(SourceMethod method) {
    static const char *names[] = { "Local file", "SSH", "HTTPS", "Local network" };
    return names[method];
}

static LibrarySourceKind library_kind(SourceMethod method) {
    return (LibrarySourceKind)method;
}

static int assemble_audio(const unsigned char *data, size_t size, void *userdata) {
    return assembler_push(userdata, data, size) == 1;
}

static int valid_ssh_name(const char *value, int allow_colon) {
    const unsigned char *p = (const unsigned char *)value;
    if (!p || !*p) return 0;
    for (; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '-' && *p != '_' &&
            !(allow_colon && *p == ':')) return 0;
    }
    return 1;
}

static char *remote_cat_command(const char *path) {
    const char *cursor;
    char *command, *out;
    size_t length = 8; /* "cat -- " plus the final NUL */
    for (cursor = path; *cursor; cursor++) length += *cursor == '\'' ? 4 : 1;
    command = malloc(length + 2); /* enclosing single quotes */
    if (!command) return NULL;
    out = command;
    memcpy(out, "cat -- ", 7); out += 7;
    *out++ = '\'';
    for (cursor = path; *cursor; cursor++) {
        if (*cursor == '\'') { memcpy(out, "'\\''", 4); out += 4; }
        else *out++ = *cursor;
    }
    *out++ = '\'';
    *out = '\0';
    return command;
}

/* Single-quote `value` for use inside a remote (or local) shell command, so it
 * is always treated literally and can never inject additional commands. Returns
 * a malloc'd string the caller must free, or NULL on allocation failure. */
static char *ssh_quote(const char *value) {
    size_t len = 3, i;
    char *out;
    if (!value) value = "";
    for (i = 0; value[i]; i++) len += value[i] == '\'' ? 4 : 1;
    out = malloc(len);
    if (!out) return NULL;
    {   char *p = out; *p++ = '\'';
        for (i = 0; value[i]; i++) {
            if (value[i] == '\'') { memcpy(p, "'\\''", 4); p += 4; }
            else *p++ = value[i];
        }
        *p++ = '\''; *p = '\0';
    }
    return out;
}

/* Build a remote shell command that lists music files under `path`, ready to be
 * passed through `ssh -- target <command>`. `path` is single-quoted so it is
 * evaluated literally by the remote shell (no injection). When `path` is empty
 * or NULL, search the remote home directory as a fallback set. Returns a
 * malloc'd string the caller must free, or NULL on allocation failure. */
static char *remote_find_command(const char *path) {
    static const char *names =
        "\\( -iname '*.mp3' -o -iname '*.flac' -o -iname '*.ogg' "
        "-o -iname '*.wav' -o -iname '*.m4a' \\)";
    char *cmd, *quoted;
    size_t len;

    if (path && path[0]) {
        quoted = ssh_quote(path);
        if (!quoted) return NULL;
        len = strlen("find ") + strlen(quoted) + strlen(" -maxdepth 1 -type f ") +
              strlen(names) + strlen(" 2>/dev/null | sort") + 1;
        cmd = malloc(len);
        if (cmd)
            snprintf(cmd, len, "find %s -maxdepth 1 -type f %s 2>/dev/null | sort",
                     quoted, names);
        free(quoted);
        return cmd;
    }

    len = strlen("(find ~/ -type f ") + strlen(names) + strlen(" 2>/dev/null || "
        "find ~/Music -type f 2>/dev/null || "
        "ls -1 ~/Music/*.mp3 ~/Music/*.flac ~/Music/*.ogg 2>/dev/null) | sort") + 1;
    cmd = malloc(len);
    if (cmd)
        snprintf(cmd, len, "(find ~/ -type f %s 2>/dev/null || "
            "find ~/Music -type f 2>/dev/null || "
            "ls -1 ~/Music/*.mp3 ~/Music/*.flac ~/Music/*.ogg 2>/dev/null) | sort",
            names);
    return cmd;
}

/* Run `ssh -- user@host <remote_command>` with stdout and stderr redirected into
 * `dest_path`. Avoids system()/shell entirely: `remote_command` is a single
 * argv element, so nothing local is interpreted. USERNAME/IP are validated so a
 * crafted value cannot become extra arguments. Returns the ssh exit status, or
 * -1 on validation/exec/setup failure. */
static int run_ssh_to_file(const char *username, const char *ip,
                           const char *remote_command, const char *dest_path) {
    char target[256];
    int fd, status;
    pid_t pid;

    if (!remote_command || !valid_ssh_name(username, 0) || !valid_ssh_name(ip, 1))
        return -1;
    if (snprintf(target, sizeof(target), "%s@%s", username, ip) >= (int)sizeof(target))
        return -1;
    fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    pid = fork();
    if (pid == 0) {
        char *const arguments[] = { "ssh", "-F", "/dev/null", "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=5",
            "--", target, (char *)remote_command, NULL };
        if (dup2(fd, STDOUT_FILENO) < 0) _exit(127);
        if (dup2(fd, STDERR_FILENO) < 0) _exit(127);
        close(fd);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    close(fd);
    if (pid < 0) return -1;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int stream_ssh(const LibrarySource *source, MusicRipperWriteFn write,
                      void *write_userdata, void *transport_userdata) {
    char target[256];
    char *command;
    unsigned char buffer[64 * 1024];
    int pipe_fds[2], status;
    pid_t pid;
    ssize_t bytes;
    int result = 0;
    const volatile int *cancel = (const volatile int *)transport_userdata;
    if (!source || !source->path || !valid_ssh_name(source->username, 0) ||
        !valid_ssh_name(source->ip, 1)) return -1;
    if (snprintf(target, sizeof(target), "%s@%s", source->username, source->ip) >= (int)sizeof(target)) return -1;
    command = remote_cat_command(source->path);
    if (!command || pipe(pipe_fds) != 0) { free(command); return -1; }
    pid = fork();
    if (pid == 0) {
        char *const arguments[] = { "ssh", "-F", "/dev/null", "-o", "BatchMode=yes", "-o", "RequestTTY=no",
            "-o", "ClearAllForwardings=yes", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=8",
            "--", target, command, NULL };
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipe_fds[1]);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    close(pipe_fds[1]);
    if (pid < 0) { close(pipe_fds[0]); free(command); return -1; }
    while ((bytes = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        if (cancel && *cancel) { result = -1; break; }
        if (!write(buffer, (size_t)bytes, write_userdata)) { result = -1; break; }
    }
    if (bytes < 0 && errno != EINTR) result = -1;
    close(pipe_fds[0]);
    if (result) kill(pid, SIGTERM);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) result = -1;
    free(command);
    return result;
}

static int stream_https(const LibrarySource *source, MusicRipperWriteFn write,
                        void *write_userdata, void *transport_userdata) {
    unsigned char buffer[64 * 1024];
    int pipe_fds[2], status;
    pid_t pid;
    ssize_t bytes;
    int result = 0;
    const volatile int *cancel = (const volatile int *)transport_userdata;
    if (!source || !source->url || strncasecmp(source->url, "https://", 8) != 0) return -1;
    if (pipe(pipe_fds) != 0) return -1;
    pid = fork();
    if (pid == 0) {
        char *const arguments[] = { "curl", "--fail", "--location", "--max-redirs", "5",
            "--proto", "=https", "--tlsv1.2", "--connect-timeout", "15", "--max-time", "300",
            "--silent", "--show-error", "--output", "-", "--", source->url, NULL };
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipe_fds[1]);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    close(pipe_fds[1]);
    if (pid < 0) { close(pipe_fds[0]); return -1; }
    while ((bytes = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        if (cancel && *cancel) { result = -1; break; }
        if (!write(buffer, (size_t)bytes, write_userdata)) { result = -1; break; }
    }
    if (bytes < 0 && errno != EINTR) result = -1;
    close(pipe_fds[0]);
    if (result) kill(pid, SIGTERM);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) result = -1;
    return result;
}

static void choose_local_file(AppState *state) {
    int pipe_fds[2];
    pid_t pid;
    ssize_t bytes;
    char buffer[512];
    int status;

    if (pipe(pipe_fds) != 0) {
        snprintf(state->status, sizeof(state->status), "Could not create pipe for file picker.");
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        snprintf(state->status, sizeof(state->status), "Could not fork for file picker.");
        return;
    }

    if (pid == 0) {
        /* Child process */
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) _exit(127);
        if (pipe_fds[1] != STDOUT_FILENO) close(pipe_fds[1]);
        close_extra_fds();
        execlp("zenity", "zenity", "--file-selection", "--title=Choose music file", (char *)NULL);
        _exit(127);
    }

    /* Parent process */
    close(pipe_fds[1]);

    bytes = read(pipe_fds[0], buffer, sizeof(buffer) - 1);
    close(pipe_fds[0]);

    waitpid(pid, &status, 0);

    if (bytes <= 0) {
        snprintf(state->status, sizeof(state->status), "No file selected.");
        return;
    }

    buffer[bytes] = '\0';
    buffer[strcspn(buffer, "\r\n")] = '\0';

    snprintf(state->path, sizeof(state->path), "%s", buffer);

    /* Try to extract metadata from the file */
    AudioMetadata metadata = {0};
    char error[256] = {0};

    if (metadata_extract_from_file(buffer, &metadata, error, sizeof(error)) == 1) {
        if (metadata.title) snprintf(state->title, sizeof(state->title), "%s", metadata.title);
        if (metadata.artist) snprintf(state->artist, sizeof(state->artist), "%s", metadata.artist);
        if (metadata.album) snprintf(state->album, sizeof(state->album), "%s", metadata.album);
        snprintf(state->status, sizeof(state->status), "Selected: %.150s (metadata extracted)", buffer);
        metadata_destroy(&metadata);
    } else {
        snprintf(state->status, sizeof(state->status), "Selected local file: %.220s", buffer);
    }
}

static void extract_ssh_metadata(AppState *state) {
    AudioMetadata metadata = {0};
    char error[256] = {0};
    int result;

    if (!state->username[0] || !state->ip[0] || !state->path[0]) {
        snprintf(state->status, sizeof(state->status), "Enter USERNAME, IP, and PATH to extract metadata.");
        return;
    }

    result = metadata_extract_from_ssh(state->username, state->ip, state->path, &metadata, error, sizeof(error));

    if (result == 1) {
        if (metadata.title) snprintf(state->title, sizeof(state->title), "%s", metadata.title);
        if (metadata.artist) snprintf(state->artist, sizeof(state->artist), "%s", metadata.artist);
        if (metadata.album) snprintf(state->album, sizeof(state->album), "%s", metadata.album);
        snprintf(state->status, sizeof(state->status), "Remote metadata extracted from %.150s", state->path);
        metadata_destroy(&metadata);
    } else if (result == 0) {
        snprintf(state->status, sizeof(state->status), "Remote file not found: %.220s", state->path);
    } else {
        snprintf(state->status, sizeof(state->status), "Metadata error: %s", error);
    }
}

static void choose_ssh_file(AppState *state) {
    char temp_file[256];
    char *remote;
    FILE *temp;
    char line[512];
    int file_count = 0;

    if (!state->username[0] || !state->ip[0]) {
        snprintf(state->status, sizeof(state->status), "Enter USERNAME and IP first.");
        return;
    }
    if (!valid_ssh_name(state->username, 0) || !valid_ssh_name(state->ip, 1)) {
        snprintf(state->status, sizeof(state->status), "USERNAME/IP contains invalid characters.");
        return;
    }

    /* Secure temp file for the list output; unlinked at the end of the function. */
    snprintf(temp_file, sizeof(temp_file), "/tmp/leecher_files_XXXXXX");
    int fd = mkstemp(temp_file);
    if (fd < 0) {
        snprintf(state->status, sizeof(state->status), "Could not create temp file for SSH list.");
        return;
    }
    close(fd);

    /* Build the remote find command (the path is single-quoted so the remote
     * shell treats it literally) and run ssh without any local shell. */
    remote = remote_find_command(state->path);
    if (!remote) {
        unlink(temp_file);
        snprintf(state->status, sizeof(state->status), "Out of memory building SSH command.");
        return;
    }
    run_ssh_to_file(state->username, state->ip, remote, temp_file);
    free(remote);

    /* Read results from temp file */
    temp = fopen(temp_file, "r");
    if (!temp) {
        unlink(temp_file);
        snprintf(state->status, sizeof(state->status), "Could not create file list (SSH may have failed).");
        return;
    }

    /* Count non-empty lines */
    file_count = 0;
    while (fgets(line, sizeof(line), temp)) {
        if (line[0] && line[0] != ' ' && line[0] != '\n') {
            file_count++;
        }
    }
    rewind(temp);

    if (file_count == 0) {
        fclose(temp);
        unlink(temp_file);
        if (state->path[0]) {
            snprintf(state->status, sizeof(state->status), "No music files in: %.180s", state->path);
        } else {
            snprintf(state->status, sizeof(state->status),
                "No music files found. Check SSH access, music location, or file permissions.");
        }
        return;
    }

    /* Use proper subprocess handling instead of popen with stdin redirection */
    char selected_path[512] = {0};
    pid_t picker_pid;
    int picker_pipe[2];
    int temp_fd;
    int zenity_status;
    char zenity_title[256];

    /* Create title for zenity */
    if (state->path[0]) {
        snprintf(zenity_title, sizeof(zenity_title), "Music files in %.220s (%d files)", state->path, file_count);
    } else {
        snprintf(zenity_title, sizeof(zenity_title), "Choose remote music file (%d files)", file_count);
    }

    /* Create pipe for zenity output */
    if (pipe(picker_pipe) != 0) {
        fclose(temp);
        unlink(temp_file);
        snprintf(state->status, sizeof(state->status), "Could not create pipe for file picker.");
        return;
    }

    picker_pid = fork();
    if (picker_pid < 0) {
        close(picker_pipe[0]);
        close(picker_pipe[1]);
        fclose(temp);
        unlink(temp_file);
        snprintf(state->status, sizeof(state->status), "Could not fork for file picker.");
        return;
    }

    if (picker_pid == 0) {
        /* Child process */
        close(picker_pipe[0]);

        /* Redirect stdout to pipe */
        if (dup2(picker_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        if (picker_pipe[1] != STDOUT_FILENO) close(picker_pipe[1]);

        /* Open temp file and redirect to stdin */
        temp_fd = open(temp_file, O_RDONLY);
        if (temp_fd < 0) _exit(127);
        if (dup2(temp_fd, STDIN_FILENO) < 0) _exit(127);
        if (temp_fd != STDIN_FILENO) close(temp_fd);

        close_extra_fds();

        /* Execute zenity */
        execlp("zenity", "zenity", "--list", "--title", zenity_title,
               "--column=File", "--width=750", "--height=550", (char *)NULL);
        _exit(127);
    }

    /* Parent process */
    close(picker_pipe[1]);
    fclose(temp);

    /* Read selected file from zenity output */
    ssize_t bytes = read(picker_pipe[0], selected_path, sizeof(selected_path) - 1);
    close(picker_pipe[0]);

    waitpid(picker_pid, &zenity_status, 0);

    if (bytes > 0 && WIFEXITED(zenity_status) && WEXITSTATUS(zenity_status) == 0) {
        selected_path[bytes] = '\0';
        selected_path[strcspn(selected_path, "\r\n")] = '\0';

        if (selected_path[0]) {
            snprintf(state->path, sizeof(state->path), "%s", selected_path);

            /* Try to extract metadata from the remote file */
            AudioMetadata metadata = {0};
            char error[256] = {0};

            if (metadata_extract_from_ssh(state->username, state->ip, selected_path, &metadata, error, sizeof(error)) == 1) {
                if (metadata.title) snprintf(state->title, sizeof(state->title), "%s", metadata.title);
                if (metadata.artist) snprintf(state->artist, sizeof(state->artist), "%s", metadata.artist);
                if (metadata.album) snprintf(state->album, sizeof(state->album), "%s", metadata.album);
                snprintf(state->status, sizeof(state->status), "Selected: %.100s (metadata extracted)", selected_path);
                metadata_destroy(&metadata);
            } else {
                snprintf(state->status, sizeof(state->status), "Selected: %.150s (metadata unavailable on remote)", selected_path);
            }
        } else {
            snprintf(state->status, sizeof(state->status), "No file selected.");
        }
    } else {
        snprintf(state->status, sizeof(state->status), "File picker was cancelled or failed.");
    }

    unlink(temp_file);
}

/* Returns the file name of `path` without its directory and extension into
 * `out`; used as a fallback title when an audio file has no tags. */
static char *basename_no_ext(const char *path, char *out, size_t out_size) {
    const char *base = strrchr(path, '/');
    const char *name = base ? base + 1 : path;
    snprintf(out, out_size, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    return out;
}

/* True when `s` is present and contains a non-whitespace character. */
static int has_real_text(const char *s) {
    if (!s) return 0;
    while (*s) { if (!isspace((unsigned char)*s)) return 1; s++; }
    return 0;
}

/* Builds a LibrarySongQuery for a source, extracting metadata from `file_path`
 * (locally), from `file_path` over SSH with `username`@`ip`, or directly from an
 * HTTP(S) `url`.  Exactly one remote mechanism is used: `url` wins, then the
 * SSH pair, otherwise the local file.  Missing fields fall back so that
 * library_handler_add_source (which requires a non-empty title, artist, album)
 * can accept the entry.  All string buffers must be caller-owned.  Returns a
 * query pointing at those buffers. */
static LibrarySongQuery build_song_query_for(AppState *state, const char *file_path,
                                             const char *username, const char *ip, const char *url,
                                             char *title, size_t title_size,
                                             char *artist, size_t artist_size,
                                             char *album, size_t album_size) {
    AudioMetadata metadata = {0};
    char error[256] = {0};
    char file_title[256];
    int got;
    const char *label = url ? url : file_path;
    if (url) got = metadata_extract_from_url(url, &metadata, error, sizeof(error));
    else if (username && ip) got = metadata_extract_from_ssh(username, ip, file_path, &metadata, error, sizeof(error));
    else got = metadata_extract_from_file(file_path, &metadata, error, sizeof(error));
    basename_no_ext(label, file_title, sizeof(file_title));
    snprintf(title, title_size, "%s", has_real_text(metadata.title) ? metadata.title : file_title);
    snprintf(artist, artist_size, "%s", has_real_text(metadata.artist) ? metadata.artist : "Unknown artist");
    snprintf(album, album_size, "%s", has_real_text(metadata.album) ? metadata.album : "Unknown album");
    if (got != 1) snprintf(state->status, sizeof(state->status), "No tags for %.160s, imported by file name.", label);
    metadata_destroy(&metadata);
    return (LibrarySongQuery){ .title = title, .artist = artist, .album = album };
}

/* Wrapper matching the original signature: `remote` extracts via state->username
 * / state->ip, otherwise the file is read locally. */
static LibrarySongQuery build_song_query(AppState *state, const char *file_path,
                                         int remote, char *title, size_t title_size,
                                         char *artist, size_t artist_size,
                                         char *album, size_t album_size) {
    return build_song_query_for(state, file_path, remote ? state->username : NULL,
                                remote ? state->ip : NULL, NULL,
                                title, title_size, artist, artist_size, album, album_size);
}

static int source_field_eq(const char *a, const char *b) {
    return strcmp(a ? a : "", b ? b : "") == 0;
}

/* Two sources refer to the same thing: same transport and same locator. */
static int source_equal(const LibrarySource *a, const LibrarySource *b) {
    return a->kind == b->kind &&
           source_field_eq(a->path, b->path) && source_field_eq(a->url, b->url) &&
           source_field_eq(a->username, b->username) && source_field_eq(a->ip, b->ip);
}

/* Mirror a freshly-added source into the auto-collect "*" playlist (file only,
 * no in-memory handles). Skipped when "*" is the playlist it was just added to,
 * and when that exact source is already collected there, so adding the same
 * file to several playlists does not pile up duplicates. If "*" happens to be
 * the one playing, its play_lib handle is refreshed on the next `playlist *`. */
static void mirror_add_to_star(const AppState *s, const LibrarySongQuery *song,
                               const LibrarySource *source) {
    char star[LIBRARY_PATH_MAX], err[256];
    LibraryHandler *h;
    size_t count, i, j;
    int already = 0;
    if (!strcmp(s->viewed_playlist, STAR_PLAYLIST)) return;
    playlist_file_path(s, STAR_PLAYLIST, star, sizeof(star));
    h = library_handler_open(star, err, sizeof(err));
    if (!h) return;
    count = library_handler_track_count(h);
    for (i = 0; i < count && !already; i++) {
        LibraryTrack t = {0};
        if (library_handler_track_at(h, i, &t, err, sizeof(err)) == 1)
            for (j = 0; j < t.source_count; j++)
                if (source_equal(&t.sources[j], source)) { already = 1; break; }
        library_handler_track_destroy(&t);
    }
    library_handler_close(h);
    if (!already)
        library_handler_add_source(star, song, source, err, sizeof(err));
}

/* Adds a source for a single audio file to the library and reloads the in-memory
 * handler so the new entry is visible immediately. Returns 1 when a source was
 * saved (the track may be new or a merge into an existing one). */
static int import_single_source(const char *library_path, AppState *state,
                                const LibrarySongQuery *song, const LibrarySource *source,
                                LibraryHandler **library, MusicRipper *ripper) {
    char error[256] = {0};
    if (library_handler_add_source(library_path, song, source, error, sizeof(error)) != 1) {
        snprintf(state->status, sizeof(state->status), "Import failed for %.160s: %s", song->title, error);
        return 0;
    }
    mirror_add_to_star(state, song, source);
    LibraryHandler *reloaded = library_handler_open(library_path, error, sizeof(error));
    if (reloaded) {
        cancel_fetch(state);
        library_handler_close(*library);
        *library = reloaded;
        if (ripper) ripper->library = reloaded;
        resync_play_lib(state);
    }
    return 1;
}

/* True when `name` ends in a recognised audio extension. */
static int is_audio_name(const char *name) {
    static const char *exts[] = { ".mp3", ".flac", ".ogg", ".wav", ".m4a", ".aac", ".opus", ".wma" };
    size_t i;
    const char *dot;
    if (!name || !*name) return 0;
    dot = strrchr(name, '.');
    if (!dot) return 0;
    for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (strcasecmp(dot, exts[i]) == 0) return 1;
    return 0;
}

/* Pull every playable song out of the directory in `dir` and import it as a
 * LOCAL source, de-duplicating by exact title/artist/album. */
static void pull_local_songs(const char *library_path, AppState *state,
                             const char *dir, LibraryHandler **library, MusicRipper *ripper) {
    DIR *d;
    struct dirent *ent;
    int total = 0, added = 0;

    if (!dir || !dir[0]) { snprintf(state->status, sizeof(state->status), "Enter a PATH directory to pull songs from."); return; }
    d = opendir(dir);
    if (!d) { snprintf(state->status, sizeof(state->status), "Cannot open directory: %.180s", dir); return; }
    while ((ent = readdir(d)) != NULL) {
        char full[1024];
        struct stat st;
        LibrarySongQuery song;
        LibrarySource source;
        char title[256], artist[256], album[256];
        if (ent->d_name[0] == '.' || !is_audio_name(ent->d_name)) continue;
        if (snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name) >= (int)sizeof(full)) continue;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        total++;
        song = build_song_query(state, full, 0, title, sizeof(title), artist, sizeof(artist), album, sizeof(album));
        source = (LibrarySource){ .kind = LIBRARY_SOURCE_LOCAL, .path = full };
        if (import_single_source(library_path, state, &song, &source, library, ripper)) added++;
    }
    closedir(d);
    snprintf(state->status, sizeof(state->status), "Local pull: %d of %d songs imported from %.160s.", added, total, dir);
}

/* Pull every playable song under the remote `path` (via find over SSH) and import
 * each as an SSH source, de-duplicating by exact title/artist/album. */
static void pull_ssh_songs(const char *library_path, AppState *state,
                           LibraryHandler **library, MusicRipper *ripper) {
    char temp_file[256];
    char *remote;
    char line[1024];
    FILE *list;
    int total = 0, added = 0;

    if (!state->username[0] || !state->ip[0]) { snprintf(state->status, sizeof(state->status), "Enter USERNAME and IP first."); return; }
    if (!valid_ssh_name(state->username, 0) || !valid_ssh_name(state->ip, 1)) { snprintf(state->status, sizeof(state->status), "USERNAME/IP contains invalid characters."); return; }
    if (!state->path[0]) { snprintf(state->status, sizeof(state->status), "Enter a PATH directory to pull songs from."); return; }
    snprintf(temp_file, sizeof(temp_file), "/tmp/leecher_pull_XXXXXX");
    int fd = mkstemp(temp_file);
    if (fd < 0) { snprintf(state->status, sizeof(state->status), "Could not create temp file for SSH pull."); return; }
    close(fd);
    remote = remote_find_command(state->path);
    if (!remote) { unlink(temp_file); snprintf(state->status, sizeof(state->status), "Out of memory building SSH command."); return; }
    run_ssh_to_file(state->username, state->ip, remote, temp_file);
    free(remote);
    list = fopen(temp_file, "r");
    if (!list) { unlink(temp_file); snprintf(state->status, sizeof(state->status), "Could not run SSH find."); return; }
    while (fgets(line, sizeof(line), list)) {
        char *path;
        LibrarySongQuery song;
        LibrarySource source;
        char title[256], artist[256], album[256];
        line[strcspn(line, "\r\n")] = '\0';
        path = line;
        while (*path == ' ' || *path == '\t') path++;
        if (!*path) continue;
        total++;
        song = build_song_query(state, path, 1, title, sizeof(title), artist, sizeof(artist), album, sizeof(album));
        source = (LibrarySource){ .kind = LIBRARY_SOURCE_SSH, .path = path, .username = state->username, .ip = state->ip };
        if (import_single_source(library_path, state, &song, &source, library, ripper)) added++;
    }
    fclose(list);
    unlink(temp_file);
    snprintf(state->status, sizeof(state->status), "SSH pull: %d of %d songs imported from %.160s.", added, total, state->path);
}

/* Imports a single file dropped onto the window into the library as a LOCAL
 * source. */
static void handle_dropped_file(const char *library_path, AppState *state, const char *file,
                                LibraryHandler **library, MusicRipper *ripper) {
    struct stat st;
    LibrarySource source;
    LibrarySongQuery song;
    char title[256], artist[256], album[256];
    if (!file || !file[0]) return;
    if (stat(file, &st) != 0 || !S_ISREG(st.st_mode)) { snprintf(state->status, sizeof(state->status), "Dropped item is not a regular file: %.160s", file); return; }
    if (!is_audio_name(file)) { snprintf(state->status, sizeof(state->status), "Dropped item is not an audio file: %.160s", file); return; }
    song = build_song_query(state, file, 0, title, sizeof(title), artist, sizeof(artist), album, sizeof(album));
    source = (LibrarySource){ .kind = LIBRARY_SOURCE_LOCAL, .path = (char *)file };
    if (import_single_source(library_path, state, &song, &source, library, ripper))
        snprintf(state->status, sizeof(state->status), "Imported dropped file: %.160s", file);
}

static void write_status(const AppState *state);

typedef struct FetchJob {
    AppState *state;
    const LibraryHandler *library;
    size_t index;
    Assembler *assembler;
    MusicRipper ripper;
} FetchJob;

static void fetch_lock(AppState *s) { pthread_mutex_lock(&s->fetch_mutex); }
static void fetch_unlock(AppState *s) { pthread_mutex_unlock(&s->fetch_mutex); }

static void free_fetch_cache(AppState *s) {
    if (s->fetch_decoder) { decoder_close(s->fetch_decoder); s->fetch_decoder = NULL; }
    s->fetch_title[0] = '\0';
    s->fetch_artist[0] = '\0';
    s->fetch_album[0] = '\0';
    s->fetch_cover[0] = '\0';
    s->fetch_ready = 0;
}

/* Single-quote a value for the local shell (for the ffmpeg path argument). */
static char *shell_quote_words(const char *value) {
    size_t len = 3, i;
    char *out;
    for (i = 0; value[i]; i++) len += value[i] == '\'' ? 4 : 1;
    out = malloc(len);
    if (!out) return NULL;
    {   char *p = out; *p++ = '\'';
        for (i = 0; value[i]; i++) {
            if (value[i] == '\'') { memcpy(p, "'\\''", 4); p += 4; }
            else *p++ = value[i];
        }
        *p++ = '\''; *p = '\0';
    }
    return out;
}

/* Accepts only safe SSH username/IP characters (prevents shell injection). */
static int ssh_name_valid(const char *value, int allow_colon) {
    const unsigned char *p = (const unsigned char *)value;
    if (!p || !*p) return 0;
    for (; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '-' && *p != '_' &&
            !(allow_colon && *p == ':')) return 0;
    }
    return 1;
}

/* Runs `cmd` via /bin/sh -c in its own process group, bounded by timeout_ms.
 * Returns the child's exit status (or -1 on setup failure / timeout / cancel).
 * The process group kill on timeout ensures any grandchildren (ssh, ffmpeg,
 * curl) are also terminated, so a stuck network source can never hang the
 * player.  When `cancel` is set (non-NULL and non-zero) the child group is
 * killed and -1 returned immediately, so aborting a fetch in the worker thread
 * never blocks the caller. */
static int run_command_timeout(const char *cmd, int timeout_ms, const volatile int *cancel) {
    pid_t pid = fork();
    int status;
    int waited = 0;
    if (pid < 0) return -1;
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    signal(SIGPIPE, SIG_IGN);
    while (waited < timeout_ms) {
        if (cancel && *cancel) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        usleep(50000);
        waited += 50;
    }
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

/* Runs ffmpeg to pull the first attached-picture (cover) from `input` (an
 * already shell-quoted ffmpeg input argument) into the per-user cover file,
 * writing the resulting path into `out` (cleared when no attached picture is
 * found). */
static void run_ffmpeg_cover(const char *input, char *out, size_t out_size, int timeout_ms,
                             const volatile int *cancel) {
    char command[3600];
    char *qcover;
    struct stat st;
    out[0] = '\0';
    qcover = shell_quote_words(cover_file);
    if (!qcover) return;
    if (snprintf(command, sizeof(command),
                 "ffmpeg -v error -y -i %s -an -map 0:v:0 -c:v copy -frames:v 1 %s 2>/dev/null",
                 input, qcover) >= (int)sizeof(command)) {
        free(qcover);
        return;
    }
    free(qcover);
    if (run_command_timeout(command, timeout_ms, cancel) == 0 && stat(cover_file, &st) == 0 && st.st_size > 0)
        snprintf(out, out_size, "%s", cover_file);
}

/* Extracts embedded album art from any supported source kind into the per-user
 * cover file using ffmpeg, and writes its path into `out` (cleared when there
 * is no embedded art). Local files and HTTPS URLs are passed straight to
 * ffmpeg; SSH/network files are streamed over ssh to ffmpeg's stdin so only
 * the tiny cover frame is transferred, not the whole track.  `cancel` (may be
 * NULL) is polled so a long ffmpeg/ssh run aborts promptly when a fetch is
 * cancelled, instead of stalling the worker (and the main loop that joins it)
 * for the full timeout. */
static void extract_source_cover(const LibrarySource *source, char *out, size_t out_size,
                                 const volatile int *cancel) {
    out[0] = '\0';
    if (!source) return;
    switch (source->kind) {
    case LIBRARY_SOURCE_LOCAL: {
        if (!source->path || !source->path[0]) return;
        char *q = shell_quote_words(source->path);
        if (q) { run_ffmpeg_cover(q, out, out_size, 10000, cancel); free(q); }
        break;
    }
    case LIBRARY_SOURCE_HTTPS: {
        if (!source->url || strncasecmp(source->url, "https://", 8) != 0) return;
        char *q = shell_quote_words(source->url);
        if (q) { run_ffmpeg_cover(q, out, out_size, 15000, cancel); free(q); }
        break;
    }
    case LIBRARY_SOURCE_SSH:
    case LIBRARY_SOURCE_NETWORK: {
        if (!source->path || !source->path[0]) return;
        if (!source->username || !source->username[0]) return;
        if (!source->ip || !source->ip[0]) return;
        if (!ssh_name_valid(source->username, 0) || !ssh_name_valid(source->ip, 1)) return;
        char *qpath = shell_quote_words(source->path);
        if (!qpath) return;
        char *remote_cmd = malloc(strlen("cat ") + strlen(qpath) + 1);
        if (!remote_cmd) { free(qpath); return; }
        sprintf(remote_cmd, "cat %s", qpath);
        free(qpath);
        char *qcmd = shell_quote_words(remote_cmd);
        free(remote_cmd);
        if (!qcmd) return;
        char command[3600];
        char *qcover = shell_quote_words(cover_file);
        if (!qcover) { free(qcmd); return; }
        int wrote = snprintf(command, sizeof(command),
                     "ssh -F /dev/null -o BatchMode=yes -o RequestTTY=no -o ClearAllForwardings=yes -o LogLevel=ERROR -- %s@%s %s 2>/dev/null | ffmpeg -v error -y -i - -an -map 0:v:0 -c:v copy -frames:v 1 %s 2>/dev/null",
                     source->username, source->ip, qcmd, qcover);
        free(qcmd);
        free(qcover);
        if (wrote >= (int)sizeof(command))
            return;
        struct stat st;
        if (run_command_timeout(command, 8000, cancel) == 0 && stat(cover_file, &st) == 0 && st.st_size > 0)
            snprintf(out, out_size, "%s", cover_file);
        break;
    }
    default:
        break;
    }
}

static void *fetch_worker(void *arg) {
    FetchJob *job = (FetchJob *)arg;
    AppState *s = job->state;
    LibraryTrack track = {0};
    LibrarySongQuery song = {0};
    DecoderSource *decoder = NULL;
    char error[256] = {0};
    char save_title[128] = {0}, save_artist[128] = {0}, save_album[128] = {0};
    char save_cover[512] = {0};
    int ready = 0;
    if (library_handler_track_at(job->library, job->index, &track, error, sizeof(error)) == 1) {
        song = (LibrarySongQuery){ .id = track.id, .title = track.title, .artist = track.artist, .album = track.album };
        if (track.title) snprintf(save_title, sizeof(save_title), "%s", track.title);
        if (track.artist) snprintf(save_artist, sizeof(save_artist), "%s", track.artist);
        if (track.album) snprintf(save_album, sizeof(save_album), "%s", track.album);
        for (size_t ci = 0; ci < track.source_count && save_cover[0] == '\0'; ci++) {
            extract_source_cover(&track.sources[ci], save_cover, sizeof(save_cover), &s->fetch_cancel);
        }
        if (save_cover[0]) {
            char unique[420];
            const char *const dir = ipc_dir;
            /* Drop prior covers: the directory stays at one committed cover,
             * and the file name gains a fresh nonce so a later track that
             * shares this index cannot alias (and show stale art for) it. */
            remove_cover_files();
            unsigned long nonce = ++cover_nonce;
            snprintf(unique, sizeof(unique), "%s/cover-%zu-%lu.jpg", dir, job->index, nonce);
            rename(save_cover, unique);
            snprintf(save_cover, sizeof(save_cover), "%s", unique);
        }
        if (!s->fetch_cancel &&
            music_ripper_play_next(&job->ripper, &song, NULL, assemble_audio, job->assembler, error, sizeof(error)) == 1 &&
            !s->fetch_cancel &&
            decoder_open(job->assembler, &decoder, error, sizeof(error)) == 1 &&
            !s->fetch_cancel) {
            ready = 1;
        }
        library_handler_track_destroy(&track);
    }
    if (ready) {
        fetch_lock(s);
        free_fetch_cache(s);
        s->fetch_decoder = decoder;
        snprintf(s->fetch_title, sizeof(s->fetch_title), "%s", save_title);
        snprintf(s->fetch_artist, sizeof(s->fetch_artist), "%s", save_artist);
        snprintf(s->fetch_album, sizeof(s->fetch_album), "%s", save_album);
        snprintf(s->fetch_cover, sizeof(s->fetch_cover), "%s", save_cover);
        s->fetch_error[0] = '\0';
        s->fetch_ready = 1;
        fetch_unlock(s);
    } else {
        if (decoder) decoder_close(decoder);
        /* Record why, so an autoplay skip can say more than "track vanished".
         * A cancel is not a failure: the next fetch is already starting. */
        fetch_lock(s);
        if (!s->fetch_cancel)
            snprintf(s->fetch_error, sizeof(s->fetch_error), "%s",
                     error[0] ? error : "source unavailable");
        fetch_unlock(s);
    }
    fetch_lock(s);
    s->fetch_active = 0;
    fetch_unlock(s);
    assembler_destroy(job->assembler);
    free(job);
    return NULL;
}

/* Signal the background fetch thread to stop and join it. The join blocks the
 * main loop, so stream_audio() cannot refill SDL's queue while it runs and a
 * local file can take a few hundred ms to unwind -- long enough to be heard as
 * a gap when the library is mutated mid-playback (#11). Fill the queue several
 * seconds deep first so the wind-down is inaudible. The caller must hold
 * fetch_mutex and have confirmed fetch_thread_valid; the lock is dropped for
 * the fill + join and retaken before return. */
static void join_fetch_thread(AppState *s) {
    s->fetch_cancel = 1;
    pthread_mutex_unlock(&s->fetch_mutex);
    s->stream_target_ms = 6000;
    stream_audio(s);
    s->stream_target_ms = 0;
    pthread_join(s->fetch_thread, NULL);
    pthread_mutex_lock(&s->fetch_mutex);
    s->fetch_thread_valid = 0;
}

/* Restart the worker (joining an in-flight one first) to fetch `index` from the
 * playlist playback is currently running over (s->play_lib). */
static void start_fetch(AppState *s, size_t index) {
    const LibraryHandler *library = s->play_lib;
    fetch_lock(s);
    if (s->fetch_thread_valid)
        join_fetch_thread(s);
    free_fetch_cache(s);
    s->fetch_cancel = 0;
    s->fetch_active = 1;
    s->fetch_ready = 0;
    s->fetch_error[0] = '\0';
    s->fetch_index = index;
    pthread_mutex_unlock(&s->fetch_mutex);

    FetchJob *job = (FetchJob *)calloc(1, sizeof(*job));
    if (!job) { fetch_lock(s); s->fetch_active = 0; fetch_unlock(s); return; }
    job->state = s;
    job->library = library;
    job->index = index;
    /* Bound the compressed stream buffered during a fetch: a stuck source that
     * streams data forever must eventually fail instead of growing RAM without
     * limit. The limit is generous so legitimate lossless files still fit. */
    {
        AssemblerConfig acfg = {0};
        acfg.max_queued_bytes = (size_t)3 * 1024 * 1024 * 1024;
        job->assembler = assembler_create(&acfg);
    }
    if (!job->assembler) { free(job); fetch_lock(s); s->fetch_active = 0; fetch_unlock(s); return; }
    job->ripper.library = library;
    job->ripper.transports = s->transports;
    job->ripper.transports.userdata = (void *)&s->fetch_cancel;
    if (pthread_create(&s->fetch_thread, NULL, fetch_worker, job) == 0) {
        s->fetch_thread_valid = 1;
    } else {
        assembler_destroy(job->assembler);
        free(job);
        fetch_lock(s); s->fetch_active = 0; fetch_unlock(s);
    }
}

static void cancel_fetch(AppState *s) {
    fetch_lock(s);
    if (s->fetch_thread_valid) {
        join_fetch_thread(s);
        free_fetch_cache(s);
    }
    pthread_mutex_unlock(&s->fetch_mutex);
    s->pending_valid = 0;
    s->immediate_pending = 0;
}

/* How much decoded PCM we keep queued ahead in SDL, in frames.  Keeping only a
 * fraction of the track decoded at once bounds RAM regardless of length. */
#define STREAM_QUEUE_FRAMES  (44100u / 4u)         /* ~0.25s per chunk */
#define STREAM_QUEUE_TARGET  (44100u * 3u / 4u)    /* refill up to ~0.75s */

/* Open the chosen output (audio_device_name), or the system default when it is
 * empty. If a named device fails to open it has probably been unplugged or
 * renamed: report it, forget the choice, and fall back to the default so
 * playback is never left with no device at all. */
static SDL_AudioDeviceID open_audio_device(AppState *s, SDL_AudioSpec *want, SDL_AudioSpec *have) {
    const char *name = s->audio_device_name[0] ? s->audio_device_name : NULL;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(name, 0, want, have, 0);
    if (!dev && name) {
        fprintf(stderr, "leecher: output '%s' unavailable (%s), using default\n", name, SDL_GetError());
        snprintf(s->status, sizeof(s->status), "Output \"%.80s\" unavailable, using default.", s->audio_device_name);
        s->audio_device_name[0] = '\0';
        dev = SDL_OpenAudioDevice(NULL, 0, want, have, 0);
    }
    return dev;
}

/* Move the running stream to the currently-selected output, resuming from the
 * frame that was actually playing. Used when the user picks a device. */
static void reopen_audio_device(AppState *s) {
    SDL_AudioSpec want = {0}, have = {0};
    Uint64 frame = 0;
    if (!s->decoder || s->audio_channels <= 0 || s->audio_rate <= 0) return;
    if (s->audio_device) {
        Uint32 queued = SDL_GetQueuedAudioSize(s->audio_device);
        Uint64 fiq = queued / ((Uint32)s->audio_channels * 2u);
        frame = (s->audio_queued_frame > fiq) ? s->audio_queued_frame - fiq : 0;
        SDL_CloseAudioDevice(s->audio_device);
        s->audio_device = 0;
    }
    want.freq = s->audio_rate; want.format = AUDIO_S16SYS;
    want.channels = (Uint8)s->audio_channels; want.samples = 4096;
    s->audio_device = open_audio_device(s, &want, &have);
    if (!s->audio_device) { s->is_playing = 0; return; }
    if (decoder_seek(s->decoder, (long long)frame) < 0) { decoder_seek(s->decoder, 0); frame = 0; }
    s->audio_queued_frame = frame;
    s->stream_eof = 0;
    s->track_ended = 0;
    stream_audio(s);
    SDL_PauseAudioDevice(s->audio_device, s->is_playing ? 0 : 1);
}

/* Swap the fetched stream into the SDL audio device and start streaming it.
 * Only a small rolling window is decoded at a time (see stream_audio), so the
 * whole track is never materialised in RAM as PCM. */
static void commit_fetch(AppState *s, size_t idx) {
    SDL_AudioSpec desired = {0}, obtained = {0};
    DecoderSource *decoder = NULL;
    int channels = 0, rate = 0;
    Uint64 total_frames = 0;

    fetch_lock(s);
    if (!s->fetch_ready || s->fetch_index != idx || !s->fetch_decoder) {
        fetch_unlock(s);
        snprintf(s->status, sizeof(s->status), "Track is not ready yet.");
        return;
    }
    decoder = s->fetch_decoder;
    total_frames = (Uint64)decoder_total_frames(decoder);
    channels = decoder_channels(decoder);
    rate = decoder_rate(decoder);
    snprintf(s->title, sizeof(s->title), "%s", s->fetch_title);
    snprintf(s->artist, sizeof(s->artist), "%s", s->fetch_artist);
    snprintf(s->album, sizeof(s->album), "%s", s->fetch_album);
    snprintf(s->cover_file, sizeof(s->cover_file), "%s", s->fetch_cover);
    s->fetch_decoder = NULL;
    free_fetch_cache(s);
    fetch_unlock(s);

    if (s->decoder) decoder_close(s->decoder);
    s->decoder = decoder;
    s->audio_queued_frame = 0;

    if (s->audio_device) SDL_CloseAudioDevice(s->audio_device);
    s->audio_device = 0;
    if (channels > 0 && rate > 0) {
        desired.freq = rate; desired.format = AUDIO_S16SYS; desired.channels = (Uint8)channels; desired.samples = 4096;
        s->audio_device = open_audio_device(s, &desired, &obtained);
    }
    if (!s->audio_device || total_frames == 0) {
        snprintf(s->status, sizeof(s->status), "SDL audio: %s", SDL_GetError());
        if (s->audio_device) { SDL_CloseAudioDevice(s->audio_device); s->audio_device = 0; }
        decoder_close(s->decoder); s->decoder = NULL;
        s->is_playing = 0;
        write_status(s);
        return;
    }
    s->audio_channels = channels;
    s->audio_rate = rate;
    s->audio_total_frames = total_frames;
    s->track_ended = 0;
    s->stream_open = 1;
    s->stream_eof = 0;
    s->is_playing = 1;
    s->position_ms = 0;
    s->duration_ms = (rate > 0) ? (Uint32)(total_frames * 1000u / (Uint64)rate) : 0;
    s->selected_track = idx;
    s->pending_valid = 0;
    s->immediate_pending = 0;
    /* Fresh track: clear the stall/no-output detectors so the first buffer
     * fill is not mistaken for a stalled device. */
    s->position_stalls = 0;
    s->no_output_ticks = 0;
    s->last_position_ms = 0;
    SDL_PauseAudioDevice(s->audio_device, 0);
    snprintf(s->status, sizeof(s->status), "Playing %s.", s->title[0] ? s->title : "track");
    stream_audio(s);
    /* Resume from the saved position/state, once, on the track we were told to
     * pick up (see the startup path). */
    if ((s->resume_position_ms > 0 || s->resume_paused) && s->audio_rate > 0) {
        Uint32 rp = s->resume_position_ms;
        s->resume_position_ms = 0;
        if (rp > 0 && rp + 1000u < s->duration_ms) {
            Uint64 frame = (Uint64)rp * (Uint64)s->audio_rate / 1000u;
            if (frame < s->audio_total_frames && decoder_seek(s->decoder, (long long)frame) >= 0) {
                SDL_ClearQueuedAudio(s->audio_device);
                s->audio_queued_frame = frame;
                s->position_ms = rp;
                s->stream_eof = 0;
                stream_audio(s);
            }
        }
        if (s->resume_paused) {
            s->resume_paused = 0;
            s->is_playing = 0;
            SDL_PauseAudioDevice(s->audio_device, 1);
        }
        snprintf(s->status, sizeof(s->status), "%s%s",
                 s->is_playing ? "Resumed: " : "Resumed (paused): ", s->title[0] ? s->title : "track");
    }
    write_status(s);
    write_resume(s);
}

/* Queue a track to play as soon as it is fetched (interrupts current). Always
 * addresses the playing playlist (s->play_lib); play_library_index() repoints
 * that at the viewed playlist first when the user starts a track there. */
static void request_play(AppState *s, size_t index) {
    const LibraryHandler *library = s->play_lib;
    size_t count = library ? library_handler_track_count(library) : 0;
    if (count == 0) { snprintf(s->status, sizeof(s->status), "Playlist is empty."); return; }
    s->pending_valid = 0;
    s->immediate_pending = 1;
    s->immediate_index = index;
    /* Immediate feedback: a remote (HTTPS/SSH) source can take seconds to
     * start, so without this a skip / track click looks like it did nothing. */
    {
        LibraryTrack t = {0};
        char e[128];
        if (library_handler_track_at(library, index, &t, e, sizeof(e)) == 1 && t.title && t.title[0])
            snprintf(s->status, sizeof(s->status), "Loading %.180s...", t.title);
        else
            snprintf(s->status, sizeof(s->status), "Loading track %zu...", index + 1);
        library_handler_track_destroy(&t);
        write_status(s);
    }
    fetch_lock(s);
    int ready = s->fetch_ready && s->fetch_index == index;
    int active = s->fetch_active && s->fetch_index == index;
    fetch_unlock(s);
    if (!ready && !active) start_fetch(s, index);
}

/* Point the playback handle at the viewed playlist and adopt it as the one
 * playing. A no-op when they already match. */
static void adopt_viewed_as_playing(AppState *s) {
    char err[128];
    LibraryHandler *h;
    if (!strcmp(s->viewed_playlist, s->playing_playlist)) return;
    h = library_handler_open(s->library_path, err, sizeof(err));
    if (!h) return;
    library_handler_close(s->play_lib);
    s->play_lib = h;
    snprintf(s->playing_playlist, sizeof(s->playing_playlist), "%s", s->viewed_playlist);
    s->play_queue_len = 0;   /* the queue indexed into the old playlist */
}

/* The user picked a track in the library list: switch playback to the playlist
 * they are viewing (if different) and start that track. */
static void play_library_index(AppState *state, size_t index) {
    state->autoplay_advancing = 0;
    adopt_viewed_as_playing(state);
    request_play(state, index);
}

/* The track autoplay moves to from `from`. repeat-one stays put (only when
 * `allow_repeat`, so a skip past a broken source still advances); shuffle picks
 * a random other track; otherwise the next in library order. count > 0. */
static size_t next_autoplay_index(const AppState *s, size_t count, size_t from, int allow_repeat) {
    if (count <= 1) return from;
    if (allow_repeat && s->repeat_one) return from;
    if (s->shuffle) {
        size_t r;
        do { r = (size_t)rand() % count; } while (r == from);
        return r;
    }
    return (from + 1) % count;
}

#define PLAY_QUEUE_CAP ((int)(sizeof(((AppState *)0)->play_queue) / sizeof(size_t)))

static void play_queue_remove_at(AppState *s, int pos) {
    if (pos < 0 || pos >= s->play_queue_len) return;
    memmove(&s->play_queue[pos], &s->play_queue[pos + 1],
            (size_t)(s->play_queue_len - pos - 1) * sizeof(size_t));
    s->play_queue_len--;
}

static void play_queue_push(AppState *s, size_t idx) {
    if (s->play_queue_len >= PLAY_QUEUE_CAP) return;
    s->play_queue[s->play_queue_len++] = idx;
}

/* The next track to play: the queue head if it still points at a real track,
 * otherwise the autoplay rule. repeat-one is handled earlier (seek in place),
 * so an explicit queue always wins over shuffle/linear order. */
static size_t take_next_index(AppState *s, size_t count, size_t from, int allow_repeat) {
    while (s->play_queue_len > 0) {
        size_t q = s->play_queue[0];
        play_queue_remove_at(s, 0);
        if (q < count) return q;   /* drop stale entries left by a shrunk library */
    }
    return next_autoplay_index(s, count, from, allow_repeat);
}

static void play_library_relative(AppState *state, long delta) {
    size_t count = state->play_lib ? library_handler_track_count(state->play_lib) : 0;
    if (count == 0) { snprintf(state->status, sizeof(state->status), "Playlist is empty."); return; }
    /* Forward honours the queue first, then shuffle; back is always linear. */
    if (delta > 0 && (state->play_queue_len > 0 || (state->shuffle && count > 1))) {
        state->autoplay_advancing = 0;
        request_play(state, take_next_index(state, count, state->selected_track, 0));
        return;
    }
    long cur = (long)state->selected_track;
    long nxt = (cur + delta) % (long)count;
    if (nxt < 0) nxt += (long)count;
    state->autoplay_advancing = 0;
    request_play(state, (size_t)nxt);
}

static void toggle_play_pause(AppState *state) {
    if (!state->audio_device) { snprintf(state->status, sizeof(state->status), "No audio loaded to pause."); return; }
    state->is_playing = !state->is_playing;
    SDL_PauseAudioDevice(state->audio_device, state->is_playing ? 0 : 1);
    snprintf(state->status, sizeof(state->status), state->is_playing ? "Playing." : "Paused.");
    write_status(state);
    write_resume(state);
}

static Uint32 playback_ms(const AppState *s) {
    if (!s->audio_device || !s->audio_rate || s->audio_channels <= 0) return s->position_ms;
    Uint32 queued = SDL_GetQueuedAudioSize(s->audio_device);
    Uint64 frames_in_queue = queued / ((Uint32)s->audio_channels * 2u);
    Uint64 frame = (s->audio_queued_frame > frames_in_queue) ? s->audio_queued_frame - frames_in_queue : 0;
    return (Uint32)(frame * 1000u / (Uint64)s->audio_rate);
}

/* Decode and queue PCM into SDL on demand.  Called every main-loop iteration;
 * it refills SDL's queue up to STREAM_QUEUE_TARGET frames and stops, so only a
 * small rolling window of the track is ever decoded and held in memory instead
 * of the full PCM (and full duplicated SDL copy) at once. */
static void stream_audio(AppState *s) {
    int channels;
    Uint32 target_bytes, queued;
    long long chunk_limit = STREAM_QUEUE_FRAMES;
    short buf[STREAM_QUEUE_FRAMES * 8]; /* up to 8 interleaved channels */
    if (!s->stream_open || !s->decoder || s->stream_eof) return;
    channels = s->audio_channels;
    if (!s->audio_device || channels <= 0 || channels > 8) return;
    target_bytes = (s->stream_target_ms && s->audio_rate > 0
        ? (Uint32)((Uint64)s->stream_target_ms * (Uint64)s->audio_rate / 1000u)
        : STREAM_QUEUE_TARGET) * (Uint32)channels * 2u;
    while (s->stream_open && !s->stream_eof) {
        queued = SDL_GetQueuedAudioSize(s->audio_device);
        if (queued >= target_bytes) break;
        if (chunk_limit > STREAM_QUEUE_FRAMES) chunk_limit = STREAM_QUEUE_FRAMES;
        long long got = decoder_read_frames(s->decoder, buf, chunk_limit);
        if (got <= 0) { s->stream_eof = 1; break; }
        /* Software gain: the OS mixer is not a given on every target desktop,
         * so scale the PCM here. Attenuation only (0..100), so no clipping. */
        {
            int gain = s->muted ? 0 : s->volume;
            if (gain != 100) {
                long long n = got * channels, i;
                for (i = 0; i < n; i++) buf[i] = (short)((int)buf[i] * gain / 100);
            }
        }
        if (SDL_QueueAudio(s->audio_device, buf, (Uint32)((size_t)got * (size_t)channels * sizeof(short))) != 0) {
            s->stream_eof = 1;
            break;
        }
        s->audio_queued_frame += (Uint64)got;
    }
}

/* Volume/mute changes only affect PCM queued after them, so already-buffered
 * audio (up to ~0.75s) would keep playing at the old level. Drop the queue and
 * re-decode from the current play position so the change is heard at once. */
static void reapply_output_gain(AppState *s) {
    if (!s->audio_device || !s->decoder || !s->stream_open || s->audio_channels <= 0) return;
    Uint32 queued = SDL_GetQueuedAudioSize(s->audio_device);
    Uint64 fiq = queued / ((Uint32)s->audio_channels * 2u);
    Uint64 frame = (s->audio_queued_frame > fiq) ? s->audio_queued_frame - fiq : 0;
    if (decoder_seek(s->decoder, (long long)frame) < 0) return;
    SDL_ClearQueuedAudio(s->audio_device);
    s->audio_queued_frame = frame;
    s->stream_eof = 0;
    stream_audio(s);
}

static void seek_ms(AppState *s, Uint32 ms) {
    Uint64 rate = (Uint64)s->audio_rate;
    Uint64 frame;
    if (!s->audio_device || !s->decoder) { snprintf(s->status, sizeof(s->status), "No audio loaded to seek."); return; }
    if (!rate) return;
    frame = (Uint64)ms * rate / 1000u;
    if (frame > s->audio_total_frames) frame = s->audio_total_frames;
    if (decoder_seek(s->decoder, (long long)frame) < 0) return;
    SDL_ClearQueuedAudio(s->audio_device);
    s->audio_queued_frame = frame;
    s->stream_eof = 0;
    s->track_ended = 0;
    s->position_ms = (Uint32)(frame * 1000u / rate);
    stream_audio(s);
    SDL_PauseAudioDevice(s->audio_device, s->is_playing ? 0 : 1);
    snprintf(s->status, sizeof(s->status), "Seeked to %u:%02u.", s->position_ms / 60000, (s->position_ms / 1000) % 60);
    write_status(s);
    write_resume(s);
}

/* Called when `is_playing` but audio is not actually reaching the sink. Two
 * shapes of this failure both trace back to the backend starting before the
 * PipeWire graph has a usable output:
 *
 *   1. SDL_OpenAudioDevice reports success but the stream is never scheduled,
 *      so the queued PCM is never consumed and the position stays at 0.
 *   2. After a plain close/reopen the new SDL_AudioDeviceID drains its queue
 *      (the position advances) but is wired to nothing, so it plays silence.
 *
 * A bare SDL_CloseAudioDevice + SDL_OpenAudioDevice does not clear shape 2:
 * SDL keeps the same broken connection to the audio server. Tearing the whole
 * audio subsystem down and back up makes SDL drop and rebuild that connection,
 * exactly as a fresh process would, then we reopen and re-stream from where
 * playback sat. */
static void retry_audio_device(AppState *s) {
    SDL_AudioSpec desired = {0}, obtained = {0};
    Uint64 played;
    if (s->audio_device) SDL_CloseAudioDevice(s->audio_device);
    s->audio_device = 0;
    if (!s->decoder || s->audio_channels <= 0 || s->audio_rate <= 0) {
        s->is_playing = 0;
        write_status(s);
        return;
    }
    s->audio_retries++;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "leecher: SDL audio reinit failed (attempt %d): %s\n", s->audio_retries, SDL_GetError());
        snprintf(s->status, sizeof(s->status), "Audio unavailable, retrying...");
        write_status(s);
        return;
    }
    desired.freq = s->audio_rate; desired.format = AUDIO_S16SYS;
    desired.channels = (Uint8)s->audio_channels; desired.samples = 4096;
    s->audio_device = open_audio_device(s, &desired, &obtained);
    if (!s->audio_device) {
        fprintf(stderr, "leecher: SDL audio reopen failed (attempt %d): %s\n", s->audio_retries, SDL_GetError());
        snprintf(s->status, sizeof(s->status), "Audio unavailable, retrying...");
        write_status(s);
        return;
    }
    /* The prior queue was never drained; resume from where playback sat. */
    played = s->audio_queued_frame;
    if (decoder_seek(s->decoder, (long long)played) < 0) { decoder_seek(s->decoder, 0); played = 0; }
    s->audio_queued_frame = played;
    s->stream_eof = 0;
    s->track_ended = 0;
    stream_audio(s);
    SDL_PauseAudioDevice(s->audio_device, s->is_playing ? 0 : 1);
    fprintf(stderr, "leecher: rebuilt audio output (attempt %d, %d device(s) visible)\n",
            s->audio_retries, SDL_GetNumAudioDevices(0));
    snprintf(s->status, sizeof(s->status), "Rebuilt audio output.");
    write_status(s);
}

/* A fetch for `failed_index` came back with nothing. Publish a status that
 * names the track and the reason, so a source that never plays is visibly a
 * broken source rather than looking like a normal track change. `skipped` is
 * true when autoplay moved on to the next track, false for a direct request
 * that simply could not start. */
static void announce_fetch_failure(AppState *s, const LibraryHandler *library,
                                   size_t failed_index, int skipped) {
    LibraryTrack track = {0};
    char err[256] = {0};
    char reason[256];
    const char *title = NULL;
    if (library_handler_track_at(library, failed_index, &track, err, sizeof(err)) == 1 &&
        track.title && track.title[0])
        title = track.title;
    fetch_lock(s);
    snprintf(reason, sizeof(reason), "%s",
             s->fetch_error[0] ? s->fetch_error : "source unavailable");
    s->fetch_error[0] = '\0';
    fetch_unlock(s);
    if (title)
        snprintf(s->status, sizeof(s->status), "%s \"%.120s\": %.100s",
                 skipped ? "Skipped" : "Could not play", title, reason);
    else
        snprintf(s->status, sizeof(s->status), "%s track %zu: %.100s",
                 skipped ? "Skipped" : "Could not play", failed_index + 1, reason);
    /* Also log it: when autoplay skips, the status line is overwritten by the
     * next track within a frame, so the journal is the durable record that a
     * source is broken. */
    fprintf(stderr, "leecher: %s\n", s->status);
    library_handler_track_destroy(&track);
    write_status(s);
}

/* After a mutation reloads the viewed playlist, mirror it into the playback
 * handle when that is the same playlist, so next/autoplay see the same rows. */
static void resync_play_lib(AppState *s) {
    char err[128];
    LibraryHandler *h;
    if (strcmp(s->viewed_playlist, s->playing_playlist) != 0) return;
    h = library_handler_open(s->library_path, err, sizeof(err));
    if (!h) return;
    library_handler_close(s->play_lib);
    s->play_lib = h;
}

static void handle_set_field(LibraryHandler **library, MusicRipper *ripper, const char *library_path,
                             AppState *state, const char *prefix, const char *key, const char *command) {
    char *end;
    size_t idx = (size_t)strtoul(command + strlen(prefix), &end, 10);
    char error[256] = {0}, reload[256] = {0};
    const char *value = end;
    while (*value == ' ') value++;
    if (library_handler_update_track(library_path, idx, key[0] == 't' ? (*value ? value : NULL) : NULL,
                                     key[0] == 'a' && key[1] == 'r' ? (*value ? value : NULL) : NULL,
                                     key[0] == 'a' && key[1] == 'l' ? (*value ? value : NULL) : NULL,
                                     error, sizeof(error)) != 1) {
        snprintf(state->status, sizeof(state->status), "Edit failed: %s", error[0] ? error : "unknown error");
        return;
    }
    {
        LibraryHandler *reloaded = library_handler_open(library_path, reload, sizeof(reload));
        if (reloaded) { cancel_fetch(state); library_handler_close(*library); *library = reloaded; ripper->library = reloaded; resync_play_lib(state); }
        else snprintf(state->status, sizeof(state->status), "Updated, but reload failed: %s", reload);
    }
    snprintf(state->status, sizeof(state->status), "Updated %s for track %zu.", key, idx);
    write_status(state);
}

static void handle_remove_track(LibraryHandler **library, MusicRipper *ripper, const char *library_path,
                                AppState *state, const char *command) {
    char *end;
    size_t idx = (size_t)strtoul(command + 7, &end, 10);
    char error[256] = {0};
    if (library_handler_remove_track(library_path, idx, error, sizeof(error)) != 1) {
        snprintf(state->status, sizeof(state->status), "Remove failed: %s", error[0] ? error : "unknown error");
        return;
    }
    {
        char reload[256] = {0};
        LibraryHandler *reloaded = library_handler_open(library_path, reload, sizeof(reload));
        if (reloaded) { cancel_fetch(state); library_handler_close(*library); *library = reloaded; ripper->library = reloaded; resync_play_lib(state); }
        else snprintf(state->status, sizeof(state->status), "Removed, but reload failed: %s", reload);
    }
    if (idx == state->selected_track) {
        state->is_playing = 0;
        if (state->audio_device) SDL_ClearQueuedAudio(state->audio_device);
        state->position_ms = 0;
        state->title[0] = '\0';
    }
    /* Keep the play queue pointing at the right rows: drop the removed track,
     * shift entries that sat above it down by one. */
    {
        int i = 0;
        while (i < state->play_queue_len) {
            if (state->play_queue[i] == idx) { play_queue_remove_at(state, i); continue; }
            if (state->play_queue[i] > idx) state->play_queue[i]--;
            i++;
        }
    }
    snprintf(state->status, sizeof(state->status), "Removed track %zu.", idx);
    write_status(state);
}

/* Imports one local audio file requested by the bar widget.  Reuse the same
 * validation and metadata extraction path as drag-and-drop, so a control
 * command cannot add a directory or an unsupported source to the library. */
static void handle_add_local_track(LibraryHandler **library, MusicRipper *ripper,
                                   const char *library_path, AppState *state,
                                   const char *command) {
    const char *path = command + strlen("add_local ");
    while (*path == ' ' || *path == '\t') path++;
    if (!*path) {
        snprintf(state->status, sizeof(state->status), "Add failed: a local audio-file path is required.");
        return;
    }
    handle_dropped_file(library_path, state, path, library, ripper);
}

/* Control lines are percent-encoded by the widget so that command values
 * (titles/artists/albums) may contain newlines or other control characters
 * without corrupting the single-line channel.  Decode %XX back to bytes. */
static void control_decode(char *out, size_t out_size, const char *in) {
    size_t o = 0;
    while (*in && o + 1 < out_size) {
        if (in[0] == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2]) && o + 1 < out_size) {
            int hi = isdigit((unsigned char)in[1]) ? in[1] - '0' : (tolower((unsigned char)in[1]) - 'a' + 10);
            int lo = isdigit((unsigned char)in[2]) ? in[2] - '0' : (tolower((unsigned char)in[2]) - 'a' + 10);
            out[o++] = (char)((hi << 4) | lo);
            in += 3;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = '\0';
}

/* `set_fields <idx> <enc-title> <enc-artist> <enc-album>` from the widget's
 * sendControlFields(): the three values are percent-encoded (enc()) so they
 * contain no literal spaces; the tokens are separated by single spaces.  Decode
 * each field and apply all three in ONE atomic library write, avoiding the old
 * three separate set_* commands that could persist a half-applied edit. */
static void handle_set_fields(LibraryHandler **library, MusicRipper *ripper,
                              const char *library_path, AppState *state, const char *command) {
    const char *p = command + strlen("set_fields ");
    char *end;
    size_t idx = (size_t)strtoul(p, &end, 10);
    char error[256] = {0}, reload[256] = {0};
    const char *fields[3] = { NULL, NULL, NULL };
    char decoded[3][512];
    int i = 0;
    const char *tok = end;
    if (end == p) { snprintf(state->status, sizeof(state->status), "Edit failed: missing track index."); return; }
    while (i < 3 && *tok) {
        const char *sp;
        size_t len;
        while (*tok == ' ') tok++;          /* the index is followed by a space */
        if (!*tok) break;
        sp = tok;
        while (*sp && *sp != ' ') sp++;
        len = (size_t)(sp - tok);
        if (len >= sizeof(decoded[i])) len = sizeof(decoded[i]) - 1;
        memcpy(decoded[i], tok, len);
        decoded[i][len] = '\0';
        control_decode(decoded[i], sizeof(decoded[i]), decoded[i]);
        fields[i] = decoded[i];
        i++;
        tok = (*sp == ' ') ? sp + 1 : sp;
    }
    /* Missing trailing fields keep their NULL (unchanged); explicit empty values
     * are decoded to "" and will clear the field, as the widget always sends all
     * three current values. */
    if (library_handler_update_track(library_path, idx, fields[0], fields[1], fields[2],
                                     error, sizeof(error)) != 1) {
        snprintf(state->status, sizeof(state->status), "Edit failed: %s", error[0] ? error : "unknown error");
        return;
    }
    {
        LibraryHandler *reloaded = library_handler_open(library_path, reload, sizeof(reload));
        if (reloaded) { cancel_fetch(state); library_handler_close(*library); *library = reloaded; ripper->library = reloaded; resync_play_lib(state); }
        else snprintf(state->status, sizeof(state->status), "Updated, but reload failed: %s", reload);
    }
    snprintf(state->status, sizeof(state->status), "Updated track %zu.", idx);
    write_status(state);
}

/* `add_ssh` / `add_network` / `add_https` carry encoded fields, one per space,
 * exactly like `set_fields`, so they are routed before the whole-line decode in
 * poll_control.  Copy the next token (up to a literal space) and decode it in
 * place.  Advances *pp past the trailing space, if any.  Returns `out`. */
static const char *next_encoded_token(const char **pp, char *out, size_t size) {
    const char *p = *pp;
    const char *sp;
    size_t len;
    while (*p == ' ') p++;
    if (!*p) { out[0] = '\0'; *pp = p; return out; }
    sp = p;
    while (*sp && *sp != ' ') sp++;
    len = (size_t)(sp - p);
    if (len >= size) len = size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    control_decode(out, size, out);
    *pp = (*sp == ' ') ? sp + 1 : sp;
    return out;
}

/* Imports one source requested by the bar widget over SSH or the local network.
 * `kind` is LIBRARY_SOURCE_SSH or LIBRARY_SOURCE_NETWORK; both use
 * username/host/path with the SSH transport and identical field layouts. */
static void handle_add_ssh_network_track(LibraryHandler **library, MusicRipper *ripper,
                                         const char *library_path, AppState *state,
                                         const char *command, LibrarySourceKind kind) {
    const char *p = command + (kind == LIBRARY_SOURCE_SSH ? 8 : 12);
    char username[128], host[128], remote_path[512];
    char title[256], artist[256], album[256];
    LibrarySongQuery song;
    LibrarySource source;

    next_encoded_token(&p, username, sizeof(username));
    next_encoded_token(&p, host, sizeof(host));
    next_encoded_token(&p, remote_path, sizeof(remote_path));

    if (!*username || !*host || !*remote_path) {
        snprintf(state->status, sizeof(state->status), "Add failed: username, host, and remote path are required.");
        return;
    }
    if (!valid_ssh_name(username, 0) || !valid_ssh_name(host, 1)) {
        snprintf(state->status, sizeof(state->status), "Add failed: invalid username or host.");
        return;
    }
    if (!is_audio_name(remote_path)) {
        snprintf(state->status, sizeof(state->status), "Add failed: not an audio file: %.160s", remote_path);
        return;
    }
    song = build_song_query_for(state, remote_path, username, host, NULL,
                                title, sizeof(title), artist, sizeof(artist), album, sizeof(album));
    source = (LibrarySource){ .kind = kind, .path = remote_path, .username = username, .ip = host };
    if (import_single_source(library_path, state, &song, &source, library, ripper))
        snprintf(state->status, sizeof(state->status), "Imported %s source %.160s (%.60s).",
                 kind == LIBRARY_SOURCE_SSH ? "SSH" : "network", remote_path, host);
}

/* Imports one https:// stream requested by the bar widget.  HTTPS URLs are
 * accepted as-is (no audio-extension check: a stream may not end in .mp3). */
static void handle_add_https_track(LibraryHandler **library, MusicRipper *ripper,
                                   const char *library_path, AppState *state,
                                   const char *command) {
    const char *p = command + 10;
    char url[512];
    char title[256], artist[256], album[256];
    LibrarySongQuery song;
    LibrarySource source;

    next_encoded_token(&p, url, sizeof(url));
    if (!*url) {
        snprintf(state->status, sizeof(state->status), "Add failed: an https:// URL is required.");
        return;
    }
    if (strncmp(url, "https://", 8) != 0) {
        snprintf(state->status, sizeof(state->status), "Add failed: only https:// URLs are supported.");
        return;
    }
    song = build_song_query_for(state, url, NULL, NULL, url,
                                title, sizeof(title), artist, sizeof(artist), album, sizeof(album));
    source = (LibrarySource){ .kind = LIBRARY_SOURCE_HTTPS, .url = url };
    if (import_single_source(library_path, state, &song, &source, library, ripper))
        snprintf(state->status, sizeof(state->status), "Imported https source %.120s", url);
}

/* Switch the VIEWED playlist: reopen the display/mutation handle at its file.
 * Playback is untouched -- it keeps running over playing_playlist. */
static void switch_viewed_playlist(LibraryHandler **library, MusicRipper *ripper,
                                   AppState *state, const char *name) {
    char path[LIBRARY_PATH_MAX], err[128];
    LibraryHandler *h;
    if (!playlist_known(state, name)) {
        snprintf(state->status, sizeof(state->status), "No such playlist: %.80s", name);
        write_status(state);
        return;
    }
    if (!strcmp(name, state->viewed_playlist)) return;
    playlist_file_path(state, name, path, sizeof(path));
    h = library_handler_open(path, err, sizeof(err));
    if (!h) {
        snprintf(state->status, sizeof(state->status), "Cannot open playlist %.60s: %.80s", name, err);
        write_status(state);
        return;
    }
    library_handler_close(*library);
    *library = h;
    if (ripper) ripper->library = h;
    snprintf(state->viewed_playlist, sizeof(state->viewed_playlist), "%s", name);
    snprintf(state->library_path, sizeof(state->library_path), "%s", path);
    snprintf(state->status, sizeof(state->status), "Viewing playlist \"%s\".", name);
    write_status(state);
}

/* Create a new empty playlist file and switch to viewing it. */
static void create_playlist(LibraryHandler **library, MusicRipper *ripper,
                            AppState *state, const char *name) {
    char path[LIBRARY_PATH_MAX];
    if (!valid_playlist_name(name)) {
        snprintf(state->status, sizeof(state->status),
                 "Invalid playlist name (letters, digits, spaces, - and _; up to 64).");
        write_status(state);
        return;
    }
    if (playlist_known(state, name)) { switch_viewed_playlist(library, ripper, state, name); return; }
    if (state->playlist_count >= (int)(sizeof(state->playlists) / sizeof(state->playlists[0]))) {
        snprintf(state->status, sizeof(state->status), "Too many playlists.");
        write_status(state);
        return;
    }
    playlist_file_path(state, name, path, sizeof(path));
    if (!atomic_write(path, EMPTY_LIBRARY_JSON, sizeof(EMPTY_LIBRARY_JSON) - 1)) {
        snprintf(state->status, sizeof(state->status), "Could not create playlist \"%.60s\".", name);
        write_status(state);
        return;
    }
    scan_playlists(state);
    switch_viewed_playlist(library, ripper, state, name);
    snprintf(state->status, sizeof(state->status), "Created playlist \"%s\".", name);
    write_status(state);
}

static void handle_control(const char *command, LibraryHandler **library, MusicRipper *ripper,
                           Assembler *assembler, AppState *state, const char *library_path) {
    /* Commands arrive as one line, e.g.: "play_pause", "next", "previous",
     * "seek 45000", "play 16", "set_fields 3 ...", "remove 4",
     * "add_local /path/to/song.flac",
     * "autoplay on". Called from the main loop once per frame. */
    while (command && *command && (*command == ' ' || *command == '\t' || *command == '\n' || *command == '\r')) command++;
    if (!command || !*command) return;
    (void)assembler;
    if (!strncmp(command, "play_pause", 10)) toggle_play_pause(state);
    else if (!strncmp(command, "play ", 5)) play_library_index(state, (size_t)strtoul(command + 5, NULL, 10));
    else if (!strncmp(command, "next", 4)) play_library_relative(state, +1);
    else if (!strncmp(command, "previous", 8)) play_library_relative(state, -1);
    else if (!strncmp(command, "playlist_new ", 13)) {
        const char *n = command + 13; while (*n == ' ') n++;
        create_playlist(library, ripper, state, n);
    }
    else if (!strncmp(command, "playlist ", 9)) {
        const char *n = command + 9; while (*n == ' ') n++;
        switch_viewed_playlist(library, ripper, state, n);
    }
    else if (!strncmp(command, "seek ", 5)) { long v = strtol(command + 5, NULL, 10); if (v < 0) v = 0; seek_ms(state, (Uint32)v); }
    else if (!strncmp(command, "autoplay ", 9)) {
        state->autoplay = (strncmp(command + 9, "off", 3) == 0) ? 0 : 1;
        snprintf(state->status, sizeof(state->status), state->autoplay ? "Autoplay enabled." : "Autoplay disabled.");
        write_status(state);
    }
    else if (!strncmp(command, "shuffle ", 8)) {
        state->shuffle = (strncmp(command + 8, "off", 3) == 0) ? 0 : 1;
        snprintf(state->status, sizeof(state->status), state->shuffle ? "Shuffle on." : "Shuffle off.");
        write_status(state);
    }
    else if (!strncmp(command, "repeat ", 7)) {
        state->repeat_one = (strncmp(command + 7, "one", 3) == 0) ? 1 : 0;
        snprintf(state->status, sizeof(state->status), state->repeat_one ? "Repeat one." : "Repeat off.");
        write_status(state);
    }
    else if (!strncmp(command, "volume ", 7)) {
        long v = strtol(command + 7, NULL, 10);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        state->volume = (int)v;
        reapply_output_gain(state);
        snprintf(state->status, sizeof(state->status), "Volume %d%%.", state->volume);
        write_status(state);
    }
    else if (!strncmp(command, "mute ", 5)) {
        state->muted = (strncmp(command + 5, "on", 2) == 0) ? 1 : 0;
        reapply_output_gain(state);
        snprintf(state->status, sizeof(state->status), state->muted ? "Muted." : "Unmuted.");
        write_status(state);
    }
    else if (!strncmp(command, "queue_clear", 11)) {
        state->play_queue_len = 0;
        snprintf(state->status, sizeof(state->status), "Play queue cleared.");
        write_status(state);
    }
    else if (!strncmp(command, "queue ", 6)) {
        size_t count = state->play_lib ? library_handler_track_count(state->play_lib) : 0;
        size_t idx = (size_t)strtoul(command + 6, NULL, 10);
        if (idx >= count) {
            snprintf(state->status, sizeof(state->status), "Cannot queue track %zu.", idx + 1);
        } else if (state->play_queue_len >= PLAY_QUEUE_CAP) {
            snprintf(state->status, sizeof(state->status), "Play queue is full (%d).", PLAY_QUEUE_CAP);
        } else {
            play_queue_push(state, idx);
            snprintf(state->status, sizeof(state->status), "Queued (%d in queue).", state->play_queue_len);
        }
        write_status(state);
    }
    else if (!strncmp(command, "unqueue ", 8)) {
        /* Remove the first queued occurrence of a library index. */
        size_t idx = (size_t)strtoul(command + 8, NULL, 10);
        int i;
        for (i = 0; i < state->play_queue_len; i++) {
            if (state->play_queue[i] == idx) { play_queue_remove_at(state, i); break; }
        }
        snprintf(state->status, sizeof(state->status), "Play queue: %d.", state->play_queue_len);
        write_status(state);
    }
    else if (!strncmp(command, "output ", 7)) {
        const char *name = command + 7;
        while (*name == ' ') name++;
        if (!*name || !strcmp(name, "default")) {
            state->audio_device_name[0] = '\0';
            snprintf(state->status, sizeof(state->status), "Output: system default.");
        } else {
            /* Only accept a name SDL currently lists, so a stale or mistyped
             * choice is rejected now rather than at the next track open. */
            int di, dc = SDL_GetNumAudioDevices(0), found = 0;
            for (di = 0; di < dc; di++) {
                const char *dn = SDL_GetAudioDeviceName(di, 0);
                if (dn && !strcmp(dn, name)) { found = 1; break; }
            }
            if (!found) {
                snprintf(state->status, sizeof(state->status), "No such output: %.90s", name);
                write_status(state);
                return;
            }
            snprintf(state->audio_device_name, sizeof(state->audio_device_name), "%s", name);
            snprintf(state->status, sizeof(state->status), "Output: %.90s", name);
        }
        if (state->audio_device && state->decoder)
            reopen_audio_device(state);
        write_status(state);
    }
    else if (!strncmp(command, "set_title ", 10)) handle_set_field(library, ripper, library_path, state, "set_title ", "title", command);
    else if (!strncmp(command, "set_artist ", 11)) handle_set_field(library, ripper, library_path, state, "set_artist ", "artist", command);
    else if (!strncmp(command, "set_album ", 10)) handle_set_field(library, ripper, library_path, state, "set_album ", "album", command);
    else if (!strncmp(command, "remove ", 7)) handle_remove_track(library, ripper, library_path, state, command);
    else if (!strncmp(command, "add_local ", 10)) handle_add_local_track(library, ripper, library_path, state, command);
    else snprintf(state->status, sizeof(state->status), "Unknown control command: %.220s", command);
}

static void poll_control(LibraryHandler **library, const MusicRipper *ripper,
                         Assembler *assembler, AppState *state, const char *library_path) {
    struct stat st;
    int fd = open(control_file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    FILE *file;
    char line[512];
    if (fd < 0) return;
    /* Only accept a control file we own and that is a regular file; reject
     * symlinks and attacker-owned files outright. */
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
        close(fd);
        unlink(control_file);
        return;
    }
    file = fdopen(fd, "r");
    if (!file) { close(fd); unlink(control_file); return; }
    line[0] = '\0';
    if (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        /* Optional command acknowledgment: a numeric id followed by a space is
         * a client-generated token echoed back as `cmd_id` in the status JSON
         * so the widget can confirm its command was received and processed. */
        {
            unsigned long id = 0;
            char *end = NULL;
            char decoded[512];
            const char *cmd = line;
            if (isdigit((unsigned char)line[0])) {
                id = strtoul(line, &end, 10);
                if (end != line && *end == ' ') cmd = end + 1;
                else id = 0;
            }
            if (id != 0) state->last_cmd_id = id;
            /* set_fields, add_ssh, add_network, and add_https carry encoded
             * fields that must be split on the still-encoded line (values may
             * contain literal spaces after decode), so route them before the
             * whole-line decode. */
            if (strncmp(cmd, "set_fields ", 11) == 0) {
                handle_set_fields(library, (MusicRipper *)ripper, library_path, state, cmd);
            } else if (strncmp(cmd, "add_ssh ", 8) == 0) {
                handle_add_ssh_network_track(library, (MusicRipper *)ripper, library_path, state, cmd, LIBRARY_SOURCE_SSH);
            } else if (strncmp(cmd, "add_network ", 12) == 0) {
                handle_add_ssh_network_track(library, (MusicRipper *)ripper, library_path, state, cmd, LIBRARY_SOURCE_NETWORK);
            } else if (strncmp(cmd, "add_https ", 10) == 0) {
                handle_add_https_track(library, (MusicRipper *)ripper, library_path, state, cmd);
            } else {
                control_decode(decoded, sizeof(decoded), cmd);
                handle_control(decoded, library, (MusicRipper *)ripper, assembler, state, library_path);
            }
        }
    }
    fclose(file);
    /* A client writes the control line non-atomically (truncate then write), so
     * a poll landing in that gap sees an empty file. Don't unlink it then --
     * the pending write would be lost and the command silently dropped. Leave
     * it for the next poll; a genuinely empty file just gets re-read once. */
    if (line[0] != '\0' || st.st_size > 0)
        unlink(control_file);
    write_status(state);
    (void)0;
}

static void draw_library(struct nk_context *ctx, const LibraryHandler *library, AppState *state) {
    size_t count = library_handler_track_count(library), i;
    if (!nk_group_begin(ctx, "library-scroll", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) return;
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_label(ctx, "LIBRARY", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, "Pick a song to play next", NK_TEXT_LEFT);
    for (i = 0; i < count; i++) {
        LibraryTrack track = {0};
        char label[384];
        if (library_handler_track_at(library, i, &track, NULL, 0) != 1) continue;
        snprintf(label, sizeof(label), "%s  |  %s%s%s", track.title ? track.title : "Untitled", track.artist ? track.artist : "Unknown artist", track.album ? "  -  " : "", track.album ? track.album : "");
        nk_layout_row_dynamic(ctx, 34, 1);
        if (nk_button_label(ctx, label)) {
            play_library_index(state, i);
        }
        library_handler_track_destroy(&track);
    }
    if (!count) { nk_layout_row_dynamic(ctx, 28, 1); nk_label(ctx, "No tracks in this library.", NK_TEXT_LEFT); }
    nk_group_end(ctx);
}

static void draw_scraper(struct nk_context *ctx, AppState *state, LibraryHandler **library,
                         const char *library_path, MusicRipper *ripper) {
    static const char *methods[] = { "Local file", "SSH", "HTTPS", "Local network" };
    if (!nk_group_begin(ctx, "scraper-scroll", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) return;
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_label(ctx, "SOURCE SCRAPER", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "Build a source entry for the selected song", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 28, 1);
    if (nk_combo_begin_label(ctx, method_name(state->method), nk_vec2(nk_widget_width(ctx), 140))) {
        int i;
        /* Combo items need their own row layout inside Nuklear's popup. */
        nk_layout_row_dynamic(ctx, 28, 1);
        for (i = 0; i < 4; i++) if (nk_combo_item_label(ctx, methods[i], NK_TEXT_LEFT)) state->method = (SourceMethod)i;
        nk_combo_end(ctx);
    }
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_label(ctx, "Title", NK_TEXT_LEFT); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, state->title, sizeof(state->title), nk_filter_default);
    nk_label(ctx, "Artist", NK_TEXT_LEFT); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, state->artist, sizeof(state->artist), nk_filter_default);
    nk_label(ctx, "Album", NK_TEXT_LEFT); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, state->album, sizeof(state->album), nk_filter_default);
    if (state->method == SOURCE_LOCAL || state->method == SOURCE_SSH || state->method == SOURCE_NETWORK) {
        nk_label(ctx, "PATH", NK_TEXT_LEFT); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, state->path, sizeof(state->path), nk_filter_default);
        if (state->method == SOURCE_LOCAL) {
            nk_layout_row_dynamic(ctx, 28, 1);
            if (nk_button_label(ctx, "Choose local music file")) choose_local_file(state);
            if (nk_button_label(ctx, "Pull all unique songs from path")) pull_local_songs(library_path, state, state->path, library, ripper);
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label(ctx, "Drag audio files from your file manager into this window to import them.", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 24, 1);
        }
    }
    if (state->method == SOURCE_SSH || state->method == SOURCE_NETWORK) {
        nk_label(ctx, "USERNAME", NK_TEXT_LEFT); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, state->username, sizeof(state->username), nk_filter_default);
        nk_label(ctx, "IP", NK_TEXT_LEFT); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, state->ip, sizeof(state->ip), nk_filter_default);
        if (state->method == SOURCE_SSH) {
            nk_layout_row_dynamic(ctx, 28, 1);
            if (nk_button_label(ctx, "Choose remote music file")) choose_ssh_file(state);
            if (nk_button_label(ctx, "Extract SSH metadata")) extract_ssh_metadata(state);
            if (nk_button_label(ctx, "Pull all unique songs from path")) pull_ssh_songs(library_path, state, library, ripper);
            nk_layout_row_dynamic(ctx, 24, 1);
        }
    }
    if (state->method == SOURCE_HTTPS) {
        nk_label(ctx, "URL", NK_TEXT_LEFT); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, state->url, sizeof(state->url), nk_filter_default);
    }
    nk_layout_row_dynamic(ctx, 30, 1);
    if (nk_button_label(ctx, "Save source to library")) {
        LibrarySongQuery song = { .title = state->title, .artist = state->artist, .album = state->album };
        LibrarySource source = { .kind = library_kind(state->method), .path = state->path[0] ? state->path : NULL, .username = state->username[0] ? state->username : NULL, .url = state->url[0] ? state->url : NULL, .ip = state->ip[0] ? state->ip : NULL };
        char error[256] = {0};
        if (library_handler_add_source(library_path, &song, &source, error, sizeof(error)) == 1) {
            LibraryHandler *reloaded = library_handler_open(library_path, error, sizeof(error));
            if (reloaded) { cancel_fetch(state); library_handler_close(*library); *library = reloaded; ripper->library = reloaded; resync_play_lib(state); snprintf(state->status, sizeof(state->status), "%s source saved to the library.", method_name(state->method)); }
            else snprintf(state->status, sizeof(state->status), "Saved source, but reload failed: %s", error);
        } else snprintf(state->status, sizeof(state->status), "%s", error);
    }
    nk_layout_row_dynamic(ctx, 1, 1); nk_spacing(ctx, 1);
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, "Credentials stay with your system agents.", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 30, 2);
    if (nk_button_label(ctx, "Unlock SSH agent")) unlock_ssh_agent(state);
    if (nk_button_label(ctx, "Network settings")) launch_credential_agent("nm-connection-editor", state, "System network settings opened. This app never receives network passwords.");
    nk_layout_row_dynamic(ctx, 44, 1);
    nk_label_wrap(ctx, "A kernel cannot present remote-login prompts. SSH agents and the desktop network/keyring service own authentication; the app only supplies PATH, USERNAME, URL, and IP.");
    nk_group_end(ctx);
}

static void apply_theme_key(ThemePalette *p, const char *key, const char *hex) {
    unsigned long v = 0;
    if (!hex || sscanf(hex, "#%lx", &v) != 1) return;
    RGB c = { (unsigned char)((v >> 16) & 0xFF), (unsigned char)((v >> 8) & 0xFF), (unsigned char)(v & 0xFF) };
    if (!strcmp(key, "background")) p->background = c;
    else if (!strcmp(key, "dark_background") || !strcmp(key, "darker_background")) p->darker = c;
    else if (!strcmp(key, "lighter_background")) p->lighter = c;
    else if (!strcmp(key, "selection")) p->selection = c;
    else if (!strcmp(key, "foreground")) p->foreground = c;
    else if (!strcmp(key, "accent")) p->accent = c;
    else if (!strcmp(key, "muted")) p->muted = c;
    else if (!strcmp(key, "red") || !strcmp(key, "color1")) p->urgent = c;
}

static void parse_colors_line(ThemePalette *p, const char *line) {
    const char *eq = strchr(line, '='), *hash;
    char key[64];
    size_t klen, start = 0;
    if (!eq) return;
    klen = (size_t)(eq - line);
    while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t')) klen--;
    while (start < klen && (line[start] == ' ' || line[start] == '\t')) start++;
    if (klen == 0 || klen - start >= sizeof(key)) return;
    memcpy(key, line + start, klen - start); key[klen - start] = '\0';
    hash = strchr(eq, '#');
    if (!hash) return;
    apply_theme_key(p, key, hash);
}

static void parse_colors_file(const char *path, ThemePalette *p) {
    FILE *f = fopen(path, "r");
    char line[256];
    if (!f) return;
    while (fgets(line, sizeof(line), f)) parse_colors_line(p, line);
    fclose(f);
}

static void load_theme(ThemePalette *p) {
    const char *home = getenv("HOME");
    char path[1024], name[256] = "tokyo-night";
    FILE *f;
    if (!home) home = "";
    *p = (ThemePalette){
        .background = {26, 27, 38}, .darker = {19, 20, 28}, .lighter = {36, 40, 59},
        .selection = {41, 46, 66}, .foreground = {169, 177, 214},
        .accent = {122, 162, 247}, .muted = {65, 72, 104}, .urgent = {247, 118, 142}
    };
    snprintf(path, sizeof(path), "%s/.local/state/omarchy/current/theme.name", home);
    f = fopen(path, "r");
    if (f) { if (fgets(name, sizeof(name), f)) name[strcspn(name, "\r\n")] = '\0'; fclose(f); }
    snprintf(path, sizeof(path), "/usr/share/omarchy/themes/%s/colors.toml", name);
    parse_colors_file(path, p);
    snprintf(path, sizeof(path), "%s/.config/omarchy/themes/%s/colors.toml", home, name);
    parse_colors_file(path, p);
}

static void apply_theme(struct nk_context *ctx, const ThemePalette *p, struct nk_font *font) {
    struct nk_color colors[NK_COLOR_COUNT];
    struct nk_color bg = nk_rgb(p->background.r, p->background.g, p->background.b);
    struct nk_color fg = nk_rgb(p->foreground.r, p->foreground.g, p->foreground.b);
    struct nk_color accent = nk_rgb(p->accent.r, p->accent.g, p->accent.b);
    struct nk_color sel = nk_rgb(p->selection.r, p->selection.g, p->selection.b);
    struct nk_color lighter = nk_rgb(p->lighter.r, p->lighter.g, p->lighter.b);
    struct nk_color darker = nk_rgb(p->darker.r, p->darker.g, p->darker.b);
    struct nk_color fg_border = nk_rgba(fg.r, fg.g, fg.b, 92);
    struct nk_color fg_soft = nk_rgba(fg.r, fg.g, fg.b, 40);
    struct nk_style *s;

    memset(colors, 0, sizeof(colors));
    colors[NK_COLOR_TEXT] = fg;
    colors[NK_COLOR_WINDOW] = bg;
    colors[NK_COLOR_HEADER] = lighter;
    colors[NK_COLOR_BORDER] = accent;
    colors[NK_COLOR_BUTTON] = lighter;
    colors[NK_COLOR_BUTTON_HOVER] = sel;
    colors[NK_COLOR_BUTTON_ACTIVE] = accent;
    colors[NK_COLOR_EDIT] = darker;
    colors[NK_COLOR_EDIT_CURSOR] = accent;
    colors[NK_COLOR_PROPERTY] = darker;
    colors[NK_COLOR_COMBO] = sel;
    colors[NK_COLOR_SCROLLBAR] = sel;
    colors[NK_COLOR_SCROLLBAR_CURSOR] = accent;
    nk_style_from_table(ctx, colors);

    if (font) ctx->style.font = &font->handle;
    s = &ctx->style;

    s->text.color = fg;

    s->window.background = bg;
    s->window.fixed_background = nk_style_item_color(bg);
    s->window.border = 0.0f;
    s->window.rounding = 0.0f;
    s->window.padding = nk_vec2(18, 14);
    s->window.group_padding = nk_vec2(2, 2);
    s->window.spacing = nk_vec2(0, 10);
    s->window.group_border = 1.0f;
    s->window.group_border_color = fg_soft;

    s->window.header.normal = nk_style_item_color(bg);
    s->window.header.hover = nk_style_item_color(bg);
    s->window.header.active = nk_style_item_color(bg);
    s->window.header.label_normal = fg;
    s->window.header.label_hover = fg;
    s->window.header.label_active = fg;

    s->button.border = 1.0f;
    s->button.rounding = 0.0f;
    s->button.padding = nk_vec2(10, 6);
    s->button.border_color = fg_border;
    s->button.normal = nk_style_item_color(nk_rgba(fg.r, fg.g, fg.b, 13));
    s->button.hover = nk_style_item_color(nk_rgba(fg.r, fg.g, fg.b, 26));
    s->button.active = nk_style_item_color(nk_rgba(fg.r, fg.g, fg.b, 46));
    s->button.text_normal = fg;
    s->button.text_hover = fg;
    s->button.text_active = fg;

    s->contextual_button = s->button;
    s->menu_button = s->button;

    s->edit.border = 1.0f;
    s->edit.rounding = 0.0f;
    s->edit.padding = nk_vec2(8, 7);
    s->edit.normal = nk_style_item_color(darker);
    s->edit.hover = nk_style_item_color(darker);
    s->edit.active = nk_style_item_color(darker);
    s->edit.border_color = fg_border;
    s->edit.cursor_normal = accent;
    s->edit.cursor_hover = accent;
    s->edit.text_normal = fg;
    s->edit.text_hover = fg;
    s->edit.text_active = fg;
    s->edit.selected_normal = accent;
    s->edit.selected_hover = accent;
    s->edit.selected_text_normal = bg;
    s->edit.selected_text_hover = bg;

    s->combo.border = 1.0f;
    s->combo.rounding = 0.0f;
    s->combo.content_padding = nk_vec2(10, 6);
    s->combo.normal = nk_style_item_color(nk_rgba(fg.r, fg.g, fg.b, 13));
    s->combo.hover = nk_style_item_color(nk_rgba(fg.r, fg.g, fg.b, 26));
    s->combo.active = nk_style_item_color(nk_rgba(fg.r, fg.g, fg.b, 26));
    s->combo.border_color = fg_border;
    s->combo.label_normal = fg;
    s->combo.label_hover = fg;
    s->combo.label_active = fg;
    s->combo.button = s->button;

    s->property.border = 1.0f;
    s->property.rounding = 0.0f;
    s->property.border_color = fg_border;
    s->property.normal = nk_style_item_color(darker);
    s->property.hover = nk_style_item_color(darker);
    s->property.active = nk_style_item_color(darker);
    s->property.label_normal = fg;
    s->property.label_hover = fg;
    s->property.label_active = fg;

    s->scrollh.border = 0.0f;
    s->scrollh.rounding = 0.0f;
    s->scrollh.normal = nk_style_item_color(bg);
    s->scrollh.hover = nk_style_item_color(bg);
    s->scrollh.cursor_normal = nk_style_item_color(nk_rgba(fg.r, fg.g, fg.b, 92));
    s->scrollh.cursor_hover = nk_style_item_color(accent);
    s->scrollh.cursor_active = nk_style_item_color(accent);
    s->scrollv = s->scrollh;

    s->slider.border = 0.0f;
    s->slider.rounding = 0.0f;
    s->slider.bar_normal = fg_soft;
    s->slider.bar_hover = nk_rgba(fg.r, fg.g, fg.b, 60);
    s->slider.bar_active = nk_rgba(fg.r, fg.g, fg.b, 60);
    s->slider.bar_filled = accent;
    s->slider.cursor_normal = nk_style_item_color(accent);
    s->slider.cursor_hover = nk_style_item_color(accent);
    s->slider.cursor_active = nk_style_item_color(accent);
}

int main(int argc, char **argv) {
    int headless = argc > 1 && !strcmp(argv[1], "--headless");
    const char *library_arg = headless ? (argc > 2 ? argv[2] : "library.json")
                                       : (argc > 1 ? argv[1] : "library.json");
    char error[256] = {0};
    LibraryHandler *library = NULL;
    MusicRipper ripper = {0};
    Assembler *assembler;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    struct nk_context *ctx = NULL;
    SDL_Event event;
    AppState state = {0};
    state.method = SOURCE_LOCAL;
    state.autoplay = 1;
    state.volume = 100;
    srand((unsigned)(time(NULL) ^ ((long)getpid() << 8)));  /* shuffle next-track picks */
    state.fetch_index = (size_t)-1;
    state.immediate_index = (size_t)-1;
    state.pending_index = (size_t)-1;
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    pthread_mutex_init(&state.fetch_mutex, NULL);
    init_ipc_dir();

    /* Resolve the library directory (migrating a legacy single-file library the
     * first time) and choose the playlist to open. The playlist that was
     * playing at the last clean shutdown is restored from .resume.json. */
    {
        char legacy[PATH_MAX];
        char rpl[96];
        size_t rt; Uint32 rpos; int rplay;
        resolve_library_dir(library_arg, &state, legacy, sizeof(legacy));
        init_resume_path(state.library_dir);
        snprintf(state.viewed_playlist, sizeof(state.viewed_playlist), "%s",
                 state.playlist_count > 0 ? state.playlists[0] : "home");
        if (read_resume(rpl, sizeof(rpl), &rt, &rpos, &rplay) && rpl[0] && playlist_known(&state, rpl))
            snprintf(state.viewed_playlist, sizeof(state.viewed_playlist), "%s", rpl);
        snprintf(state.playing_playlist, sizeof(state.playing_playlist), "%s", state.viewed_playlist);
        playlist_file_path(&state, state.viewed_playlist, state.library_path, sizeof(state.library_path));
    }
    library = library_handler_open(state.library_path, error, sizeof(error));
    state.play_lib = library ? library_handler_open(state.library_path, error, sizeof(error)) : NULL;
    int running = 1;
    unsigned pos_tick = 0;
    struct nk_font_atlas *font_atlas;
    ThemePalette palette;
    if (!library || !state.play_lib) { fprintf(stderr, "Cannot load %s: %s\n", state.library_path, error); return 1; }
    ripper.library = library;
    ripper.transports.ssh = stream_ssh;
    ripper.transports.https = stream_https;
    ripper.transports.network = stream_ssh;
    state.transports = ripper.transports;
    assembler = assembler_create(NULL);
    if (!assembler) { fprintf(stderr, "Cannot create assembler queue\n"); library_handler_close(library); return 1; }
    if (SDL_Init(headless ? SDL_INIT_AUDIO : SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); assembler_destroy(assembler); library_handler_close(library); return 1; }
    if (!headless) {
        window = SDL_CreateWindow("Leecher Music Player", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1160, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
        if (!window || !renderer) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); if (renderer) SDL_DestroyRenderer(renderer); if (window) SDL_DestroyWindow(window); SDL_Quit(); assembler_destroy(assembler); library_handler_close(library); return 1; }
        ctx = nk_sdl_init(window, renderer);
        nk_sdl_font_stash_begin(&font_atlas);
        SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    }
    struct nk_font *ui_font = NULL;
    if (!headless) {
        FILE *ff = fopen("/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf", "rb");
        if (ff) {
            fseek(ff, 0, SEEK_END);
            long fsz = ftell(ff);
            fseek(ff, 0, SEEK_SET);
            if (fsz > 0 && fsz <= (1L << 28)) {
                unsigned char *blob = (unsigned char *)malloc((size_t)fsz);
                if (blob && fread(blob, 1, (size_t)fsz, ff) == (size_t)fsz)
                    ui_font = nk_font_atlas_add_from_memory(font_atlas, blob, (size_t)fsz, 13, NULL);
            }
            fclose(ff);
        }
    }
    if (!headless) {
        nk_sdl_font_stash_end();
        load_theme(&palette);
        apply_theme(ctx, &palette, ui_font);
    }
    write_status(&state);
    /* With autoplay on, start playing at once so the widget isn't idle. Resume
     * the track + position from the last run when the resume file still points
     * at a real track; otherwise start from the top. */
    if (state.autoplay && !state.immediate_pending && !state.pending_valid && !state.audio_device && !state.autoplay_advancing) {
        size_t count = library_handler_track_count(state.play_lib);
        if (count > 0) {
            size_t start = 0, rt;
            Uint32 rpos;
            int rplay;
            if (read_resume(NULL, 0, &rt, &rpos, &rplay) && rt < count) {
                start = rt;
                state.resume_position_ms = rpos;
                state.resume_paused = !rplay;
            }
            state.autoplay_advancing = 1;
            request_play(&state, start);
        }
    }
    while (running && !stop_requested) {
        poll_control(&library, &ripper, assembler, &state, state.library_path);

        /* Keep SDL's audio queue refilled from the decoder's rolling window. */
        stream_audio(&state);

        /* Fulfil an immediate track request as soon as its fetch is ready. */
        if (state.immediate_pending) {
            fetch_lock(&state);
            int ready = state.fetch_ready && state.fetch_index == state.immediate_index;
            int failed = !state.fetch_active && !state.fetch_ready;
            fetch_unlock(&state);
            if (ready) {
                commit_fetch(&state, state.immediate_index);
            } else if (failed) {
                size_t count = library_handler_track_count(state.play_lib);
                state.immediate_pending = 0;
                if (state.autoplay && state.autoplay_advancing && count > 1) {
                    size_t nxt = take_next_index(&state, count, state.immediate_index, 0);
                    announce_fetch_failure(&state, state.play_lib, state.immediate_index, 1);
                    request_play(&state, nxt);
                } else {
                    announce_fetch_failure(&state, state.play_lib, state.immediate_index, 0);
                }
            }
        }

        /* Prefetch the next track shortly before the current one ends. Skipped
         * under repeat-one: the track-end handler just seeks back to 0. */
        if (state.autoplay && !state.repeat_one && !state.immediate_pending && !state.pending_valid &&
            state.audio_device && state.duration_ms > 0 && state.is_playing) {
            size_t count = library_handler_track_count(state.play_lib);
            Uint32 p = playback_ms(&state);
            Uint32 trigger = state.duration_ms > 30000 ? state.duration_ms - 20000 : state.duration_ms / 2;
            /* take_next_index() pops the queue, so only call it once we have
             * actually decided to prefetch -- not every frame. */
            if (count > 0 && p >= trigger) {
                size_t next = take_next_index(&state, count, state.selected_track, 0);
                fetch_lock(&state);
                int already = state.fetch_active && state.fetch_index == next && !state.fetch_ready;
                int cached = state.fetch_ready && state.fetch_index == next;
                fetch_unlock(&state);
                state.pending_valid = 1;
                state.pending_index = next;
                state.autoplay_advancing = 1;
                if (!already && !cached)
                    start_fetch(&state, next);
            }
        }

        /* Commit a prefetched or autoplay-next track as soon as its fetch is
         * ready, once the current track has ended. Without this a slow source
         * (HTTPS/network) that was still downloading when the track ended would
         * finish later and never be committed, stalling autoplay. */
        if (state.track_ended && state.pending_valid) {
            size_t idx = state.pending_index;
            fetch_lock(&state);
            int pending_ready = state.fetch_ready && state.fetch_index == idx;
            int pending_failed = !state.fetch_active && !state.fetch_ready;
            fetch_unlock(&state);
            if (pending_ready) {
                commit_fetch(&state, idx);
            } else if (pending_failed && state.autoplay && state.autoplay_advancing) {
                size_t count = library_handler_track_count(state.play_lib);
                announce_fetch_failure(&state, state.play_lib, idx, 1);
                state.pending_valid = 0;
                if (count > 1) {
                    size_t nxt = take_next_index(&state, count, idx, 0);
                    request_play(&state, nxt);
                }
            }
        }

        if (state.is_playing && ++pos_tick % 30 == 0) {
            Uint32 p = playback_ms(&state);
            /* -1 means the backend cannot enumerate; only a hard 0 is "no sink". */
            int no_output = SDL_GetNumAudioDevices(0) == 0;
            state.no_output_ticks = no_output ? state.no_output_ticks + 1 : 0;
            if (p > state.last_position_ms) {
                state.position_stalls = 0;
            } else if (state.audio_device && !state.track_ended && state.duration_ms > 0) {
                state.position_stalls++;
            }
            state.last_position_ms = p;
            if (p != state.position_ms) { state.position_ms = p; write_status(&state); }
            /* Persist the resume point every ~5s of playback. */
            if ((Uint32)(SDL_GetTicks() - state.last_resume_write_ms) >= 5000) {
                state.last_resume_write_ms = SDL_GetTicks();
                write_resume(&state);
            }
            /* Self-heal audio that never reached the sink. Two failure shapes:
             * the position is not advancing (queue never consumed), or SDL sees
             * no output devices at all for a sustained window (the device
             * opened before the graph was ready and is now playing into nothing
             * while the position still ticks up). Rebuild from the buffered
             * samples so playback recovers without a manual restart. */
            if (state.audio_device && !state.track_ended && state.duration_ms > 0 &&
                (state.position_stalls >= 8 || state.no_output_ticks >= 3) &&
                (Uint32)(SDL_GetTicks() - state.last_audio_retry_ms) >= 6000) {
                state.last_audio_retry_ms = SDL_GetTicks();
                state.position_stalls = 0;
                state.last_position_ms = 0;
                p = state.position_ms;
                retry_audio_device(&state);
            } else if (!no_output && state.audio_retries > 0 &&
                       state.position_stalls == 0 && p > 0) {
                /* A real sink is present and the position is moving again: the
                 * last rebuild took, so stop carrying the attempt count. */
                state.audio_retries = 0;
            }
            if (!state.track_ended && state.duration_ms > 0 && p >= state.duration_ms - 250u) {
                state.position_ms = state.duration_ms;
                if (state.autoplay && state.repeat_one && state.decoder && state.audio_device) {
                    /* Replay in place: no fetch, just rewind the open decoder. */
                    if (state.pending_valid) { cancel_fetch(&state); state.pending_valid = 0; }
                    seek_ms(&state, 0);
                    snprintf(state.status, sizeof(state.status), "Repeating: %s",
                             state.title[0] ? state.title : "track");
                    write_status(&state);
                } else {
                    state.track_ended = 1;
                    write_status(&state);
                    if (state.pending_valid) {
                        size_t idx = state.pending_index;
                        fetch_lock(&state);
                        int ready = state.fetch_ready && state.fetch_index == idx;
                        int active = state.fetch_active && state.fetch_index == idx;
                        fetch_unlock(&state);
                        if (ready) commit_fetch(&state, idx);
                        else if (!active) start_fetch(&state, idx);
                    } else if (state.autoplay) {
                        size_t count = library_handler_track_count(state.play_lib);
                        size_t next = count ? take_next_index(&state, count, state.selected_track, 1)
                                            : state.selected_track;
                        state.pending_valid = 1;
                        state.pending_index = next;
                        state.autoplay_advancing = 1;
                        start_fetch(&state, next);
                    } else {
                        /* Autoplay off and nothing queued: the track is over.
                         * Publish is_playing=false -- the earlier write_status()
                         * still showed it playing. */
                        state.is_playing = 0;
                        SDL_PauseAudioDevice(state.audio_device, 1);
                        write_resume(&state);
                        write_status(&state);
                    }
                }
            }
        }
        if (headless) {
            /* Pump the event queue even with no window: SDL only refreshes its
             * audio device list (what SDL_GetNumAudioDevices reports) when the
             * loop is pumped and it can see AUDIODEVICEADDED/REMOVED. Without
             * this a backend that started before the sink existed never learns
             * the sink arrived. */
            SDL_PumpEvents();
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
            SDL_Delay(16);
        } else {
            nk_input_begin(ctx);
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) { running = 0; }
                else if (event.type == SDL_DROPFILE) {
                    if (event.drop.file) {
                        handle_dropped_file(state.library_path, &state, event.drop.file, &library, &ripper);
                        SDL_free(event.drop.file);
                    }
                    continue;
                }
                nk_sdl_handle_event(&event);
            }
            nk_input_end(ctx);
            {
                int ww = 1160, wh = 720;
                SDL_GetWindowSize(window, &ww, &wh);
                if (ww < 400) ww = 400;
                if (wh < 320) wh = 320;
                if (nk_begin(ctx, "Leecher", nk_rect(0, 0, (float)ww, (float)wh), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_TITLE)) {
                    nk_layout_row_dynamic(ctx, 32, 1); nk_label(ctx, "LEEcher  /  library to stream", NK_TEXT_LEFT);
                    nk_layout_row_dynamic(ctx, 24, 1); nk_label(ctx, state.status[0] ? state.status : "Choose a library track or add a source route.", NK_TEXT_LEFT);
                    float body = (float)wh - 32 - 24 - 34;
                    if (body < 80) body = 80;
                    nk_layout_row_dynamic(ctx, body, 2); draw_library(ctx, library, &state); draw_scraper(ctx, &state, &library, state.library_path, &ripper);
                }
            }
            nk_end(ctx);
            SDL_SetRenderDrawColor(renderer, palette.background.r, palette.background.g, palette.background.b, 255); SDL_RenderClear(renderer); nk_sdl_render(NK_ANTI_ALIASING_ON); SDL_RenderPresent(renderer);
        }
    }
    if (state.audio_device) state.position_ms = playback_ms(&state);
    write_resume(&state);   /* final resume point on a clean shutdown */
    cancel_fetch(&state);
    if (state.audio_device) SDL_CloseAudioDevice(state.audio_device);
    if (state.decoder) { decoder_close(state.decoder); state.decoder = NULL; }
    remove_cover_files(); /* GC committed covers on shutdown */
    pthread_mutex_destroy(&state.fetch_mutex);
    stop_ssh_agent(&state);
    if (!headless) { nk_sdl_shutdown(); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); }
    SDL_Quit(); assembler_destroy(assembler); library_handler_close(library); library_handler_close(state.play_lib);
    return 0;
}

/* Unit tests for the pure logic in backend/app.c.
 *
 * app.c is one translation unit with a main() and no public header, so rather
 * than refactor it we #include it with main() renamed and call the static
 * helpers directly. This tests the real code paths, not a copy. */
#define main leecher_app_main_unused
#include "../../backend/app.c"
#undef main

#include <assert.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

#define CHECK_EQ_SIZE(a, b) do { \
    checks++; \
    size_t _a = (a), _b = (b); \
    if (_a != _b) { fprintf(stderr, "  FAIL %s:%d: %zu != %zu\n", __FILE__, __LINE__, _a, _b); failures++; } \
} while (0)

#define CHECK_STR(a, b) do { \
    checks++; \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); failures++; } \
} while (0)

/* ---------------------------------------------------------------- json_escape */
static void test_json_escape(void) {
    char *s;
    s = json_escape("plain");            CHECK_STR(s, "plain");                 free(s);
    s = json_escape("a\"b");             CHECK_STR(s, "a\\\"b");                free(s);
    s = json_escape("a\\b");             CHECK_STR(s, "a\\\\b");                free(s);
    s = json_escape("line1\nline2");     CHECK_STR(s, "line1\\nline2");         free(s);
    s = json_escape("tab\there");        CHECK_STR(s, "tab\\there");            free(s);
    s = json_escape("\x01");             CHECK_STR(s, "\\u0001");               free(s);
    s = json_escape("\x1f");             CHECK_STR(s, "\\u001f");               free(s);
    s = json_escape("\r");               CHECK_STR(s, "\\r");                   free(s);
    s = json_escape("");                 CHECK_STR(s, "");                      free(s);
    s = json_escape(NULL);               CHECK_STR(s, "");                      free(s);
    /* bytes >= 0x20 (incl. UTF-8 continuation) pass through verbatim */
    s = json_escape("caf\xc3\xa9");      CHECK_STR(s, "caf\xc3\xa9");           free(s);
    /* long string, 2x expansion */
    {
        char big[600], want[3600];
        size_t i;
        for (i = 0; i < sizeof(big) - 1; i++) big[i] = '"';
        big[sizeof(big) - 1] = '\0';
        for (i = 0; i < sizeof(big) - 1; i++) { want[i * 2] = '\\'; want[i * 2 + 1] = '"'; }
        want[(sizeof(big) - 1) * 2] = '\0';
        s = json_escape(big); CHECK_STR(s, want); free(s);
    }
    /* worst case for the exact-sized allocation: every byte -> \uXXXX (6x) */
    {
        char big[512];
        size_t i;
        for (i = 0; i < sizeof(big) - 1; i++) big[i] = '\x1f';
        big[sizeof(big) - 1] = '\0';
        s = json_escape(big);
        CHECK_EQ_SIZE(strlen(s), (sizeof(big) - 1) * 6);
        for (i = 0; i < sizeof(big) - 1; i++) CHECK(memcmp(s + i * 6, "\\u001f", 6) == 0);
        free(s);
    }
}

/* ------------------------------------------------------------- ssh_name_valid */
static void test_ssh_name_valid(void) {
    CHECK(ssh_name_valid("user", 0) == 1);
    CHECK(ssh_name_valid("user.name-1_2", 0) == 1);
    CHECK(ssh_name_valid("192.168.1.10", 0) == 1);
    CHECK(ssh_name_valid("", 0) == 0);
    CHECK(ssh_name_valid("user name", 0) == 0);
    CHECK(ssh_name_valid("user;rm -rf", 0) == 0);
    CHECK(ssh_name_valid("$(whoami)", 0) == 0);
    CHECK(ssh_name_valid("a/../b", 0) == 0);
    CHECK(ssh_name_valid("host:22", 0) == 0);       /* colon rejected unless allowed */
    CHECK(ssh_name_valid("host:22", 1) == 1);
    CHECK(ssh_name_valid("a`id`b", 0) == 0);
    CHECK(ssh_name_valid("a\nb", 0) == 0);
    CHECK(ssh_name_valid("a b", 1) == 0);           /* space still rejected with colon allowed */
    CHECK(ssh_name_valid("-flag", 0) == 1);         /* leading dash is allowed by the charset */
    CHECK(ssh_name_valid("user@host", 0) == 0);     /* '@' not in the safe set */
    CHECK(ssh_name_valid(NULL, 0) == 0);
}

/* ---------------------------------------------------------- shell_quote_words */
static void test_shell_quote_words(void) {
    char *q;
    q = shell_quote_words("simple");        CHECK_STR(q, "'simple'");                free(q);
    q = shell_quote_words("a b");           CHECK_STR(q, "'a b'");                   free(q);
    q = shell_quote_words("it's");          CHECK_STR(q, "'it'\\''s'");              free(q);
    q = shell_quote_words("");              CHECK_STR(q, "''");                      free(q);
}

/* ---------------------------------------------------------- control_decode */
static void test_control_decode(void) {
    char buf[64];
    control_decode(buf, sizeof(buf), "no%20change%2Fhere");
    CHECK_STR(buf, "no change/here");
    control_decode(buf, sizeof(buf), "plain");
    CHECK_STR(buf, "plain");
    control_decode(buf, sizeof(buf), "100%25");
    CHECK_STR(buf, "100%");
    /* malformed percent escapes are left as-is, not dropped or over-read */
    control_decode(buf, sizeof(buf), "a%");
    CHECK_STR(buf, "a%");
    control_decode(buf, sizeof(buf), "a%zz");
    CHECK_STR(buf, "a%zz");
    control_decode(buf, sizeof(buf), "a%4");
    CHECK_STR(buf, "a%4");
    /* output is bounded by the destination size */
    {
        char small[5];
        control_decode(small, sizeof(small), "abcdefghij");
        CHECK(strlen(small) < sizeof(small));
    }
}

/* -------------------------------------------------------- next_autoplay_index */
static void test_next_autoplay_index(void) {
    AppState s = {0};

    /* linear */
    CHECK_EQ_SIZE(next_autoplay_index(&s, 5, 0, 1), 1);
    CHECK_EQ_SIZE(next_autoplay_index(&s, 5, 4, 1), 0);        /* wraps */
    CHECK_EQ_SIZE(next_autoplay_index(&s, 1, 0, 1), 0);        /* single track */

    /* repeat-one holds only when allowed */
    s.repeat_one = 1;
    CHECK_EQ_SIZE(next_autoplay_index(&s, 5, 2, 1), 2);
    CHECK_EQ_SIZE(next_autoplay_index(&s, 5, 2, 0), 3);        /* skip past ignores repeat */
    s.repeat_one = 0;

    /* shuffle never returns the current index and stays in range */
    s.shuffle = 1;
    srand(12345);
    for (int i = 0; i < 200; i++) {
        size_t n = next_autoplay_index(&s, 6, 3, 1);
        CHECK(n < 6);
        CHECK(n != 3);
    }
    CHECK_EQ_SIZE(next_autoplay_index(&s, 1, 0, 1), 0);        /* can't avoid self with 1 */
}

/* ------------------------------------------------- autoplay dead-playlist guard */
static void test_autoplay_note_failure(void) {
    AppState s = {0};
    /* 3-track playlist, every fetch failing: give up on the 3rd (a full lap). */
    CHECK(autoplay_note_failure(&s, 3) == 0);
    CHECK(autoplay_note_failure(&s, 3) == 0);
    CHECK(autoplay_note_failure(&s, 3) == 1);
    /* one success in between resets the streak */
    s.autoplay_fail_streak = 0;
    CHECK(autoplay_note_failure(&s, 3) == 0);
    CHECK(autoplay_note_failure(&s, 3) == 0);
    s.autoplay_fail_streak = 0;                 /* commit_fetch would do this */
    CHECK(autoplay_note_failure(&s, 3) == 0);
    /* count 0 never trips the guard */
    s.autoplay_fail_streak = 99;
    CHECK(autoplay_note_failure(&s, 0) == 0);
}

/* ------------------------------------------------------------- the play queue */
static void test_play_queue(void) {
    AppState s = {0};

    play_queue_push(&s, 4);
    play_queue_push(&s, 2);
    play_queue_push(&s, 7);
    CHECK(s.play_queue_len == 3);
    CHECK(s.play_queue[0] == 4 && s.play_queue[1] == 2 && s.play_queue[2] == 7);

    play_queue_remove_at(&s, 1);                 /* drop the 2 */
    CHECK(s.play_queue_len == 2);
    CHECK(s.play_queue[0] == 4 && s.play_queue[1] == 7);

    play_queue_remove_at(&s, 5);                 /* out of range: no-op */
    CHECK(s.play_queue_len == 2);

    /* cap is respected */
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < PLAY_QUEUE_CAP + 10; i++) play_queue_push(&s, (size_t)i);
    CHECK(s.play_queue_len == PLAY_QUEUE_CAP);
}

/* --------------------------------------------------------------- take_next_index */
static void test_take_next_index(void) {
    AppState s = {0};

    /* queue drains in order and beats linear/shuffle */
    play_queue_push(&s, 3);
    play_queue_push(&s, 1);
    CHECK_EQ_SIZE(take_next_index(&s, 5, 0, 1), 3);
    CHECK_EQ_SIZE(take_next_index(&s, 5, 0, 1), 1);
    CHECK(s.play_queue_len == 0);
    CHECK_EQ_SIZE(take_next_index(&s, 5, 0, 1), 1);   /* empty -> linear next */

    /* stale entries (>= count) are skipped, not returned */
    memset(&s, 0, sizeof(s));
    play_queue_push(&s, 99);
    play_queue_push(&s, 2);
    CHECK_EQ_SIZE(take_next_index(&s, 5, 0, 1), 2);
    CHECK(s.play_queue_len == 0);

    /* an all-stale queue falls through to the autoplay rule */
    memset(&s, 0, sizeof(s));
    play_queue_push(&s, 50);
    CHECK_EQ_SIZE(take_next_index(&s, 5, 4, 1), 0);   /* linear wrap */
    CHECK(s.play_queue_len == 0);
}

/* ---------------------------------------------------------- next_encoded_token */
static void test_next_encoded_token(void) {
    const char *p = "alice bob.example%2Ecom /music/track%20one.flac";
    char tok[128];

    next_encoded_token(&p, tok, sizeof(tok));
    CHECK_STR(tok, "alice");
    next_encoded_token(&p, tok, sizeof(tok));
    CHECK_STR(tok, "bob.example.com");
    next_encoded_token(&p, tok, sizeof(tok));
    CHECK_STR(tok, "/music/track one.flac");
    next_encoded_token(&p, tok, sizeof(tok));
    CHECK_STR(tok, "");                               /* exhausted */
}

/* ---------------------------------------------------------------- assembler */
static void test_assembler(void) {
    Assembler *a;
    AssemblerPiece p = {0};
    AssemblerConfig cfg = {0};
    unsigned char buf[4096];
    size_t i;

    a = assembler_create(NULL);
    CHECK(a != NULL);
    CHECK(assembler_pop(a, &p) == 0);                 /* empty */
    CHECK(assembler_push(a, (const unsigned char *)"abc", 3) == 1);
    CHECK(assembler_push(a, (const unsigned char *)"de", 2) == 1);
    CHECK(assembler_queued_bytes(a) == 5);
    CHECK(assembler_queued_pieces(a) == 2);
    CHECK(assembler_pop(a, &p) == 1);
    CHECK(p.size == 3 && memcmp(p.data, "abc", 3) == 0);
    assembler_piece_destroy(&p);
    CHECK(assembler_queued_bytes(a) == 2);
    CHECK(assembler_pop(a, &p) == 1 && p.size == 2);
    assembler_piece_destroy(&p);
    CHECK(assembler_pop(a, &p) == 0);
    assembler_destroy(a);

    /* backpressure: max_queued_bytes is a hard ceiling, push returns 0 */
    cfg.max_queued_bytes = 5000;
    a = assembler_create(&cfg);
    CHECK(a != NULL);
    for (i = 0; i < sizeof(buf); i++) buf[i] = (unsigned char)i;
    CHECK(assembler_push(a, buf, 4096) == 1);
    CHECK(assembler_push(a, buf, 4096) == 0);         /* would exceed 5000 */
    CHECK(assembler_queued_bytes(a) == 4096);
    CHECK(assembler_pop(a, &p) == 1);
    CHECK(memcmp(p.data, buf, p.size) == 0);          /* bytes intact */
    assembler_piece_destroy(&p);
    assembler_destroy(a);

    assembler_destroy(NULL);                          /* tolerates NULL */
}

/* ------------------------------------------------------------------ decoder */
/* Minimal PCM16 mono WAV in memory. */
static unsigned char *make_wav(int rate, int frames, size_t *out_len) {
    size_t data = (size_t)frames * 2;
    size_t total = 44 + data;
    unsigned char *w = calloc(1, total);
    unsigned le32[1];
    (void)le32;
    memcpy(w, "RIFF", 4);
    w[4] = (unsigned char)((36 + data));  w[5] = (unsigned char)((36 + data) >> 8);
    w[6] = (unsigned char)((36 + data) >> 16); w[7] = (unsigned char)((36 + data) >> 24);
    memcpy(w + 8, "WAVEfmt ", 8);
    w[16] = 16;                                   /* fmt chunk size */
    w[20] = 1;                                    /* PCM */
    w[22] = 1;                                    /* channels */
    w[24] = (unsigned char)rate; w[25] = (unsigned char)(rate >> 8);
    w[26] = (unsigned char)(rate >> 16); w[27] = (unsigned char)(rate >> 24);
    { unsigned br = (unsigned)rate * 2;
      w[28] = (unsigned char)br; w[29] = (unsigned char)(br >> 8);
      w[30] = (unsigned char)(br >> 16); w[31] = (unsigned char)(br >> 24); }
    w[32] = 2;                                    /* block align */
    w[34] = 16;                                   /* bits per sample */
    memcpy(w + 36, "data", 4);
    w[40] = (unsigned char)data; w[41] = (unsigned char)(data >> 8);
    w[42] = (unsigned char)(data >> 16); w[43] = (unsigned char)(data >> 24);
    *out_len = total;
    return w;
}

static void *sb_feeder(void *arg) {
    StreamBuffer *sb = arg;
    int i;
    for (i = 0; i < 5; i++) stream_buffer_append(sb, (const unsigned char *)"0123456789", 10);
    stream_buffer_set_complete(sb);
    return NULL;
}

static void test_stream_buffer(void) {
    StreamBuffer *sb = stream_buffer_create(0);
    unsigned char buf[64];
    pthread_t th;

    CHECK(sb != NULL);
    CHECK(stream_buffer_append(sb, (const unsigned char *)"abc", 3) == 1);
    CHECK(stream_buffer_size(sb) == 3);
    CHECK(!stream_buffer_is_complete(sb));
    CHECK(stream_buffer_read_at(sb, buf, 3, 0) == 3);
    CHECK(memcmp(buf, "abc", 3) == 0);
    stream_buffer_set_complete(sb);
    CHECK(stream_buffer_is_complete(sb));
    CHECK(stream_buffer_read_at(sb, buf, 10, 1) == 2);   /* only "bc" left at the real end */
    CHECK(stream_buffer_read_at(sb, buf, 10, 3) == 0);   /* at EOF */
    stream_buffer_destroy(sb);

    /* the cap is a hard ceiling: an append past it fails the buffer */
    sb = stream_buffer_create(8);
    CHECK(stream_buffer_append(sb, (const unsigned char *)"12345", 5) == 1);
    CHECK(stream_buffer_append(sb, (const unsigned char *)"12345", 5) == 0);
    CHECK(stream_buffer_is_failed(sb));
    stream_buffer_destroy(sb);

    /* a reader blocked at the frontier is released when another thread fills it */
    sb = stream_buffer_create(0);
    CHECK(pthread_create(&th, NULL, sb_feeder, sb) == 0);
    CHECK(stream_buffer_read_at(sb, buf, 50, 0) == 50);
    pthread_join(th, NULL);
    CHECK(stream_buffer_wait_complete(sb) == 50);
    stream_buffer_destroy(sb);
}

static void test_decoder(void) {
    size_t wlen;
    unsigned char *wav = make_wav(8000, 8000, &wlen);   /* 1 second */
    StreamBuffer *sb = stream_buffer_create(0);
    DecoderSource *d = NULL;
    char err[256] = {0};
    short out[8192];
    long long n, pos, left;

    CHECK(stream_buffer_append(sb, wav, wlen) == 1);
    stream_buffer_set_complete(sb);
    CHECK(decoder_open(sb, &d, err, sizeof(err)) == 1);
    CHECK(d != NULL);
    CHECK(decoder_rate(d) == 8000);
    CHECK(decoder_channels(d) == 1);
    CHECK(decoder_total_frames(d) == 8000);

    n = decoder_read_frames(d, out, 2048);
    CHECK(n == 2048);
    pos = decoder_seek(d, 4000);
    CHECK(pos == 4000);
    left = 0;
    while ((n = decoder_read_frames(d, out, 4096)) > 0) left += n;
    CHECK(left == 4000);                                /* only 4000 frames after the seek */
    n = decoder_read_frames(d, out, 100);
    CHECK(n == 0);                                      /* EOF */
    decoder_close(d);                                   /* frees the StreamBuffer */
    free(wav);

    /* empty stream -> "no audio" (0), not a crash; ownership stays with us */
    sb = stream_buffer_create(0);
    stream_buffer_set_complete(sb);
    d = NULL;
    CHECK(decoder_open(sb, &d, err, sizeof(err)) == 0);
    stream_buffer_destroy(sb);

    /* opened while the transfer is still "in progress" (bytes all present but
     * the buffer not yet marked complete): once it completes, a seek still
     * works -- the decoder re-opens the handle internally to repair it. */
    wav = make_wav(8000, 8000, &wlen);
    sb = stream_buffer_create(0);
    CHECK(stream_buffer_append(sb, wav, wlen) == 1);
    d = NULL;
    CHECK(decoder_open(sb, &d, err, sizeof(err)) == 1);   /* opened_partial: not complete yet */
    CHECK(decoder_total_frames(d) == 8000);
    stream_buffer_set_complete(sb);
    CHECK(decoder_seek(d, 6000) == 6000);
    left = 0;
    while ((n = decoder_read_frames(d, out, 4096)) > 0) left += n;
    CHECK(left == 2000);
    decoder_close(d);
    free(wav);
}

/* ------------------------------------------------------------- resume state */
static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

/* Exercise the write side: write_resume() then read_resume() must round-trip,
 * and the guards (no track / zero duration) must skip the write. */
static void test_write_resume(void) {
    char path[] = "/tmp/leecher-wres.XXXXXX";
    int fd = mkstemp(path);
    AppState s = {0};
    size_t ti = 0;
    Uint32 pos = 0;
    int playing = -1;
    char pl[96] = "";
    assert(fd >= 0);
    close(fd);
    unlink(path);
    snprintf(resume_file, sizeof(resume_file), "%s", path);

    s.selected_track = (size_t)-1;
    s.duration_ms = 0;
    write_resume(&s);
    CHECK(fopen(path, "r") == NULL || read_resume(pl, sizeof(pl), &ti, &pos, &playing) == 0);  /* nothing written */

    snprintf(s.playing_playlist, sizeof(s.playing_playlist), "%s", "road trip");
    s.selected_track = 5;
    s.position_ms = 87654;
    s.duration_ms = 200000;
    s.is_playing = 0;
    write_resume(&s);
    CHECK(read_resume(pl, sizeof(pl), &ti, &pos, &playing) == 1);
    CHECK_EQ_SIZE(ti, 5);
    CHECK(pos == 87654);
    CHECK(playing == 0);
    CHECK_STR(pl, "road trip");

    s.is_playing = 1;
    write_resume(&s);
    CHECK(read_resume(pl, sizeof(pl), &ti, &pos, &playing) == 1 && playing == 1);

    unlink(path);
    resume_file[0] = '\0';
}

static void test_resume_roundtrip(void) {
    char path[] = "/tmp/leecher-resume-test.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    snprintf(resume_file, sizeof(resume_file), "%s", path);

    size_t ti = 999;
    Uint32 pos = 999;
    int playing = -1;
    char pl[96] = "sentinel";

    /* what write_resume() actually emits */
    write_file(path, "{\"playlist\":\"home\",\"track_index\":3,\"position_ms\":125000,\"is_playing\":true}\n");
    CHECK(read_resume(pl, sizeof(pl), &ti, &pos, &playing) == 1);
    CHECK_EQ_SIZE(ti, 3);
    CHECK(pos == 125000);
    CHECK(playing == 1);
    CHECK_STR(pl, "home");

    write_file(path, "{\"playlist\":\"chill\",\"track_index\":0,\"position_ms\":0,\"is_playing\":false}\n");
    CHECK(read_resume(pl, sizeof(pl), &ti, &pos, &playing) == 1);
    CHECK_EQ_SIZE(ti, 0);
    CHECK(pos == 0);
    CHECK(playing == 0);          /* the off-by-one that shipped once */
    CHECK_STR(pl, "chill");

    /* a legacy file with no playlist field still parses; playlist comes back empty */
    write_file(path, "{\"track_index\":1,\"position_ms\":5,\"is_playing\":true}\n");
    CHECK(read_resume(pl, sizeof(pl), &ti, &pos, &playing) == 1 && pl[0] == '\0');

    write_file(path, "garbage not json");
    CHECK(read_resume(pl, sizeof(pl), &ti, &pos, &playing) == 0);

    write_file(path, "{\"position_ms\":10}");   /* no track_index */
    CHECK(read_resume(pl, sizeof(pl), &ti, &pos, &playing) == 0);

    unlink(path);
    resume_file[0] = '\0';
    CHECK(read_resume(pl, sizeof(pl), &ti, &pos, &playing) == 0);   /* no path -> no resume */
}

/* ------------------------------------------------------------- playlists */

static void test_valid_playlist_name(void) {
    CHECK(valid_playlist_name("home"));
    CHECK(valid_playlist_name("Road Trip 2"));
    CHECK(valid_playlist_name("a-b_c"));
    CHECK(!valid_playlist_name(""));
    CHECK(!valid_playlist_name(" leading"));
    CHECK(!valid_playlist_name("trailing "));
    CHECK(!valid_playlist_name("bad/name"));
    CHECK(!valid_playlist_name("dots.here"));
    CHECK(!valid_playlist_name(".."));
    CHECK(!valid_playlist_name("tab\there"));
    {
        char long_name[80];
        memset(long_name, 'x', sizeof(long_name));
        long_name[64] = '\0';
        CHECK(valid_playlist_name(long_name));   /* exactly 64 */
        long_name[65] = '\0';
        long_name[64] = 'x';
        CHECK(!valid_playlist_name(long_name));   /* 65 */
    }
    /* staging list names never pass -- users can't hand-create one */
    CHECK(!valid_playlist_name("INCOMING >> home <<"));
}

static void test_incoming_names(void) {
    char name[LIBRARY_NAME_MAX], target[LIBRARY_NAME_MAX];

    incoming_name_for("home", name, sizeof(name));
    CHECK_STR(name, "INCOMING >> home <<");
    incoming_name_for("Road Trip", name, sizeof(name));
    CHECK_STR(name, "INCOMING >> Road Trip <<");

    CHECK(incoming_target_of("INCOMING >> home <<", target, sizeof(target)) == 1);
    CHECK_STR(target, "home");
    CHECK(incoming_target_of("INCOMING >> Road Trip <<", target, sizeof(target)) == 1);
    CHECK_STR(target, "Road Trip");
    CHECK(is_incoming_name("INCOMING >> x <<"));

    CHECK(incoming_target_of("home", NULL, 0) == 0);
    CHECK(incoming_target_of("INCOMING >> ", NULL, 0) == 0);       /* no suffix */
    CHECK(incoming_target_of("INCOMING >>  <<", NULL, 0) == 0);    /* empty target */
    CHECK(incoming_target_of("prefix INCOMING >> x <<", NULL, 0) == 0);
    CHECK(!is_incoming_name("*"));
}

static void test_playlist_rank_order(void) {
    /* qsort with playlist_cmp: home, regulars (alpha), staging, "*" */
    char list[6][LIBRARY_NAME_MAX];
    snprintf(list[0], sizeof(list[0]), "%s", "*");
    snprintf(list[1], sizeof(list[1]), "%s", "INCOMING >> home <<");
    snprintf(list[2], sizeof(list[2]), "%s", "zeppelin");
    snprintf(list[3], sizeof(list[3]), "%s", "home");
    snprintf(list[4], sizeof(list[4]), "%s", "Ambient");
    snprintf(list[5], sizeof(list[5]), "%s", "INCOMING >> Ambient <<");
    qsort(list, 6, sizeof(list[0]), playlist_cmp);
    CHECK_STR(list[0], "home");
    CHECK_STR(list[1], "Ambient");
    CHECK_STR(list[2], "zeppelin");
    CHECK_STR(list[3], "INCOMING >> Ambient <<");
    CHECK_STR(list[4], "INCOMING >> home <<");
    CHECK_STR(list[5], "*");
}

static void test_scan_and_known_playlists(void) {
    char dir[] = "/tmp/leecher-pl.XXXXXX";
    AppState s = {0};
    assert(mkdtemp(dir));
    snprintf(s.library_dir, sizeof(s.library_dir), "%s", dir);

    { char p[600]; snprintf(p, sizeof(p), "%s/home.json", dir); write_file(p, EMPTY_LIBRARY_JSON); }
    { char p[600]; snprintf(p, sizeof(p), "%s/chill.json", dir); write_file(p, EMPTY_LIBRARY_JSON); }
    { char p[600]; snprintf(p, sizeof(p), "%s/Ambient.json", dir); write_file(p, EMPTY_LIBRARY_JSON); }
    { char p[600]; snprintf(p, sizeof(p), "%s/%s.json", dir, STAR_PLAYLIST); write_file(p, EMPTY_LIBRARY_JSON); }
    { char p[600]; snprintf(p, sizeof(p), "%s/.resume.json", dir); write_file(p, "{}"); }
    { char p[600]; snprintf(p, sizeof(p), "%s/notes.txt", dir); write_file(p, "x"); }

    scan_playlists(&s);
    CHECK_EQ_SIZE((size_t)s.playlist_count, 4);   /* .resume.json and notes.txt skipped */
    CHECK_STR(s.playlists[0], "home");             /* home always sorts first */
    CHECK_STR(s.playlists[1], "Ambient");          /* then case-insensitive alpha */
    CHECK_STR(s.playlists[2], "chill");
    CHECK_STR(s.playlists[3], STAR_PLAYLIST);      /* the auto-collect list sorts last */
    CHECK(playlist_known(&s, "chill"));
    CHECK(playlist_known(&s, STAR_PLAYLIST));
    CHECK(!playlist_known(&s, "missing"));

    { char p[600]; snprintf(p, sizeof(p), "%s/home.json", dir); unlink(p); }
    { char p[600]; snprintf(p, sizeof(p), "%s/chill.json", dir); unlink(p); }
    { char p[600]; snprintf(p, sizeof(p), "%s/Ambient.json", dir); unlink(p); }
    { char p[600]; snprintf(p, sizeof(p), "%s/%s.json", dir, STAR_PLAYLIST); unlink(p); }
    { char p[600]; snprintf(p, sizeof(p), "%s/.resume.json", dir); unlink(p); }
    { char p[600]; snprintf(p, sizeof(p), "%s/notes.txt", dir); unlink(p); }
    rmdir(dir);
}

static void test_source_key(void) {
    LibrarySource a = { .kind = LIBRARY_SOURCE_LOCAL, .path = (char *)"/m/x.flac" };
    LibrarySource b = { .kind = LIBRARY_SOURCE_LOCAL, .path = (char *)"/m/x.flac" };
    LibrarySource c = { .kind = LIBRARY_SOURCE_LOCAL, .path = (char *)"/m/y.flac" };
    LibrarySource d = { .kind = LIBRARY_SOURCE_HTTPS, .url = (char *)"https://h/x" };
    char ka[128], kb[128], kc[128], kd[128];
    source_key(&a, ka, sizeof(ka));
    source_key(&b, kb, sizeof(kb));
    source_key(&c, kc, sizeof(kc));
    source_key(&d, kd, sizeof(kd));
    CHECK_STR(ka, kb);              /* identical source -> identical key */
    CHECK(strcmp(ka, kc) != 0);     /* different path */
    CHECK(strcmp(ka, kd) != 0);     /* different kind */
}

/* Building "*" from two playlists that share a track keeps one copy of the
 * shared source, and unions the rest. */
static void test_rebuild_star_playlist(void) {
    char dir[] = "/tmp/leecher-star.XXXXXX";
    AppState s = {0};
    LibraryHandler *star;
    assert(mkdtemp(dir));
    snprintf(s.library_dir, sizeof(s.library_dir), "%s", dir);

    { char p[600]; snprintf(p, sizeof(p), "%s/home.json", dir);
      write_file(p, "{\"version\":1,\"tracks\":["
        "{\"title\":\"Shared\",\"artist\":\"A\",\"album\":\"X\",\"sources\":[{\"kind\":\"local\",\"PATH\":\"/m/s.flac\"}]},"
        "{\"title\":\"Only Home\",\"artist\":\"A\",\"album\":\"X\",\"sources\":[{\"kind\":\"local\",\"PATH\":\"/m/h.flac\"}]}]}"); }
    { char p[600]; snprintf(p, sizeof(p), "%s/mix.json", dir);
      write_file(p, "{\"version\":1,\"tracks\":["
        "{\"title\":\"Shared\",\"artist\":\"A\",\"album\":\"X\",\"sources\":[{\"kind\":\"local\",\"PATH\":\"/m/s.flac\"}]},"
        "{\"title\":\"Only Mix\",\"artist\":\"A\",\"album\":\"X\",\"sources\":[{\"kind\":\"https\",\"URL\":\"https://h/m\"}]}]}"); }
    { char p[600]; snprintf(p, sizeof(p), "%s/%s.json", dir, STAR_PLAYLIST); write_file(p, EMPTY_LIBRARY_JSON); }
    scan_playlists(&s);

    rebuild_star_playlist(&s);

    { char p[600]; snprintf(p, sizeof(p), "%s/%s.json", dir, STAR_PLAYLIST);
      star = library_handler_open(p, NULL, 0); }
    CHECK(star != NULL);
    if (star) {
        size_t n = library_handler_track_count(star), i, total_sources = 0;
        CHECK_EQ_SIZE(n, 3);   /* Shared, Only Home, Only Mix */
        for (i = 0; i < n; i++) {
            LibraryTrack t = {0};
            if (library_handler_track_at(star, i, &t, NULL, 0) == 1) total_sources += t.source_count;
            library_handler_track_destroy(&t);
        }
        CHECK_EQ_SIZE(total_sources, 3);   /* the shared source is not doubled */
        library_handler_close(star);
    }

    { char p[600]; snprintf(p, sizeof(p), "%s/home.json", dir); unlink(p); }
    { char p[600]; snprintf(p, sizeof(p), "%s/mix.json", dir); unlink(p); }
    { char p[600]; snprintf(p, sizeof(p), "%s/%s.json", dir, STAR_PLAYLIST); unlink(p); }
    rmdir(dir);
}

static void test_resolve_library_dir_migrates(void) {
    char base[] = "/tmp/leecher-mig.XXXXXX";
    AppState s = {0};
    char legacy[PATH_MAX];
    char arg[PATH_MAX], home[PATH_MAX];
    FILE *f;
    assert(mkdtemp(base));
    snprintf(arg, sizeof(arg), "%s/library.json", base);
    write_file(arg, "{\"version\":1,\"tracks\":[{\"title\":\"T\",\"artist\":\"A\",\"album\":\"B\",\"sources\":[]}]}\n");

    resolve_library_dir(arg, &s, legacy, sizeof(legacy));

    snprintf(home, sizeof(home), "%s/library/home.json", base);
    f = fopen(home, "r");
    CHECK(f != NULL);                       /* the legacy file was migrated in */
    if (f) fclose(f);
    CHECK_EQ_SIZE((size_t)s.playlist_count, 2);   /* home + the seeded "*" */
    CHECK_STR(s.playlists[0], "home");
    CHECK_STR(s.playlists[1], STAR_PLAYLIST);
    CHECK(strstr(s.library_dir, "/library") != NULL);

    unlink(arg); unlink(home);
    { char p[PATH_MAX]; snprintf(p, sizeof(p), "%s/library/%s.json", base, STAR_PLAYLIST); unlink(p); }
    { char d[PATH_MAX]; snprintf(d, sizeof(d), "%s/library", base); rmdir(d); }
    rmdir(base);
}

/* -------------------------------------------------------- library_handler */
static const char *LIB_FIXTURE =
    "{\"version\":1,\"tracks\":["
    "{\"title\":\"Alpha\",\"artist\":\"A\",\"album\":\"One\","
      "\"sources\":[{\"kind\":\"local\",\"PATH\":\"/m/a.flac\",\"USERNAME\":null,\"URL\":null,\"IP\":null}]},"
    "{\"title\":\"Bravo\",\"artist\":\"B\",\"album\":\"Two\","
      "\"sources\":[{\"kind\":\"local\",\"PATH\":\"/m/b.flac\",\"USERNAME\":null,\"URL\":null,\"IP\":null}]}"
    "]}";

static size_t lib_count(const char *path) {
    LibraryHandler *h = library_handler_open(path, NULL, 0);
    size_t n = h ? library_handler_track_count(h) : (size_t)-1;
    library_handler_close(h);
    return n;
}

static void lib_title(const char *path, size_t idx, char *out, size_t out_sz) {
    LibraryHandler *h = library_handler_open(path, NULL, 0);
    LibraryTrack t = {0};
    char err[128];
    out[0] = '\0';
    if (h && library_handler_track_at(h, idx, &t, err, sizeof(err)) == 1 && t.title)
        snprintf(out, out_sz, "%s", t.title);
    library_handler_track_destroy(&t);
    library_handler_close(h);
}

static void test_library_handler(void) {
    char path[] = "/tmp/leecher-lib.XXXXXX";
    int fd = mkstemp(path);
    char err[256];
    char title[128];
    LibraryHandler *h;
    LibraryTrack t = {0};
    assert(fd >= 0);
    write_file(path, LIB_FIXTURE);
    close(fd);

    CHECK(lib_count(path) == 2);
    lib_title(path, 0, title, sizeof(title));
    CHECK_STR(title, "Alpha");
    lib_title(path, 1, title, sizeof(title));
    CHECK_STR(title, "Bravo");

    /* track_at out of range -> 0, not a crash */
    h = library_handler_open(path, err, sizeof(err));
    CHECK(h != NULL);
    CHECK(library_handler_track_at(h, 99, &t, err, sizeof(err)) == 0);

    /* resolve by title */
    {
        LibrarySongQuery q = {0};
        q.title = "Bravo";
        CHECK(library_handler_resolve(h, &q, &t, err, sizeof(err)) == 1);
        library_handler_track_destroy(&t);
        q.title = "Nonexistent Song";
        CHECK(library_handler_resolve(h, &q, &t, err, sizeof(err)) == 0);
        library_handler_track_destroy(&t);
    }
    library_handler_close(h);

    /* add a new source for a new track */
    {
        LibrarySongQuery q = {0};
        LibrarySource src = {0};
        char p[] = "/m/charlie.flac";
        q.title = "Charlie"; q.artist = "C"; q.album = "Three";
        src.kind = LIBRARY_SOURCE_LOCAL; src.path = p;
        CHECK(library_handler_add_source(path, &q, &src, err, sizeof(err)) == 1);
        /* missing album -> rejected */
        q.album = NULL;
        CHECK(library_handler_add_source(path, &q, &src, err, sizeof(err)) == -1);
        q.album = "Three";
        CHECK(lib_count(path) == 3);
        lib_title(path, 2, title, sizeof(title));
        CHECK_STR(title, "Charlie");
    }

    /* rename track 0, leave artist/album alone */
    CHECK(library_handler_update_track(path, 0, "Alpha Prime", NULL, NULL, err, sizeof(err)) == 1);
    lib_title(path, 0, title, sizeof(title));
    CHECK_STR(title, "Alpha Prime");
    h = library_handler_open(path, NULL, 0);
    CHECK(library_handler_track_at(h, 0, &t, err, sizeof(err)) == 1);
    CHECK(t.artist && strcmp(t.artist, "A") == 0);       /* untouched */
    library_handler_track_destroy(&t);
    library_handler_close(h);

    /* remove the middle track */
    CHECK(library_handler_remove_track(path, 1, err, sizeof(err)) == 1);
    CHECK(lib_count(path) == 2);
    lib_title(path, 0, title, sizeof(title)); CHECK_STR(title, "Alpha Prime");
    lib_title(path, 1, title, sizeof(title)); CHECK_STR(title, "Charlie");
    /* out of range: the header says 0 but the code returns -1 -- pin the
     * actual behaviour so a doc/impl reconciliation is a deliberate change */
    CHECK(library_handler_remove_track(path, 9, err, sizeof(err)) == -1);
    CHECK(lib_count(path) == 2);   /* and the library is untouched */

    /* the file stays valid JSON after every mutation */
    CHECK(lib_count(path) == 2);

    /* a malformed library is rejected, not half-parsed */
    write_file(path, "{ this is not json");
    CHECK(library_handler_open(path, err, sizeof(err)) == NULL);

    unlink(path);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "json_escape",         test_json_escape },
        { "ssh_name_valid",      test_ssh_name_valid },
        { "shell_quote_words",   test_shell_quote_words },
        { "control_decode",      test_control_decode },
        { "next_autoplay_index", test_next_autoplay_index },
        { "autoplay_note_failure", test_autoplay_note_failure },
        { "play_queue",          test_play_queue },
        { "take_next_index",     test_take_next_index },
        { "next_encoded_token",  test_next_encoded_token },
        { "assembler",           test_assembler },
        { "stream_buffer",       test_stream_buffer },
        { "decoder",             test_decoder },
        { "library_handler",     test_library_handler },
        { "write_resume",        test_write_resume },
        { "resume_roundtrip",    test_resume_roundtrip },
        { "valid_playlist_name", test_valid_playlist_name },
        { "scan_known_playlists", test_scan_and_known_playlists },
        { "source_key",          test_source_key },
        { "incoming_names",      test_incoming_names },
        { "playlist_rank_order", test_playlist_rank_order },
        { "rebuild_star_playlist", test_rebuild_star_playlist },
        { "resolve_library_dir", test_resolve_library_dir_migrates },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;
        tests[i].fn();
        printf("  %-20s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

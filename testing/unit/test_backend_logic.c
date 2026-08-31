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
    s = json_escape("");                 CHECK_STR(s, "");                      free(s);
    s = json_escape(NULL);               CHECK_STR(s, "");                      free(s);
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

/* ------------------------------------------------------------- resume state */
static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
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

    /* what write_resume() actually emits */
    write_file(path, "{\"track_index\":3,\"position_ms\":125000,\"is_playing\":true}\n");
    CHECK(read_resume(&ti, &pos, &playing) == 1);
    CHECK_EQ_SIZE(ti, 3);
    CHECK(pos == 125000);
    CHECK(playing == 1);

    write_file(path, "{\"track_index\":0,\"position_ms\":0,\"is_playing\":false}\n");
    CHECK(read_resume(&ti, &pos, &playing) == 1);
    CHECK_EQ_SIZE(ti, 0);
    CHECK(pos == 0);
    CHECK(playing == 0);          /* the off-by-one that shipped once */

    write_file(path, "garbage not json");
    CHECK(read_resume(&ti, &pos, &playing) == 0);

    write_file(path, "{\"position_ms\":10}");   /* no track_index */
    CHECK(read_resume(&ti, &pos, &playing) == 0);

    unlink(path);
    resume_file[0] = '\0';
    CHECK(read_resume(&ti, &pos, &playing) == 0);   /* no path -> no resume */
}

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "json_escape",         test_json_escape },
        { "ssh_name_valid",      test_ssh_name_valid },
        { "shell_quote_words",   test_shell_quote_words },
        { "control_decode",      test_control_decode },
        { "next_autoplay_index", test_next_autoplay_index },
        { "play_queue",          test_play_queue },
        { "take_next_index",     test_take_next_index },
        { "next_encoded_token",  test_next_encoded_token },
        { "resume_roundtrip",    test_resume_roundtrip },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;
        tests[i].fn();
        printf("  %-20s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

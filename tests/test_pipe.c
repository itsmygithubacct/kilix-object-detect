/*
 * The pipe on its own, and the two doors opened for siblings.
 *
 * A face detector and a face embedder both live behind kod_pipe; what
 * they need proven here is that a reply comes back whole, that a missing
 * tool never opens, that the deadline is on the whole read, and that a
 * class outside COCO's allowlist can be let through on request.
 */

#include "kilix_object_detect.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

static const char *const FAKE[] = {"python3", "tests/fake_detect.py", NULL};
static const char *const GEOMETRY[] = {"--geometry", "64x64", NULL};

static long elapsed_ms_since(const struct timespec *from)
{
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - from->tv_sec) * 1000 +
           (now.tv_nsec - from->tv_nsec) / 1000000;
}

static bool test_a_reply_comes_back_whole(void)
{
    kod_pipe *pipe = NULL;
    kod_pipe_options options;
    uint8_t request[64 * 64 * 4];
    uint8_t reply[KOD_REPLY_BYTES];
    float rows[KOD_REPLY_ROWS][KOD_REPLY_COLUMNS];

    kod_pipe_options_init(&options);
    options.argv = FAKE;
    options.extra = GEOMETRY;
    options.timeout_seconds = 10;
    CHECK(kod_pipe_open(&pipe, &options));
    CHECK(!kod_pipe_warm(pipe));
    (void)memset(request, 0x40, sizeof(request));
    CHECK(kod_pipe_call(pipe, request, sizeof(request), reply, sizeof(reply)));
    CHECK(kod_pipe_warm(pipe));
    CHECK(kod_pipe_error(pipe) == NULL);
    (void)memcpy(rows, reply, sizeof(rows));
    CHECK(rows[0][0] == 0.0f);
    CHECK(rows[0][1] > 0.89f && rows[0][1] < 0.91f);
    /* A second round, so the steady-state clock is exercised too. */
    CHECK(kod_pipe_send(pipe, request, sizeof(request)));
    CHECK(kod_pipe_receive(pipe, reply, sizeof(reply)));
    kod_pipe_close(pipe);
    return true;
}

static bool test_a_missing_tool_never_opens(void)
{
    static const char *const MISSING[] = {"./definitely-not-a-tool", NULL};
    kod_pipe *pipe = NULL;
    kod_pipe_options options;

    kod_pipe_options_init(&options);
    options.argv = MISSING;
    options.log_path = "/dev/null";
    CHECK(!kod_pipe_open(&pipe, &options));
    CHECK(pipe == NULL);
    CHECK(!kod_pipe_open(&pipe, NULL));
    options.argv = NULL;
    CHECK(!kod_pipe_open(&pipe, &options));
    return true;
}

static bool test_resolution(void)
{
    char storage[512];
    const char *argv[KOD_ARGV_MAX + 1];

    (void)unsetenv("KOD_TEST_PIPE");
    CHECK(kod_pipe_resolve("KOD_TEST_PIPE", "kilix-look-detect", storage,
                           sizeof(storage), argv, KOD_ARGV_MAX + 1u));
    CHECK(argv[1] == NULL);
    CHECK(strstr(argv[0], "kilix-look-detect") != NULL);

    CHECK(setenv("KOD_TEST_PIPE", "  python3   tests/fake_detect.py ", 1) == 0);
    CHECK(kod_pipe_resolve("KOD_TEST_PIPE", "kilix-look-detect", storage,
                           sizeof(storage), argv, KOD_ARGV_MAX + 1u));
    CHECK(strcmp(argv[0], "python3") == 0);
    CHECK(strcmp(argv[1], "tests/fake_detect.py") == 0);
    CHECK(argv[2] == NULL);

    /* Too many words is refused whole and the bundled tool stands in;
     * never a truncated command. */
    CHECK(setenv("KOD_TEST_PIPE", "a b c d e f g h i j k l m n o", 1) == 0);
    CHECK(kod_pipe_resolve("KOD_TEST_PIPE", "kilix-look-detect", storage,
                           sizeof(storage), argv, KOD_ARGV_MAX + 1u));
    CHECK(argv[1] == NULL);
    CHECK(strstr(argv[0], "kilix-look-detect") != NULL);
    (void)unsetenv("KOD_TEST_PIPE");

    /* Nothing to fall back on: refused. */
    CHECK(!kod_pipe_resolve("KOD_TEST_PIPE", NULL, storage, sizeof(storage),
                            argv, KOD_ARGV_MAX + 1u));
    return true;
}

/* A reply dripped out slower than the warm-up allowance fails at the
 * deadline, not when the drip finishes. */
static bool test_the_deadline_is_on_the_whole_read(void)
{
    kod_pipe *pipe = NULL;
    kod_pipe_options options;
    uint8_t request[64 * 64 * 4];
    uint8_t reply[KOD_REPLY_BYTES];
    struct timespec started;

    CHECK(setenv("FAKE_DRIP", "10:400", 1) == 0);
    kod_pipe_options_init(&options);
    options.argv = FAKE;
    options.extra = GEOMETRY;
    options.timeout_seconds = 1;
    options.warmup_seconds = 1;
    CHECK(kod_pipe_open(&pipe, &options));
    (void)unsetenv("FAKE_DRIP");
    (void)memset(request, 0, sizeof(request));
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    CHECK(!kod_pipe_call(pipe, request, sizeof(request), reply,
                         sizeof(reply)));
    CHECK(elapsed_ms_since(&started) < 2500);
    CHECK(kod_pipe_error(pipe) != NULL);
    CHECK(strstr(kod_pipe_error(pipe), "never answered") != NULL);
    /* Broken stays broken. */
    CHECK(!kod_pipe_send(pipe, request, sizeof(request)));
    CHECK(!kod_pipe_pending(pipe));
    kod_pipe_close(pipe);
    return true;
}

/* A child that dies holding a request is reported as gone, at once -
 * not as slow, ten seconds later. */
static bool test_a_child_that_exits_is_not_slow(void)
{
    static const char *const DIES[] = {
        "python3", "-c",
        "import sys; sys.stdin.buffer.read(16384); sys.exit(3)", NULL};
    kod_pipe *pipe = NULL;
    kod_pipe_options options;
    uint8_t request[64 * 64 * 4];
    uint8_t reply[KOD_REPLY_BYTES];
    struct timespec started;

    kod_pipe_options_init(&options);
    options.argv = DIES;
    options.timeout_seconds = 10;
    options.warmup_seconds = 10;
    CHECK(kod_pipe_open(&pipe, &options));
    (void)memset(request, 0, sizeof(request));
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    CHECK(!kod_pipe_call(pipe, request, sizeof(request), reply,
                         sizeof(reply)));
    CHECK(elapsed_ms_since(&started) < 5000);
    CHECK(kod_pipe_exited(pipe));
    CHECK(strstr(kod_pipe_error(pipe), "exited") != NULL);
    kod_pipe_close(pipe);

    /* And through the detector, the same fact in its own words. */
    {
        kod_detector *detector = NULL;
        kod_options detector_options;
        kod_box boxes[KOD_BOX_MAX];
        size_t count = 0u;

        kod_options_init(&detector_options);
        detector_options.argv = DIES;
        detector_options.size = 64;
        detector_options.timeout_seconds = 10;
        detector_options.warmup_seconds = 10;
        CHECK(kod_open(&detector, &detector_options));
        (void)clock_gettime(CLOCK_MONOTONIC, &started);
        CHECK(!kod_detect(detector, request, 64, 64, boxes, KOD_BOX_MAX,
                          &count));
        CHECK(elapsed_ms_since(&started) < 5000);
        CHECK(strstr(kod_error(detector), "exited") != NULL);
        kod_close(detector);
    }
    return true;
}

static bool test_pending_and_overdue_do_not_block(void)
{
    kod_pipe *pipe = NULL;
    kod_pipe_options options;
    uint8_t request[64 * 64 * 4];
    uint8_t reply[KOD_REPLY_BYTES];
    struct timespec started;

    kod_pipe_options_init(&options);
    options.argv = FAKE;
    options.extra = GEOMETRY;
    options.timeout_seconds = 10;
    CHECK(kod_pipe_open(&pipe, &options));
    (void)memset(request, 0, sizeof(request));
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    CHECK(kod_pipe_send(pipe, request, sizeof(request)));
    CHECK(!kod_pipe_overdue(pipe));
    while (!kod_pipe_pending(pipe)) {
        struct timespec pause = {0, 1000000L};

        CHECK(elapsed_ms_since(&started) < 10000);
        (void)nanosleep(&pause, NULL);
    }
    CHECK(kod_pipe_receive(pipe, reply, sizeof(reply)));
    kod_pipe_close(pipe);
    return true;
}

static bool test_a_child_that_never_reads_has_a_bounded_send(void)
{
    static const char *const STALLS[] = {
        "python3", "-c", "import time; time.sleep(30)", NULL};
    kod_pipe *pipe = NULL;
    kod_pipe_options options;
    uint8_t *request = calloc(1024u * 1024u, 1u);
    struct timespec started;

    CHECK(request != NULL);
    kod_pipe_options_init(&options);
    options.argv = STALLS;
    options.timeout_seconds = 1;
    options.warmup_seconds = 1;
    CHECK(kod_pipe_open(&pipe, &options));
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    CHECK(!kod_pipe_send(pipe, request, 1024u * 1024u));
    CHECK(elapsed_ms_since(&started) < 2500);
    CHECK(strstr(kod_pipe_error(pipe), "request deadline") != NULL);
    CHECK(!kod_pipe_send(pipe, request, 1u));
    kod_pipe_close(pipe);
    free(request);
    return true;
}

/* Class 99 is nobody's COCO class: dropped by the allowlist, reported
 * when the caller says it labels the classes itself. */
static bool test_any_class(void)
{
    kod_detector *detector = NULL;
    kod_options options;
    uint8_t *frame = calloc((size_t)64 * 64 * 4, 1u);
    kod_box boxes[KOD_BOX_MAX];
    size_t count = 99u;

    CHECK(frame != NULL);
    CHECK(setenv("FAKE_CLASS", "99", 1) == 0);
    kod_options_init(&options);
    options.argv = FAKE;
    options.size = 64;
    options.timeout_seconds = 10;
    CHECK(kod_open(&detector, &options));
    CHECK(kod_detect(detector, frame, 64, 64, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 0u);
    kod_close(detector);

    options.any_class = true;
    CHECK(kod_open(&detector, &options));
    CHECK(kod_detect(detector, frame, 64, 64, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 1u);
    CHECK(boxes[0].class_id == 99);
    CHECK(kod_label(99) == NULL);
    kod_close(detector);
    (void)unsetenv("FAKE_CLASS");
    free(frame);
    return true;
}

/* A caller's mistake is refused with a reason, and the detector goes on
 * working; a fault sets the same field and does not. */
static bool test_bad_arguments_are_refused_not_faults(void)
{
    kod_detector *detector = NULL;
    kod_options options;
    uint8_t *frame = calloc((size_t)64 * 64 * 4, 1u);
    kod_box boxes[KOD_BOX_MAX];
    kod_rect region = {0, 0, 64, 64};
    size_t count = 0u;
    bool done = false;

    CHECK(frame != NULL);
    kod_options_init(&options);
    options.argv = FAKE;
    options.size = 64;
    options.timeout_seconds = 10;
    CHECK(kod_open(&detector, &options));
    CHECK(!kod_detect(detector, NULL, 64, 64, boxes, KOD_BOX_MAX, &count));
    CHECK(kod_error(detector) != NULL);
    CHECK(!kod_detect_regions(detector, frame, 0, 64, &region, 1u, boxes,
                              KOD_BOX_MAX, &count));
    CHECK(kod_error(detector) != NULL);
    CHECK(!kod_offer(detector, frame, 64, 64, NULL, 0u));
    CHECK(!kod_take(detector, NULL, KOD_BOX_MAX, &count, &done));
    /* Still alive. */
    CHECK(kod_detect(detector, frame, 64, 64, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 1u);
    kod_close(detector);
    free(frame);
    return true;
}

/* Not-a-number in any column is not a detection.  The fake takes its
 * score and box from the environment, and Python reads "nan" as NaN. */
static bool test_nan_rows_are_not_boxes(void)
{
    kod_detector *detector = NULL;
    kod_options options;
    uint8_t *frame = calloc((size_t)64 * 64 * 4, 1u);
    kod_box boxes[KOD_BOX_MAX];
    size_t count = 9u;

    CHECK(frame != NULL);
    kod_options_init(&options);
    options.argv = FAKE;
    options.size = 64;
    options.timeout_seconds = 10;
    CHECK(setenv("FAKE_SCORE", "nan", 1) == 0);
    CHECK(kod_open(&detector, &options));
    CHECK(unsetenv("FAKE_SCORE") == 0);
    CHECK(kod_detect(detector, frame, 64, 64, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 0u);
    kod_close(detector);

    CHECK(setenv("FAKE_BOX", "nan,0.25,0.75,0.75", 1) == 0);
    CHECK(kod_open(&detector, &options));
    CHECK(unsetenv("FAKE_BOX") == 0);
    CHECK(kod_detect(detector, frame, 64, 64, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 0u);
    kod_close(detector);

    /* The class is cast to int. A NaN or an out-of-range value has to be
     * rejected before that cast, not after it has already happened. */
    CHECK(setenv("FAKE_CLASS", "nan", 1) == 0);
    CHECK(kod_open(&detector, &options));
    CHECK(unsetenv("FAKE_CLASS") == 0);
    CHECK(kod_detect(detector, frame, 64, 64, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 0u);
    kod_close(detector);

    CHECK(setenv("FAKE_CLASS", "1e20", 1) == 0);
    options.any_class = true;
    CHECK(kod_open(&detector, &options));
    CHECK(unsetenv("FAKE_CLASS") == 0);
    CHECK(kod_detect(detector, frame, 64, 64, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 0u);
    kod_close(detector);
    options.any_class = false;

    CHECK(setenv("FAKE_BOX", "0.25,1e20,0.75,1e20", 1) == 0);
    CHECK(kod_open(&detector, &options));
    CHECK(unsetenv("FAKE_BOX") == 0);
    CHECK(kod_detect(detector, frame, 64, 64, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 0u);
    kod_close(detector);
    free(frame);
    return true;
}

/* A crop larger than the pipe buffer must fail inside the request
 * allowance when the child never reads, and the detector has to say so.
 * The pipe test covers the same deadline; this is the path kilix-look
 * and FaceNet actually call. */
static bool test_a_stalled_detector_reports_the_deadline(void)
{
    static const char *const STALLS[] = {
        "python3", "-c", "import time; time.sleep(30)", NULL};
    kod_detector *detector = NULL;
    kod_options options;
    uint8_t *frame = calloc((size_t)320 * 320 * 4u, 1u);
    kod_box boxes[1];
    size_t count = 0u;
    struct timespec started;

    CHECK(frame != NULL);
    kod_options_init(&options);
    options.argv = STALLS;
    options.size = 320;
    options.timeout_seconds = 1;
    options.warmup_seconds = 1;
    CHECK(kod_open(&detector, &options));
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    CHECK(!kod_detect(detector, frame, 320, 320, boxes, 1u, &count));
    CHECK(elapsed_ms_since(&started) < 2500);
    CHECK(kod_error(detector) != NULL);
    CHECK(strstr(kod_error(detector), "deadline") != NULL);
    kod_close(detector);
    free(frame);
    return true;
}

/* Every rectangle that does not come out is counted, including the ones
 * the merge table itself could not hold. */
static bool test_every_dropped_region_is_counted(void)
{
    kod_rect motion[40];
    kod_rect out[KOD_REGION_MAX];
    size_t dropped = 0u;
    size_t written;

    for (int i = 0; i < 40; i++) {
        motion[i].x = (i % 8) * 200;
        motion[i].y = (i / 8) * 200;
        motion[i].w = 10;
        motion[i].h = 10;
    }
    written = kod_regions(motion, 40u, 1600, 1000, 32, out, KOD_REGION_MAX,
                          &dropped);
    CHECK(written == KOD_REGION_MAX);
    CHECK(written + dropped == 40u);
    return true;
}

/* The public scaler: the fitted geometry of a real rectangle, and a
 * black square at scale 1 for a degenerate one. */
static bool test_the_public_scaler(void)
{
    uint8_t *frame = calloc((size_t)640 * 360 * 4, 1u);
    uint8_t square[32 * 32 * 4];
    kod_rect from = {100, 50, 64, 32};
    kod_rect empty = {0, 0, 0, 0};
    kod_fit fit;

    CHECK(frame != NULL);
    (void)memset(frame, 0xFF, (size_t)640 * 360 * 4);
    fit = kod_scale_into_square(frame, 640, 360, &from, square, 32);
    CHECK(fit.scale > 1.99f && fit.scale < 2.01f);
    CHECK(fit.offset_x == 0);
    CHECK(fit.offset_y == 8);
    CHECK(square[0] == 0u);                       /* letterbox row */
    CHECK(square[(8 * 32) * 4] == 0xFFu);         /* first drawn pixel */
    fit = kod_scale_into_square(frame, 640, 360, &empty, square, 32);
    CHECK(fit.scale == 1.0f && fit.offset_x == 0 && fit.offset_y == 0);
    CHECK(square[(8 * 32) * 4] == 0u);
    free(frame);

    /* An origin near the top of the int range used to overflow the fast
     * path's unchecked index. It must come back without a fault. */
    {
        uint8_t tiny[4 * 4 * 4];
        uint8_t out[8 * 8 * 4];
        kod_rect huge = {2000000000, 2000000000, 2000000000, 2000000000};

        (void)memset(tiny, 0x11, sizeof(tiny));
        fit = kod_scale_into_square(tiny, 4, 4, &huge, out, 8);
        CHECK(fit.scale > 0.0f);
        CHECK(out[0] == 0u);
    }
    return true;
}

int main(void)
{
    struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"a reply comes back whole", test_a_reply_comes_back_whole},
        {"a missing tool never opens", test_a_missing_tool_never_opens},
        {"resolution: env words, bundled fallback, refusal",
         test_resolution},
        {"the deadline is on the whole read",
         test_the_deadline_is_on_the_whole_read},
        {"a child that exits is not slow",
         test_a_child_that_exits_is_not_slow},
        {"pending and overdue do not block",
         test_pending_and_overdue_do_not_block},
        {"a child that never reads has a bounded send",
         test_a_child_that_never_reads_has_a_bounded_send},
        {"any_class lets a non-COCO class through", test_any_class},
        {"bad arguments are refused, not faults",
         test_bad_arguments_are_refused_not_faults},
        {"nan rows are not boxes", test_nan_rows_are_not_boxes},
        {"a stalled detector reports the deadline",
         test_a_stalled_detector_reports_the_deadline},
        {"every dropped region is counted",
         test_every_dropped_region_is_counted},
        {"the public scaler", test_the_public_scaler},
    };
    int failed = 0;

    for (size_t i = 0u; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (tests[i].run()) {
            (void)printf("ok %s\n", tests[i].name);
        } else {
            (void)printf("FAIL %s\n", tests[i].name);
            failed++;
        }
    }
    return failed == 0 ? 0 : 1;
}

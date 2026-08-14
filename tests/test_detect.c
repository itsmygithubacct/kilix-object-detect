/*
 * The detector, and the coordinate arithmetic around it.
 *
 * The fake detector answers with a box at a fixed place in its *own*
 * normalised space, which is what makes this a test of the mapping rather
 * than of a model: whatever crop it was handed, the reply is the same, so
 * where the box lands in the frame is entirely this library's arithmetic.
 * Boxes in the wrong place is the failure mode that looks like a working
 * program, and it has happened here before.
 */

#include "kilix_object_detect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

#define W 640
#define H 360
#define SIZE 128

static const char *const FAKE[] = {"python3", "tests/fake_detect.py", NULL};

static bool start(kod_detector **detector, float min_score)
{
    kod_options options;

    kod_options_init(&options);
    options.argv = FAKE;
    options.size = SIZE;
    options.min_score = min_score;
    options.timeout_seconds = 10;
    return kod_open(detector, &options);
}

static uint8_t *blank(void)
{
    return calloc((size_t)W * (size_t)H * 4u, 1u);
}

/*
 * A whole frame is letterboxed into the square, so a reply covering the
 * middle half of the square must come back as the middle half of the
 * frame - not of the square, and not stretched.
 */
static bool test_a_whole_frame_maps_back(void)
{
    kod_detector *detector = NULL;
    uint8_t *frame = blank();
    kod_box boxes[KOD_BOX_MAX];
    size_t count = 0u;

    CHECK(frame != NULL);
    CHECK(start(&detector, 0.25f));
    CHECK(kod_detect(detector, frame, W, H, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 1u);
    CHECK(boxes[0].class_id == 0);
    CHECK(boxes[0].region == -1);
    /*
     * The fake answers over the middle half of the *square*, bars
     * included.  A 16:9 frame in a square is 72 rows of picture in 128,
     * so half the square vertically is most of the picture - which is the
     * arithmetic being checked, and the reason a naive "a quarter in
     * means a quarter in" expectation is wrong here.
     */
    CHECK(boxes[0].at.x > 150 && boxes[0].at.x < 170);
    CHECK(boxes[0].at.y >= 0 && boxes[0].at.y < 30);
    CHECK(boxes[0].at.w > 310 && boxes[0].at.w < 330);
    CHECK(boxes[0].at.h > 300 && boxes[0].at.h <= H);
    /* And inside the frame, always. */
    CHECK(boxes[0].at.x >= 0 && boxes[0].at.y >= 0);
    CHECK(boxes[0].at.x + boxes[0].at.w <= W);
    CHECK(boxes[0].at.y + boxes[0].at.h <= H);
    CHECK(kod_crops(detector) == 1u);
    kod_close(detector);
    free(frame);
    return true;
}

/*
 * And the other direction: a reply that covers the middle half of the
 * *picture* inside the letterbox comes back as the middle half of the
 * frame.  Between this and the case above, the bar arithmetic is pinned
 * from both sides.
 */
static bool test_the_letterbox_is_undone(void)
{
    kod_detector *detector = NULL;
    uint8_t *frame = blank();
    kod_box boxes[KOD_BOX_MAX];
    size_t count = 0u;

    CHECK(frame != NULL);
    /* 72 rows of picture centred in 128: the middle half of it runs from
     * (28 + 18)/128 to (28 + 54)/128. */
    CHECK(setenv("FAKE_BOX", "0.359375,0.25,0.640625,0.75", 1) == 0);
    CHECK(start(&detector, 0.25f));
    CHECK(kod_detect(detector, frame, W, H, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 1u);
    CHECK(boxes[0].at.x > 150 && boxes[0].at.x < 170);
    CHECK(boxes[0].at.y > 80 && boxes[0].at.y < 100);
    CHECK(boxes[0].at.w > 310 && boxes[0].at.w < 330);
    CHECK(boxes[0].at.h > 170 && boxes[0].at.h < 190);
    kod_close(detector);
    free(frame);
    CHECK(unsetenv("FAKE_BOX") == 0);
    return true;
}

/*
 * The point of the whole exercise: a crop in the corner of the frame
 * returns a box in that corner, not in the middle.
 */
static bool test_a_crop_maps_into_its_own_corner(void)
{
    kod_detector *detector = NULL;
    uint8_t *frame = blank();
    kod_box boxes[KOD_BOX_MAX];
    kod_rect region;
    size_t count = 0u;

    CHECK(frame != NULL);
    CHECK(start(&detector, 0.25f));
    region.x = 400;
    region.y = 200;
    region.w = 160;
    region.h = 160;
    CHECK(kod_detect_regions(detector, frame, W, H, &region, 1u, boxes,
                             KOD_BOX_MAX, &count));
    CHECK(count == 1u);
    CHECK(boxes[0].region == 0);
    /* 0.25..0.75 of a 160-pixel square at (400,200). */
    CHECK(boxes[0].at.x > 430 && boxes[0].at.x < 450);
    CHECK(boxes[0].at.y > 230 && boxes[0].at.y < 250);
    CHECK(boxes[0].at.w > 70 && boxes[0].at.w < 90);
    CHECK(boxes[0].at.h > 70 && boxes[0].at.h < 90);
    kod_close(detector);
    free(frame);
    return true;
}

/* Two crops of the same thing is one thing. */
static bool test_overlapping_crops_report_once(void)
{
    kod_detector *detector = NULL;
    uint8_t *frame = blank();
    kod_box boxes[KOD_BOX_MAX];
    kod_rect regions[2];
    size_t count = 0u;

    CHECK(frame != NULL);
    CHECK(start(&detector, 0.25f));
    regions[0].x = 100; regions[0].y = 100;
    regions[0].w = 160; regions[0].h = 160;
    regions[1].x = 104; regions[1].y = 104;   /* nearly the same crop */
    regions[1].w = 160; regions[1].h = 160;
    CHECK(kod_detect_regions(detector, frame, W, H, regions, 2u, boxes,
                             KOD_BOX_MAX, &count));
    CHECK(kod_crops(detector) == 2u);   /* both were run... */
    CHECK(count == 1u);                 /* ...and reported as one object */
    kod_close(detector);
    free(frame);
    return true;
}

/* Nothing moved is not an error, and costs no inference at all - which is
 * the entire saving this design exists for. */
static bool test_no_regions_costs_nothing(void)
{
    kod_detector *detector = NULL;
    uint8_t *frame = blank();
    kod_box boxes[KOD_BOX_MAX];
    size_t count = 7u;

    CHECK(frame != NULL);
    CHECK(start(&detector, 0.25f));
    CHECK(kod_detect_regions(detector, frame, W, H, NULL, 0u, boxes,
                             KOD_BOX_MAX, &count));
    CHECK(count == 0u);
    CHECK(kod_crops(detector) == 0u);
    kod_close(detector);
    free(frame);
    return true;
}

static bool test_the_threshold_and_the_allowlist(void)
{
    kod_detector *detector = NULL;
    uint8_t *frame = blank();
    kod_box boxes[KOD_BOX_MAX];
    size_t count = 0u;

    CHECK(frame != NULL);
    /* The fake answers 0.90; asking for more silences it without
     * breaking the stream. */
    CHECK(start(&detector, 0.95f));
    CHECK(kod_detect(detector, frame, W, H, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 0u);
    kod_close(detector);

    /* A class nobody asked about is dropped whatever it scores. */
    CHECK(setenv("FAKE_CLASS", "61", 1) == 0);   /* toilet */
    CHECK(start(&detector, 0.25f));
    CHECK(kod_detect(detector, frame, W, H, boxes, KOD_BOX_MAX, &count));
    CHECK(count == 0u);
    kod_close(detector);
    CHECK(unsetenv("FAKE_CLASS") == 0);
    free(frame);
    return true;
}

static bool test_a_dead_detector_is_survivable(void)
{
    static const char *const MISSING[] = {"kilix-no-such-detector", NULL};
    static const char *const LOG = "build/test-detect-child.log";
    kod_options options;
    kod_detector *detector = NULL;
    uint8_t *frame = blank();
    kod_box boxes[KOD_BOX_MAX];
    size_t count = 0u;

    CHECK(frame != NULL);
    kod_options_init(&options);
    options.argv = MISSING;
    options.size = SIZE;
    options.timeout_seconds = 2;
    options.log_path = LOG;
    (void)remove(LOG);
    /*
     * Refused at open, not on the first crop.
     *
     * This used to succeed and surface as "the detector stopped reading"
     * later, against an empty log - which on a recorder reads as a model
     * that crashed rather than one that was never installed.
     */
    CHECK(!kod_open(&detector, &options));
    CHECK(detector == NULL);
    {
        FILE *log = fopen(LOG, "r");
        char line[512] = "";
        bool said = false;

        CHECK(log != NULL);
        said = fgets(line, (int)sizeof(line), log) != NULL;
        (void)fclose(log);
        CHECK(said);
        CHECK(strstr(line, "kilix-no-such-detector") != NULL);
    }
    (void)boxes;
    (void)count;
    free(frame);
    return true;
}

static bool test_rejections(void)
{
    kod_detector *detector = NULL;
    kod_options options;
    uint8_t *frame = blank();
    kod_box boxes[KOD_BOX_MAX];
    size_t count = 0u;

    CHECK(frame != NULL);
    kod_options_init(&options);
    options.argv = FAKE;
    options.size = 0;
    CHECK(!kod_open(&detector, &options));
    options.size = 100000;
    CHECK(!kod_open(&detector, &options));
    CHECK(!kod_open(NULL, NULL));
    CHECK(kod_error(NULL) == NULL);
    CHECK(kod_crops(NULL) == 0u);
    CHECK(!kod_detect(NULL, frame, W, H, boxes, KOD_BOX_MAX, &count));

    CHECK(start(&detector, 0.25f));
    CHECK(!kod_detect(detector, NULL, W, H, boxes, KOD_BOX_MAX, &count));
    CHECK(!kod_detect(detector, frame, 0, 0, boxes, KOD_BOX_MAX, &count));
    kod_close(detector);
    kod_close(NULL);
    free(frame);
    return true;
}

/*
 * The whole point of the non-blocking path: offering costs nothing and
 * the answer arrives later, at the same coordinates the blocking call
 * would have produced.
 */
static bool test_offering_does_not_wait(void)
{
    kod_detector *detector = NULL;
    uint8_t *frame = blank();
    kod_box async_boxes[KOD_BOX_MAX];
    kod_box sync_boxes[KOD_BOX_MAX];
    kod_rect regions[2];
    size_t async_count = 0u;
    size_t sync_count = 0u;
    bool done = false;
    int spins = 0;

    CHECK(frame != NULL);
    CHECK(start(&detector, 0.1f));
    regions[0].x = 0;    regions[0].y = 0;   regions[0].w = 200;
    regions[0].h = 200;
    regions[1].x = 400;  regions[1].y = 100; regions[1].w = 160;
    regions[1].h = 160;

    CHECK(kod_offer(detector, frame, W, H, regions, 2u));
    CHECK(kod_busy(detector));
    /* A second offer while one is in flight is refused, and refusing must
     * not be recorded as a fault. */
    CHECK(!kod_offer(detector, frame, W, H, regions, 2u));
    CHECK(kod_error(detector) == NULL);

    while (!done && spins < 100000) {
        CHECK(kod_take(detector, async_boxes, KOD_BOX_MAX, &async_count,
                       &done));
        spins++;
    }
    CHECK(done);
    CHECK(!kod_busy(detector));
    CHECK(async_count > 0u);
    /* Taking again with nothing in flight is done, not an error. */
    done = false;
    CHECK(kod_take(detector, async_boxes, KOD_BOX_MAX, NULL, &done));
    CHECK(done);

    /* ...and the same regions through the blocking call agree. */
    CHECK(kod_offer(detector, frame, W, H, regions, 2u) || true);
    while (kod_busy(detector)) {
        CHECK(kod_take(detector, sync_boxes, KOD_BOX_MAX, &sync_count, &done));
    }
    CHECK(kod_detect_regions(detector, frame, W, H, regions, 2u, sync_boxes,
                             KOD_BOX_MAX, &sync_count));
    CHECK(sync_count == async_count);
    for (size_t i = 0u; i < sync_count; i++) {
        CHECK(sync_boxes[i].at.x == async_boxes[i].at.x);
        CHECK(sync_boxes[i].at.y == async_boxes[i].at.y);
        CHECK(sync_boxes[i].at.w == async_boxes[i].at.w);
        CHECK(sync_boxes[i].at.h == async_boxes[i].at.h);
    }
    kod_close(detector);
    free(frame);
    return true;
}

/*
 * A blocking call while a batch is in flight is refused, with a reason -
 * and nothing more.  The batch it collided with is healthy, so refusing
 * it must not disable the detector: the batch still completes, and the
 * blocking calls work again once it has.
 */
static bool test_a_busy_refusal_does_not_poison(void)
{
    kod_detector *detector = NULL;
    uint8_t *frame = blank();
    kod_box boxes[KOD_BOX_MAX];
    kod_rect regions[2];
    size_t count = 0u;
    bool done = false;

    CHECK(frame != NULL);
    CHECK(start(&detector, 0.1f));
    regions[0].x = 0;    regions[0].y = 0;   regions[0].w = 200;
    regions[0].h = 200;
    regions[1].x = 400;  regions[1].y = 100; regions[1].w = 160;
    regions[1].h = 160;
    CHECK(kod_offer(detector, frame, W, H, regions, 2u));
    CHECK(kod_busy(detector));

    CHECK(!kod_detect(detector, frame, W, H, boxes, KOD_BOX_MAX, &count));
    CHECK(kod_error(detector) != NULL);
    CHECK(!kod_detect_regions(detector, frame, W, H, regions, 2u, boxes,
                              KOD_BOX_MAX, &count));

    /* The batch it refused for is still in flight and still completes. */
    while (!done) {
        CHECK(kod_take(detector, boxes, KOD_BOX_MAX, &count, &done));
    }
    CHECK(count > 0u);

    /* And now that the detector is free, blocking calls work. */
    CHECK(kod_detect(detector, frame, W, H, boxes, KOD_BOX_MAX, &count));
    CHECK(count > 0u);
    kod_close(detector);
    free(frame);
    return true;
}

/* The frame may go away the moment it has been offered. */
static bool test_the_frame_may_be_released(void)
{
    kod_detector *detector = NULL;
    uint8_t *frame = blank();
    kod_box boxes[KOD_BOX_MAX];
    kod_rect regions[2];
    size_t count = 0u;
    bool done = false;

    CHECK(frame != NULL);
    CHECK(start(&detector, 0.1f));
    regions[0].x = 0;    regions[0].y = 0;   regions[0].w = 200;
    regions[0].h = 200;
    regions[1].x = 400;  regions[1].y = 100; regions[1].w = 160;
    regions[1].h = 160;
    CHECK(kod_offer(detector, frame, W, H, regions, 2u));
    /* Freed while a two-crop batch is in flight: the second crop must
     * already be the detector's own copy, or this reads freed memory and
     * the answer changes with whatever lands there. */
    free(frame);
    while (!done) {
        CHECK(kod_take(detector, boxes, KOD_BOX_MAX, &count, &done));
    }
    CHECK(count > 0u);
    kod_close(detector);
    return true;
}

int main(void)
{
    const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        {"a whole frame maps back", test_a_whole_frame_maps_back},
        {"the letterbox is undone", test_the_letterbox_is_undone},
        {"a crop maps into its own corner",
         test_a_crop_maps_into_its_own_corner},
        {"overlapping crops report once",
         test_overlapping_crops_report_once},
        {"no regions costs nothing", test_no_regions_costs_nothing},
        {"the threshold and the allowlist",
         test_the_threshold_and_the_allowlist},
        {"a dead detector is survivable",
         test_a_dead_detector_is_survivable},
        {"rejections", test_rejections},
        {"offering does not wait", test_offering_does_not_wait},
        {"a busy refusal does not poison",
         test_a_busy_refusal_does_not_poison},
        {"the frame may be released",
         test_the_frame_may_be_released}
    };
    size_t passed = 0u;

    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}

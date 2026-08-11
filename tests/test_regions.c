/*
 * Crops, which is the half of this that has no excuse for being wrong.
 *
 * Every failure here is silent in production: a crop that leaves the
 * frame, a crop smaller than the model's input, two crops of one object
 * charged twice. None of them look like errors — they look like a
 * detector that is worse than it should be.
 */

#include "kilix_object_detect.h"

#include <stdio.h>
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

static kod_rect rect(int x, int y, int w, int h)
{
    kod_rect out;

    out.x = x;
    out.y = y;
    out.w = w;
    out.h = h;
    return out;
}

static bool inside(const kod_rect *region, int width, int height)
{
    return region->x >= 0 && region->y >= 0 && region->w == region->h &&
           region->x + region->w <= width && region->y + region->h <= height;
}

static bool covers(const kod_rect *region, const kod_rect *motion)
{
    return region->x <= motion->x && region->y <= motion->y &&
           region->x + region->w >= motion->x + motion->w &&
           region->y + region->h >= motion->y + motion->h;
}

static bool test_a_crop_is_square_and_big_enough(void)
{
    kod_rect motion = rect(100, 100, 20, 40);
    kod_rect regions[KOD_REGION_MAX];
    size_t dropped = 0u;
    const size_t count = kod_regions(&motion, 1u, W, H, 320, regions,
                                     KOD_REGION_MAX, &dropped);

    CHECK(count == 1u);
    CHECK(dropped == 0u);
    CHECK(inside(&regions[0], W, H));
    CHECK(regions[0].w >= 320);
    /* And it contains what moved, with room around it: a box drawn tight
     * around the moving pixels of a person is a box around their legs. */
    CHECK(covers(&regions[0], &motion));
    return true;
}

/* A crop cannot be bigger than the frame, however small the frame is. */
static bool test_a_small_frame_is_not_exceeded(void)
{
    kod_rect motion = rect(10, 10, 20, 20);
    kod_rect regions[KOD_REGION_MAX];
    const size_t count = kod_regions(&motion, 1u, 200, 120, 320, regions,
                                     KOD_REGION_MAX, NULL);

    CHECK(count == 1u);
    CHECK(inside(&regions[0], 200, 120));
    CHECK(regions[0].w == 120);   /* the short side wins */
    return true;
}

/* Motion at the very edge still yields a square, shifted rather than
 * shrunk: a square that is quietly turned into a rectangle is a square
 * the model was never given. */
static bool test_motion_at_the_edge_stays_square(void)
{
    const kod_rect corners[] = {
        {0, 0, 30, 30}, {W - 30, 0, 30, 30},
        {0, H - 30, 30, 30}, {W - 30, H - 30, 30, 30}
    };

    for (size_t i = 0u; i < sizeof(corners) / sizeof(corners[0]); i++) {
        kod_rect regions[KOD_REGION_MAX];
        const size_t count = kod_regions(&corners[i], 1u, W, H, 320, regions,
                                         KOD_REGION_MAX, NULL);

        CHECK(count == 1u);
        CHECK(inside(&regions[0], W, H));
        CHECK(covers(&regions[0], &corners[i]));
    }
    return true;
}

static bool test_overlapping_motion_is_one_crop(void)
{
    kod_rect motion[3];
    kod_rect regions[KOD_REGION_MAX];
    size_t count;

    motion[0] = rect(100, 100, 40, 60);
    motion[1] = rect(110, 120, 40, 60);   /* overlapping */
    motion[2] = rect(160, 130, 20, 20);   /* near, within the slack */
    count = kod_regions(motion, 3u, W, H, 320, regions, KOD_REGION_MAX, NULL);
    CHECK(count == 1u);
    for (size_t i = 0u; i < 3u; i++) {
        CHECK(covers(&regions[0], &motion[i]));
    }
    return true;
}

/* Two things at opposite ends of a big frame are two crops - merging them
 * would hand the model one crop of mostly grass. */
static bool test_distant_motion_is_two_crops(void)
{
    kod_rect motion[2];
    kod_rect regions[KOD_REGION_MAX];
    size_t count;

    motion[0] = rect(0, 0, 40, 40);
    motion[1] = rect(1800, 1000, 40, 40);
    count = kod_regions(motion, 2u, 1920, 1080, 320, regions, KOD_REGION_MAX,
                        NULL);
    CHECK(count == 2u);
    CHECK(inside(&regions[0], 1920, 1080));
    CHECK(inside(&regions[1], 1920, 1080));
    return true;
}

/*
 * A chain of boxes that only touch pairwise still merges: A touches B and
 * B touches C, so all three are one thing.  The first pass cannot see
 * that, which is why there is a second.
 */
static bool test_a_chain_merges(void)
{
    kod_rect motion[3];
    kod_rect regions[KOD_REGION_MAX];
    size_t count;

    motion[0] = rect(100, 500, 40, 40);
    motion[1] = rect(700, 500, 40, 40);
    /* Overlaps the first and reaches the second, so all three are one
     * thing - which the first pass cannot see, because by the time it
     * meets this one the other two are already separate groups. */
    motion[2] = rect(130, 500, 590, 40);
    count = kod_regions(motion, 3u, 1920, 1080, 320, regions, KOD_REGION_MAX,
                        NULL);
    CHECK(count == 1u);
    for (size_t i = 0u; i < 3u; i++) {
        CHECK(covers(&regions[0], &motion[i]));
    }
    return true;
}

/* Beyond the cap the smallest go, and saying so is the point: silent
 * truncation reads as "nothing else moved". */
static bool test_too_many_are_dropped_and_counted(void)
{
    kod_rect motion[KOD_REGION_MAX + 4];
    kod_rect regions[KOD_REGION_MAX];
    size_t dropped = 0u;
    size_t count;

    for (size_t i = 0u; i < sizeof(motion) / sizeof(motion[0]); i++) {
        motion[i] = rect((int)i * 400, (int)i * 200, 30 + (int)i * 10,
                         30 + (int)i * 10);
    }
    count = kod_regions(motion, sizeof(motion) / sizeof(motion[0]), 8000,
                        4000, 320, regions, KOD_REGION_MAX, &dropped);
    CHECK(count == KOD_REGION_MAX);
    CHECK(dropped == 4u);
    /* Biggest first, so what survives is what mattered. */
    CHECK(regions[0].w >= regions[KOD_REGION_MAX - 1u].w);
    return true;
}

static bool test_rejections(void)
{
    kod_rect motion = rect(10, 10, 20, 20);
    kod_rect regions[KOD_REGION_MAX];
    size_t dropped = 7u;

    CHECK(kod_regions(NULL, 1u, W, H, 320, regions, KOD_REGION_MAX,
                      &dropped) == 0u);
    CHECK(dropped == 0u);
    CHECK(kod_regions(&motion, 0u, W, H, 320, regions, KOD_REGION_MAX,
                      NULL) == 0u);
    CHECK(kod_regions(&motion, 1u, 0, 0, 320, regions, KOD_REGION_MAX,
                      NULL) == 0u);
    CHECK(kod_regions(&motion, 1u, W, H, 320, NULL, KOD_REGION_MAX,
                      NULL) == 0u);
    CHECK(kod_regions(&motion, 1u, W, H, 320, regions, 0u, NULL) == 0u);
    /* A zero-sized motion box is not motion. */
    {
        kod_rect empty = rect(10, 10, 0, 0);

        CHECK(kod_regions(&empty, 1u, W, H, 320, regions, KOD_REGION_MAX,
                          NULL) == 0u);
    }
    return true;
}

static bool test_the_allowlist(void)
{
    int class_id = -1;

    CHECK(kod_class_from_name("person") == 0);
    CHECK(strcmp(kod_label(2), "car") == 0);
    CHECK(kod_class_from_name("toilet") < 0);
    CHECK(kod_label(61) == NULL);
    CHECK(kod_label(-1) == NULL);
    CHECK(kod_class_at(0u, &class_id) != NULL && class_id == 0);
    CHECK(kod_class_at(10000u, NULL) == NULL);
    return true;
}

int main(void)
{
    const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        {"a crop is square and big enough",
         test_a_crop_is_square_and_big_enough},
        {"a small frame is not exceeded", test_a_small_frame_is_not_exceeded},
        {"motion at the edge stays square",
         test_motion_at_the_edge_stays_square},
        {"overlapping motion is one crop",
         test_overlapping_motion_is_one_crop},
        {"distant motion is two crops", test_distant_motion_is_two_crops},
        {"a chain merges", test_a_chain_merges},
        {"too many are dropped and counted",
         test_too_many_are_dropped_and_counted},
        {"rejections", test_rejections},
        {"the allowlist", test_the_allowlist}
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

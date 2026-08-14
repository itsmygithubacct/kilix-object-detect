/*
 * The scaler, pinned byte for byte against its own plain transcription.
 *
 * scale_into_square is the arithmetic the whole module leans on, and it
 * is allowed to get faster but never to get different: area averaging is
 * load-bearing (point sampling reported movement on 100% of frames of a
 * still scene), and the letterbox offsets feed straight into the
 * coordinate mapping the other tests pin.  So the oracle here is the
 * original implementation, kept verbatim - one memset, one pair of
 * loops, a bounds check per source pixel - and every case must produce
 * the same bytes and the same placement: letterboxed whole frames both
 * ways round, crops inside the frame, crops that leave it on every
 * side, crops entirely outside it, and a few hundred random rectangles.
 */

#include "../src/kilix_object_detect.c"

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

#define FRAME_W 640
#define FRAME_H 360
#define SQUARE_MAX 321

/* The shipped algorithm as it was first written, kept as the oracle. */
static fit reference_scale_into_square(
    const uint8_t *bgra, int width, int height, const kod_rect *from,
    uint8_t *square, int size)
{
    fit placement;
    const float sx = (float)from->w / (float)size;
    const float sy = (float)from->h / (float)size;
    int drawn_w;
    int drawn_h;

    placement.scale = sx > sy ? sx : sy;
    drawn_w = (int)((float)from->w / placement.scale);
    drawn_h = (int)((float)from->h / placement.scale);
    if (drawn_w > size) { drawn_w = size; }
    if (drawn_h > size) { drawn_h = size; }
    placement.offset_x = (size - drawn_w) / 2;
    placement.offset_y = (size - drawn_h) / 2;

    (void)memset(square, 0, (size_t)size * (size_t)size * 4u);
    for (int y = 0; y < drawn_h; y++) {
        for (int x = 0; x < drawn_w; x++) {
            const int from_x0 = from->x + (int)((float)x * placement.scale);
            const int from_y0 = from->y + (int)((float)y * placement.scale);
            int from_x1 = from->x + (int)((float)(x + 1) * placement.scale);
            int from_y1 = from->y + (int)((float)(y + 1) * placement.scale);
            uint32_t blue = 0u;
            uint32_t green = 0u;
            uint32_t red = 0u;
            uint32_t taken = 0u;
            uint8_t *target;

            if (from_x1 <= from_x0) { from_x1 = from_x0 + 1; }
            if (from_y1 <= from_y0) { from_y1 = from_y0 + 1; }
            for (int sy_at = from_y0; sy_at < from_y1; sy_at++) {
                if (sy_at < 0 || sy_at >= height) {
                    continue;
                }
                for (int sx_at = from_x0; sx_at < from_x1; sx_at++) {
                    const uint8_t *pixel;

                    if (sx_at < 0 || sx_at >= width) {
                        continue;
                    }
                    pixel = bgra + ((size_t)sy_at * (size_t)width +
                                    (size_t)sx_at) * 4u;
                    blue += pixel[0];
                    green += pixel[1];
                    red += pixel[2];
                    taken++;
                }
            }
            if (taken == 0u) {
                continue;
            }
            target = square + ((size_t)(y + placement.offset_y) *
                               (size_t)size +
                               (size_t)(x + placement.offset_x)) * 4u;
            target[0] = (uint8_t)(blue / taken);
            target[1] = (uint8_t)(green / taken);
            target[2] = (uint8_t)(red / taken);
            target[3] = 0xFFu;
        }
    }
    return placement;
}

static uint32_t rng_state = 0x2545F491u;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int rng_between(int low, int high)
{
    return low + (int)(rng() % (uint32_t)(high - low + 1));
}

/*
 * Both implementations over one rectangle, into squares prefilled with a
 * value nothing writes: a pixel the new code forgot to clear shows up as
 * loudly as one it miscomputed.
 */
static bool agrees(const uint8_t *frame, int width, int height,
                   const kod_rect *from, int size, const char *what)
{
    static uint8_t ours[SQUARE_MAX * SQUARE_MAX * 4];
    static uint8_t reference[SQUARE_MAX * SQUARE_MAX * 4];
    const size_t bytes = (size_t)size * (size_t)size * 4u;
    fit got;
    fit expected;

    (void)memset(ours, 0xAB, bytes);
    (void)memset(reference, 0xAB, bytes);
    got = scale_into_square(frame, width, height, from, ours, size);
    expected = reference_scale_into_square(frame, width, height, from,
                                           reference, size);
    if (got.scale != expected.scale || got.offset_x != expected.offset_x ||
        got.offset_y != expected.offset_y) {
        (void)fprintf(stderr,
                      "placement differs for %s: rect %d,%d %dx%d size %d\n",
                      what, from->x, from->y, from->w, from->h, size);
        return false;
    }
    if (memcmp(ours, reference, bytes) != 0) {
        (void)fprintf(stderr,
                      "pixels differ for %s: rect %d,%d %dx%d size %d\n",
                      what, from->x, from->y, from->w, from->h, size);
        return false;
    }
    return true;
}

static kod_rect rect(int x, int y, int w, int h)
{
    kod_rect out;

    out.x = x;
    out.y = y;
    out.w = w;
    out.h = h;
    return out;
}

static bool test_letterboxed_whole_frames(void)
{
    static const int sizes[] = {64, 128, 320, 321};
    uint8_t *frame = malloc((size_t)FRAME_W * (size_t)FRAME_W * 4u);

    CHECK(frame != NULL);
    for (size_t i = 0u; i < (size_t)FRAME_W * (size_t)FRAME_W * 4u; i++) {
        frame[i] = (uint8_t)rng();
    }
    for (size_t i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const kod_rect landscape = rect(0, 0, FRAME_W, FRAME_H);
        const kod_rect portrait = rect(0, 0, FRAME_H, FRAME_W);

        CHECK(agrees(frame, FRAME_W, FRAME_H, &landscape, sizes[i],
                     "a landscape frame"));
        CHECK(agrees(frame, FRAME_H, FRAME_W, &portrait, sizes[i],
                     "a portrait frame"));
    }
    free(frame);
    return true;
}

static bool test_the_awkward_rectangles(void)
{
    const kod_rect cases[] = {
        rect(100, 80, 160, 160),        /* square, inside */
        rect(200, 150, 100, 33),        /* wide and shallow */
        rect(50, 40, 3, 5),             /* tiny: every column upscaled */
        rect(320, 180, 1, 1),           /* a single pixel */
        rect(0, 0, FRAME_W, FRAME_H),   /* exactly the frame */
        rect(-40, 50, 200, 200),        /* off the left edge */
        rect(50, -40, 200, 200),        /* off the top */
        rect(560, 10, 200, 200),        /* off the right */
        rect(500, 250, 200, 200),       /* off the bottom */
        rect(-50, -50, 800, 500),       /* past every edge at once */
        rect(-500, -500, 100, 100),     /* entirely outside */
        rect(700, 400, 50, 50)          /* entirely outside, the other way */
    };
    uint8_t *frame = malloc((size_t)FRAME_W * (size_t)FRAME_H * 4u);

    CHECK(frame != NULL);
    for (size_t i = 0u; i < (size_t)FRAME_W * (size_t)FRAME_H * 4u; i++) {
        frame[i] = (uint8_t)rng();
    }
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(agrees(frame, FRAME_W, FRAME_H, &cases[i], 128, "an edge case"));
        CHECK(agrees(frame, FRAME_W, FRAME_H, &cases[i], 320, "an edge case"));
    }
    free(frame);
    return true;
}

static bool test_a_crowd_of_random_rectangles(void)
{
    static const int sizes[] = {64, 96, 128, 320};
    uint8_t *frame = malloc((size_t)FRAME_W * (size_t)FRAME_H * 4u);

    CHECK(frame != NULL);
    for (size_t i = 0u; i < (size_t)FRAME_W * (size_t)FRAME_H * 4u; i++) {
        frame[i] = (uint8_t)rng();
    }
    for (int i = 0; i < 400; i++) {
        const kod_rect from = rect(
            rng_between(-100, FRAME_W + 60), rng_between(-100, FRAME_H + 60),
            rng_between(1, 500), rng_between(1, 500));
        const int size = sizes[rng() % 4u];

        CHECK(agrees(frame, FRAME_W, FRAME_H, &from, size,
                     "a random rectangle"));
    }
    free(frame);
    return true;
}

int main(void)
{
    const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        {"letterboxed whole frames", test_letterboxed_whole_frames},
        {"the awkward rectangles", test_the_awkward_rectangles},
        {"a crowd of random rectangles", test_a_crowd_of_random_rectangles}
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

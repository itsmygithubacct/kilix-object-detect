#ifndef KILIX_OBJECT_DETECT_H
#define KILIX_OBJECT_DETECT_H

/*
 * What is in the picture, and only where something moved.
 *
 * Two halves that are usually written separately and should not be.  The
 * first is the detector itself, as a supervised subprocess behind a
 * fixed-size pipe: this library links no ML runtime and knows nothing
 * about accelerators, so where inference happens - this machine, a
 * virtualenv, a box with a GPU over ssh - is a launch detail.
 *
 * The second is the part that makes it affordable, and it is the reason
 * this is a module rather than a script.  A detector run on whole frames
 * pays full price for every frame and sees a person forty pixels tall as
 * forty pixels; run on *crops around what moved*, it pays for the crops
 * and sees that person filling the frame it is given.  That is Frigate's
 * shape and it is worth copying exactly:
 *
 *     motion boxes  ->  kod_regions()  ->  square crops
 *                                             |
 *                                       kod_detect_regions()
 *                                             |
 *                                       boxes, in frame coordinates
 *
 * It also closes a hole that whole-frame detection cannot: a zone the
 * operator marked as one to ignore stops costing detector time, because
 * a crop is never taken from it.  Ignoring a region *after* the model has
 * already looked at it saves nothing.
 *
 * Dependencies: C11, POSIX, and the `ffmpeg` binary if you use the source
 * helpers.  The detector command is spawned; nothing is linked.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KILIX_OBJECT_DETECT_VERSION_MAJOR 0
#define KILIX_OBJECT_DETECT_VERSION_MINOR 1
#define KILIX_OBJECT_DETECT_VERSION_PATCH 0

/*
 * The contract, shared with the sound classifier on purpose so that one
 * reader handles both replies: float32[20][6], rows of
 * [class, score, y0, x0, y1, x1] with coordinates normalised 0-1.  Every
 * detector plugin the reference implementations ship produces exactly
 * that, which is small enough to be a pipe protocol rather than an RPC.
 */
#define KOD_REPLY_ROWS 20
#define KOD_REPLY_COLUMNS 6
#define KOD_REPLY_BYTES \
    (KOD_REPLY_ROWS * KOD_REPLY_COLUMNS * (int)sizeof(float))

/* Boxes returned in one call, across every region. */
#define KOD_BOX_MAX 32
/* Crops taken from one frame.  Beyond this the smallest are dropped and
 * counted, because an unbounded region list is an unbounded frame cost. */
#define KOD_REGION_MAX 8

typedef struct kod_rect {
    int x;
    int y;
    int w;
    int h;
} kod_rect;

typedef struct kod_box {
    int class_id;
    float score;
    kod_rect at;      /* frame pixels, never crop pixels */
    /* Which crop found it, or -1 for a whole-frame detection.  Useful
     * when tuning: a subject found only in the widest region is a subject
     * the tighter crops cut in half. */
    int region;
} kod_box;

/*
 * The classes this reports, by COCO id.
 *
 * An allowlist, not a threshold, is the defence against nonsense: on
 * still footage the models invented toilets, birds and aeroplanes, and
 * filtering by name is what makes a low confidence threshold safe.  0.25
 * is measured; 0.45 demonstrably dropped real people in infrared.
 */
const char *kod_label(int class_id);
int kod_class_from_name(const char *name);
/* Enumerate the allowlist: index 0.. , NULL past the end.  `class_id` may
 * be NULL. */
const char *kod_class_at(size_t index, int *class_id);

/* ------------------------------- regions -------------------------------- */

/*
 * Motion boxes into detector-sized crops.
 *
 * Overlapping and nearby boxes are merged, each result is squared and
 * padded outwards, clamped into the frame, and grown to at least
 * `min_size` - a crop smaller than the model's input is upscaled noise,
 * and a crop that hugs the motion exactly cuts off the head of whatever
 * cast it.
 *
 * Returns how many regions were written.  `dropped`, when not NULL,
 * receives how many were discarded for exceeding the capacity: silent
 * truncation here would read as "nothing else moved".
 */
size_t kod_regions(
    const kod_rect *motion, size_t count, int frame_width, int frame_height,
    int min_size, kod_rect *out, size_t capacity, size_t *dropped);

/* ------------------------------- detector ------------------------------- */

typedef struct kod_detector kod_detector;

typedef struct kod_options {
    /*
     * The detector command, argv-style and NULL-terminated.
     *
     * NULL means: whatever KILIX_OBJECT_DETECTOR says, else the bundled
     * `kilix-look-detect` - beside this program if it travels with it,
     * else in the checkout's tools/, else on PATH.
     */
    const char *const *argv;

    /*
     * The square the model is fed, in pixels.  Default 320.
     *
     * Fixed for the life of the process, because that is what makes the
     * pipe self-framing.  Crops are scaled into it; a whole frame is
     * letterboxed into it so nothing is stretched.
     */
    int size;

    /* Below this a row is dropped.  Default 0.25 - see the note on the
     * allowlist above for why that is not reckless. */
    float min_score;

    /* Seconds to wait for a reply before deciding the detector has
     * wedged.  Default 5. */
    int timeout_seconds;

    /*
     * Seconds allowed for the *first* reply.  Default 90.
     *
     * Loading a model is not wedging.  A cold detector spends tens of
     * seconds importing a framework and reading weights off disk, and a
     * five-second timeout turns that into a permanent failure on every
     * cold start - which looks exactly like "the detector never ran" and
     * took a live run to notice.
     */
    int warmup_seconds;

    /* Where the detector's stderr goes, or NULL to leave it alone.  A
     * text-mode caller wants "no model; run kilix-look model" on screen;
     * a caller drawing a full-screen view wants it in a file, because one
     * stray line into the alternate screen corrupts the display. */
    const char *log_path;
} kod_options;

void kod_options_init(kod_options *options);

bool kod_open(kod_detector **detector, const kod_options *options);
void kod_close(kod_detector *detector);

/* The last failure as a short phrase, or NULL.  Never contains a URL. */
const char *kod_error(const kod_detector *detector);

/*
 * The whole frame, letterboxed into the model's square.
 *
 * For a still image, or for a caller with no motion gate.  Honest about
 * its cost: this is the expensive way, and on a 640x360 frame it sees a
 * distant subject at about a tenth of the model's input.
 */
bool kod_detect(
    kod_detector *detector, const uint8_t *bgra, int width, int height,
    kod_box *out, size_t capacity, size_t *count);

/*
 * Only where something moved.
 *
 * One inference per region, coordinates mapped back to the frame, and
 * duplicates across overlapping crops merged by overlap within a class -
 * the same car seen by two neighbouring crops is one car.
 */
bool kod_detect_regions(
    kod_detector *detector, const uint8_t *bgra, int width, int height,
    const kod_rect *regions, size_t region_count, kod_box *out,
    size_t capacity, size_t *count);

/* How many crops have been run since opening: the number that says what
 * the motion gate is actually saving. */
uint64_t kod_crops(const kod_detector *detector);

/* ------------------------------- drawing --------------------------------- */

/*
 * The colour a class is drawn in: people green, vehicles blue, animals
 * amber, the rest pink.  Three groups is what the eye reads at a glance,
 * where sixteen colours is a legend.
 *
 * Here rather than in a viewer because every program that draws these
 * boxes must draw them the same: a colour that means "person" in one
 * window and "car" in another is worse than no colour.
 */
uint32_t kod_class_colour(int class_id);

/*
 * Outline boxes onto a BGRA frame in place.
 *
 * Outlines only, and no text: labels need a font, and a font would put a
 * rasteriser in a library whose whole claim is C11 and POSIX.  The
 * outline and its colour are the part that has to agree between callers;
 * where the label sits is presentation.
 */
void kod_draw_boxes(
    uint8_t *bgra, int width, int height, const kod_box *boxes, size_t count);

/* The crops the detector was given, dimmer than the detections: "why did
 * it not see that" is usually answered by where the crops were. */
void kod_draw_regions(
    uint8_t *bgra, int width, int height, const kod_rect *regions,
    size_t count);

/*
 * Where a bundled tool is, preferring one that travels with this program:
 * beside the executable, then the checkout's tools/, then PATH.  Public
 * because everything that spawns one of this project's helpers needs the
 * same answer, and two implementations of it would drift.
 */
void kod_bundled_tool(const char *name, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* KILIX_OBJECT_DETECT_H */

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
#define KILIX_OBJECT_DETECT_VERSION_MINOR 2
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
/*
 * Crops in flight at once.  kod_regions() keeps a frame within this by
 * dropping the smallest and counting them, because an unbounded region
 * list is an unbounded frame cost.  A longer list handed straight to
 * kod_detect_regions() is still inferred in full, this many at a time;
 * kod_offer() takes only the first KOD_REGION_MAX, because a caller that
 * cannot wait cannot be handed a second round either - route the list
 * through kod_regions() to decide what a capped batch keeps.
 */
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

/* -------------------------------- the pipe ------------------------------- */

/*
 * The supervised subprocess on its own, for a sibling whose reply is not
 * the detector's.
 *
 * A face embedder answers float32[512]; a face detector answers the same
 * 480 bytes this detector does but means "face" by class 0.  What they
 * share is everything that is hard about a model in another process:
 * starting it and knowing why it did not start, a first reply that takes
 * a minute and later replies that take milliseconds, a deadline on the
 * whole read rather than on each piece of it, and closing a child that
 * will not stop.  That code was written once, here.  This is the door a
 * sibling uses instead of copying it.
 *
 * Fixed-size both ways.  The caller decides what a request and a reply
 * are; the pipe moves the bytes and keeps the time.
 */
#define KOD_ARGV_MAX 13
#define KOD_EXTRA_MAX 4

typedef struct kod_pipe kod_pipe;

typedef struct kod_pipe_options {
    /* The command, argv-style and NULL-terminated.  Required. */
    const char *const *argv;
    /* Appended after argv - "--geometry", "320x320" - or NULL.  At most
     * KOD_EXTRA_MAX words. */
    const char *const *extra;
    /* Seconds for a reply once the first has ever arrived.  Default 5,
     * at most a day. */
    int timeout_seconds;
    /* Seconds for the first reply.  Default 90; never less than
     * timeout_seconds, at most a day.  See kod_options for why it is
     * that long. */
    int warmup_seconds;
    /* Where the child's stderr goes, or NULL to leave it alone. */
    const char *log_path;
} kod_pipe_options;

void kod_pipe_options_init(kod_pipe_options *options);

/*
 * Which command to run: the words of `env_variable` if it is set and
 * usable, else the tool named `bundled`, found the way kod_bundled_tool()
 * finds it.  `storage` holds the strings `argv` points into and must
 * outlive them.  A variable that is set but unusable - too many words,
 * or too long - is refused whole rather than run truncated, and the
 * bundled tool is used instead.
 */
bool kod_pipe_resolve(
    const char *env_variable, const char *bundled, char *storage,
    size_t storage_size, const char **argv, size_t capacity);

/* False when the child never started; its stderr says why. */
bool kod_pipe_open(kod_pipe **pipe, const kod_pipe_options *options);
void kod_pipe_close(kod_pipe *pipe);

/* The last failure as a short phrase, or NULL.  Once set, the pipe is
 * broken and every call fails until it is closed. */
const char *kod_pipe_error(const kod_pipe *pipe);

/* One request, whole, bounded by the warm-up or steady-state allowance.
 * A successful send starts the reply clock. */
bool kod_pipe_send(kod_pipe *pipe, const uint8_t *bytes, size_t size);

/*
 * One reply, whole, waiting until the deadline the last send started:
 * the warm-up allowance before any reply has ever arrived, the timeout
 * after.  Bytes already in the pipe when the deadline passes still
 * count; a reply that trickles in does not restart the clock.  The
 * clock also starts at open, so a child that speaks first can be
 * received.  A child that closes its end fails at once, and
 * kod_pipe_exited() says so: a crash is a different fact from a slow
 * model, and the two send an operator to different places.
 */
bool kod_pipe_receive(kod_pipe *pipe, uint8_t *bytes, size_t size);
bool kod_pipe_exited(const kod_pipe *pipe);

/* Both, for a caller that can wait. */
bool kod_pipe_call(
    kod_pipe *pipe, const uint8_t *request, size_t request_size,
    uint8_t *reply, size_t reply_size);

/* For a caller that cannot wait: whether the reply has begun to arrive,
 * and whether the deadline has passed without it.  Neither blocks. */
bool kod_pipe_pending(const kod_pipe *pipe);
bool kod_pipe_overdue(const kod_pipe *pipe);

/* Whether any reply has ever arrived: what "did not answer" means
 * depends on it. */
bool kod_pipe_warm(const kod_pipe *pipe);

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

    /*
     * Report every class the detector names, not only the allowlist.
     *
     * For a detector whose classes are not COCO's - a face detector
     * answering class 0 and meaning "face" - the allowlist would pass
     * that row as a person or drop it.  kod_label() still knows only
     * COCO, so a caller setting this labels the classes itself.  Default
     * false.
     */
    bool any_class;
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

/*
 * The same work, for a caller that cannot stop to wait.
 *
 * kod_detect_regions() blocks for as long as the model takes, which is
 * right for a still image and wrong for anything holding a camera open:
 * measured on a recorder running three cameras, inference in the frame
 * loop dropped decoding from 30 frames a second to under 5 - for every
 * camera, including the ones nothing was moving in.
 *
 * kod_offer() takes the crops and sends the first; it returns false when
 * a batch is already in flight, which is the ordinary answer and does not
 * set an error.  kod_take() reads a reply if one has arrived and never
 * waits for one; when it sets `done` the boxes are in `out` and the
 * detector is free again.  The crops are copied, so the frame they came
 * from may be released the moment kod_offer() returns.  At most
 * KOD_REGION_MAX regions are taken per offer - see the note at its
 * definition.
 */
bool kod_offer(
    kod_detector *detector, const uint8_t *bgra, int width, int height,
    const kod_rect *regions, size_t region_count);
bool kod_busy(const kod_detector *detector);
bool kod_take(
    kod_detector *detector, kod_box *out, size_t capacity, size_t *count,
    bool *done);

/* How many crops have been run since opening: the number that says what
 * the motion gate is actually saving. */
uint64_t kod_crops(const kod_detector *detector);

/* ------------------------------- the pixels ------------------------------ */

/* Where a source rectangle landed inside a square: source pixels per
 * square pixel, and where the image starts inside it.  Undoing a box is
 * minus the offsets, times the scale, plus the rectangle's own origin,
 * in that order and in one place. */
typedef struct kod_fit {
    float scale;
    int offset_x;
    int offset_y;
} kod_fit;

/*
 * A source rectangle into a BGRA square, by area averaging, letterboxed.
 *
 * Public because every model in this family is fed a square cut from a
 * frame and two ways of cutting one drift: the face embedder must be
 * given the same pixels the face detector was.  `square` holds
 * size*size*4 bytes and is fully written.  Area averaging rather than
 * point sampling, and that is not a detail: the motion module measured
 * point sampling reporting movement on every frame of a still scene
 * where averaging reported a quarter.  A degenerate rectangle or an
 * absurd size yields a black square at scale 1.
 */
kod_fit kod_scale_into_square(
    const uint8_t *bgra, int width, int height, const kod_rect *from,
    uint8_t *square, int size);

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

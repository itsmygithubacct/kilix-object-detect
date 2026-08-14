/*
 * kilix-look — what is in a picture, a recording, or a camera right now.
 *
 * The library is the useful part; this is the way in.  It is also the
 * only place in this family where seeing and hearing happen at once:
 * `--listen` runs the sound classifier on the same source and draws its
 * classes under the picture, because a bark and a dog in the same second
 * is a different fact from either one alone.
 */

#include "kilix_object_detect.h"
#include "kod_app.h"
#include "kod_ui.h"

#include "kilix_motion_detect.h"
#include "kilix_rtsp.h"
#include "kilix_sound_detect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FETCH_TOOL "kilix-look-fetch-model"
#define MOTION_HEIGHT 100

static int usage(FILE *stream)
{
    (void)fprintf(
        stream,
        "usage: kilix-look <command> [options]\n"
        "\n"
        "  image <file>            what is in one picture\n"
        "  scan <source>           a line per frame that had something\n"
        "  watch <source>          the picture, with what was found on it\n"
        "  classes                 what it will report\n"
        "  --selftest              check this build end to end\n"
        "\n"
        "A source is an image, a recording, an rtsp:// camera - anything\n"
        "ffmpeg can open.  A recording is paced like the camera that shot\n"
        "it, so pointing this at footage behaves like pointing it at the\n"
        "thing that recorded it.\n"
        "\n"
        "  --listen        hear as well as look (watch, scan)\n"
        "  --hear F        what counts as a sound, default 0.50\n"
        "  --seconds N     stop after this long\n"
        "  --threshold F   what counts as a detection, default 0.25\n"
        "  --size N        the square the model is fed, default 320\n"
        "  --whole-frame   detect on whole frames instead of motion crops\n"
        "  --render FILE   write the first frame that found something\n"
        "  --regions       draw the crops the detector was given\n"
        "  --decode WxH    decode size, default 640x360 (letterboxed)\n"
        "  --fast          read a recording as fast as it decodes\n");
    return 2;
}

typedef struct settings {
    const char *render;
    float threshold;
    float sound_threshold;
    int seconds;
    int size;
    int decode_width;
    int decode_height;
    bool listen;
    bool whole_frame;
    bool regions;
    bool fast;
} settings;

static void settings_init(settings *out)
{
    (void)memset(out, 0, sizeof(*out));
    out->threshold = 0.25f;
    out->sound_threshold = 0.5f;
    out->size = 320;
    out->decode_width = 640;
    out->decode_height = 360;
}

static bool parse(settings *out, int argc, char **argv, int from)
{
    for (int i = from; i < argc; i++) {
        if (strcmp(argv[i], "--render") == 0 && i + 1 < argc) {
            out->render = argv[++i];
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            out->seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            out->threshold = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            out->size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--listen") == 0) {
            out->listen = true;
        } else if (strcmp(argv[i], "--hear") == 0 && i + 1 < argc) {
            out->sound_threshold = (float)atof(argv[++i]);
            out->listen = true;
        } else if (strcmp(argv[i], "--whole-frame") == 0) {
            out->whole_frame = true;
        } else if (strcmp(argv[i], "--regions") == 0) {
            out->regions = true;
        } else if (strcmp(argv[i], "--fast") == 0) {
            out->fast = true;
        } else if (strcmp(argv[i], "--decode") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &out->decode_width,
                       &out->decode_height) != 2) {
                return false;
            }
        } else {
            return false;
        }
    }
    return out->threshold >= 0.0f && out->threshold <= 1.0f &&
           out->size >= 64 && out->size <= 2048 &&
           out->decode_width >= 64 && out->decode_width <= 4096 &&
           out->decode_height >= 64 && out->decode_height <= 4096;
}

static bool open_detector(kod_detector **detector, const settings *options,
                          bool quiet)
{
    kod_options detector_options;
    char log_path[KOD_PATH_MAX];

    kod_options_init(&detector_options);
    detector_options.size = options->size;
    detector_options.min_score = options->threshold;
    if (quiet && kod_paths_file(log_path, sizeof(log_path), "detect.log")) {
        /* A full-screen view owns the terminal, and one line from a model
         * loader corrupts it. */
        detector_options.log_path = log_path;
    }
    return kod_open(detector, &detector_options);
}

static void print_boxes(const kod_box *boxes, size_t count)
{
    for (size_t i = 0u; i < count; i++) {
        (void)printf("  %-10s %.2f  %dx%d at %d,%d%s\n",
                     kod_label(boxes[i].class_id), (double)boxes[i].score,
                     boxes[i].at.w, boxes[i].at.h, boxes[i].at.x,
                     boxes[i].at.y,
                     boxes[i].region >= 0 ? "" : "  (whole frame)");
    }
}

/* --------------------------------- image --------------------------------- */

static int command_image(const char *path, const settings *options)
{
    kod_detector *detector = NULL;
    kod_box boxes[KOD_BOX_MAX];
    size_t count = 0u;
    const char *reason = NULL;
    uint8_t *frame;
    int width = 0;
    int height = 0;
    int status = 1;

    frame = kod_grab(path, &width, &height, &reason);
    if (frame == NULL) {
        (void)fprintf(stderr, "kilix-look: %s: %s\n", path,
                      reason != NULL ? reason : "cannot read it");
        return 1;
    }
    (void)printf("%s: %dx%d\n", path, width, height);
    if (!open_detector(&detector, options, false)) {
        (void)fprintf(stderr, "kilix-look: no detector\n");
        free(frame);
        return 1;
    }
    /* A still has no motion to crop to, so the whole frame is the only
     * honest thing to look at. */
    if (kod_detect(detector, frame, width, height, boxes, KOD_BOX_MAX,
                   &count)) {
        if (count == 0u) {
            (void)printf("  nothing it reports\n");
        }
        print_boxes(boxes, count);
        status = 0;
    } else {
        (void)fprintf(stderr, "kilix-look: %s\n",
                      kod_error(detector) != NULL ? kod_error(detector)
                                                  : "the detector failed");
    }
    if (options->render != NULL && status == 0) {
        kod_draw_boxes(frame, width, height, boxes, count);
        kod_draw_labels(frame, width, height, boxes, count);
        if (kod_write_ppm(options->render, frame, width, height)) {
            (void)printf("%s\n", options->render);
        }
    }
    kod_close(detector);
    free(frame);
    return status;
}

/* ---------------------------- the moving ones ---------------------------- */

typedef struct pipeline {
    krtsp_source *source;
    kmd_detector *motion;
    kod_detector *detector;
    ksd_listener *sound;
    int width;
    int height;
    uint64_t frames;
    uint64_t motion_frames;
    /* What the source had read last time we looked.  Borrowing returns
     * the newest frame whether or not it is a new one, so a loop that
     * spins faster than the camera counts one frame many times - and then
     * differences a frame against itself and reports no motion. */
    uint64_t seen;
} pipeline;

static void pipeline_stop(pipeline *run)
{
    ksd_close(run->sound);
    kod_close(run->detector);
    kmd_detector_free(run->motion);
    krtsp_source_stop(run->source);
    (void)memset(run, 0, sizeof(*run));
}

static bool pipeline_start(pipeline *run, const char *source,
                           const settings *options, bool quiet)
{
    krtsp_source_options source_options;
    kmd_config motion_config;

    (void)memset(run, 0, sizeof(*run));
    krtsp_source_options_init(&source_options);
    source_options.pixfmt = KRTSP_PIXFMT_BGRA;
    /*
     * A fixed decode size, letterboxed.
     *
     * Not a limitation to work around: it means the geometry is known
     * before the first frame arrives, so buffers are sized once and the
     * read stays fixed-size.  Letterboxing rather than stretching keeps
     * boxes in the right place on a camera that is not 16:9.
     */
    source_options.width = options->decode_width;
    source_options.height = options->decode_height;
    source_options.letterbox = true;
    /* Scanning wants through the file; watching wants to see it. */
    source_options.realtime = !options->fast;
    if (!krtsp_source_start(&run->source, source, &source_options)) {
        return false;
    }
    run->width = source_options.width;
    run->height = source_options.height;
    kmd_config_init(&motion_config);
    motion_config.width = run->width;
    motion_config.height = run->height;
    motion_config.pixfmt = KMD_PIXFMT_BGRA;
    /* Differenced at about a hundred pixels tall, measured: 8x cheaper
     * than at the decode size and it only moves the smallest thing it can
     * see from four pixels to six. */
    motion_config.detect_height = MOTION_HEIGHT;
    if (!kmd_detector_create(&run->motion, &motion_config)) {
        pipeline_stop(run);
        return false;
    }
    if (!open_detector(&run->detector, options, quiet)) {
        pipeline_stop(run);
        return false;
    }
    if (options->listen) {
        ksd_options sound_options;
        char log_path[KOD_PATH_MAX];

        ksd_options_init(&sound_options);
        sound_options.min_score = 0.0f;   /* the caller thresholds */
        if (kod_paths_file(log_path, sizeof(log_path), "ffmpeg-audio.log")) {
            sound_options.log_path = log_path;
        }
        if (quiet &&
            kod_paths_file(log_path, sizeof(log_path), "classify.log")) {
            sound_options.classifier_log_path = log_path;
        }
        /* Its own ffmpeg, its own connection.  A second pull of audio
         * only is a fraction of the video one, and sharing would mean a
         * wedged audio model stopping the picture. */
        if (!ksd_open(&run->sound, source, &sound_options)) {
            (void)fprintf(stderr, "kilix-look: not listening; sight only\n");
        }
    }
    return true;
}

/*
 * One frame through the whole thing: motion, crops, detection.
 *
 * `boxes` and `regions` receive what was found and where it looked, so
 * both the text output and the overlay say the same thing.
 */
static bool pipeline_step(
    pipeline *run, const settings *options, const uint8_t **frame,
    kod_box *boxes, size_t *box_count, kod_rect *regions,
    size_t *region_count)
{
    const uint8_t *pixels;
    kmd_box motion[32];
    kod_rect moved[32];
    krtsp_source_stats stats;
    size_t moved_count;
    int age_ms = 0;

    *box_count = 0u;
    *region_count = 0u;
    krtsp_source_get_stats(run->source, &stats);
    if (stats.frames == run->seen) {
        return false;   /* nothing new since last time */
    }
    pixels = krtsp_source_borrow(run->source, &age_ms);
    if (pixels == NULL) {
        return false;
    }
    run->seen = stats.frames;
    run->frames++;
    *frame = pixels;
    moved_count = kmd_detect(run->motion, pixels, motion,
                             sizeof(motion) / sizeof(motion[0]), NULL);
    if (moved_count > 0u) {
        run->motion_frames++;
    }
    if (options->whole_frame) {
        (void)kod_detect(run->detector, pixels, run->width, run->height,
                         boxes, KOD_BOX_MAX, box_count);
        krtsp_source_release(run->source);
        return true;
    }
    for (size_t i = 0u; i < moved_count && i < 32u; i++) {
        moved[i].x = motion[i].x0;
        moved[i].y = motion[i].y0;
        moved[i].w = motion[i].x1 - motion[i].x0;
        moved[i].h = motion[i].y1 - motion[i].y0;
    }
    *region_count = kod_regions(moved, moved_count, run->width, run->height,
                                options->size, regions, KOD_REGION_MAX, NULL);
    if (*region_count > 0u) {
        (void)kod_detect_regions(run->detector, pixels, run->width,
                                 run->height, regions, *region_count, boxes,
                                 KOD_BOX_MAX, box_count);
    }
    krtsp_source_release(run->source);
    return true;
}

static size_t sound_step(pipeline *run, float *scores)
{
    ksd_event heard[KSD_CLASS_COUNT];
    size_t count = 0u;

    if (run->sound == NULL) {
        return 0u;
    }
    if (ksd_step(run->sound, heard, KSD_CLASS_COUNT, &count)) {
        const float *latest = ksd_scores(run->sound);

        if (latest != NULL) {
            (void)memcpy(scores, latest, sizeof(float) * KSD_CLASS_COUNT);
        }
    } else if (ksd_error(run->sound) != NULL) {
        ksd_close(run->sound);
        run->sound = NULL;
    }
    return KSD_CLASS_COUNT;
}

static int command_scan(const char *source, const settings *options)
{
    pipeline run;
    kod_box boxes[KOD_BOX_MAX];
    kod_rect regions[KOD_REGION_MAX];
    float sound[KSD_CLASS_COUNT];
    float sound_peak[KSD_CLASS_COUNT];
    char safe[256];
    const time_t deadline =
        options->seconds > 0 ? time(NULL) + options->seconds : 0;
    size_t total = 0u;
    bool complained = false;

    (void)memset(sound, 0, sizeof(sound));
    (void)memset(sound_peak, 0, sizeof(sound_peak));
    kod_redact(source, safe, sizeof(safe));
    (void)printf("looking at %s\n", safe);
    if (!pipeline_start(&run, source, options, false)) {
        (void)fprintf(stderr, "kilix-look: cannot open that source\n");
        return 1;
    }
    for (;;) {
        const uint8_t *frame = NULL;
        size_t box_count = 0u;
        size_t region_count = 0u;
        struct timespec pause = {0, 20 * 1000 * 1000};

        if (deadline > 0 && time(NULL) >= deadline) {
            break;
        }
        if (!pipeline_step(&run, options, &frame, boxes, &box_count, regions,
                           &region_count)) {
            (void)nanosleep(&pause, NULL);
            if (krtsp_source_status(run.source) == KRTSP_FAILED) {
                break;
            }
            continue;
        }
        if (kod_error(run.detector) != NULL && !complained) {
            /* Said once.  A detector that has broken stays broken, and a
             * line per frame would bury the run it broke during. */
            (void)fprintf(stderr, "kilix-look: %s\n",
                          kod_error(run.detector));
            complained = true;
        }
        if (box_count > 0u) {
            (void)printf("frame %llu: %zu region%s\n",
                         (unsigned long long)run.frames, region_count,
                         region_count == 1u ? "" : "s");
            print_boxes(boxes, box_count);
            total += box_count;
            (void)fflush(stdout);
        }
        if (run.sound != NULL) {
            (void)sound_step(&run, sound);
            for (size_t i = 0u; i < KSD_CLASS_COUNT; i++) {
                if (sound[i] > sound_peak[i]) {
                    sound_peak[i] = sound[i];
                }
                if (sound[i] >= options->sound_threshold) {
                    (void)printf("  heard %s %.2f\n", ksd_label((int)i),
                                 (double)sound[i]);
                    sound[i] = 0.0f;
                }
            }
        }
    }
    (void)printf("%llu frames, %llu with motion, %llu crops, %zu detections\n",
                 (unsigned long long)run.frames,
                 (unsigned long long)run.motion_frames,
                 (unsigned long long)kod_crops(run.detector), total);
    if (options->listen) {
        /* What the ear nearly said, always: "nothing over the threshold"
         * and "nothing at all" are different findings, and only one of
         * them means the audio path was working. */
        bool any = false;

        (void)printf("heard:");
        for (size_t i = 0u; i < KSD_CLASS_COUNT; i++) {
            if (sound_peak[i] >= 0.01f) {
                (void)printf(" %s %.2f", ksd_label((int)i),
                             (double)sound_peak[i]);
                any = true;
            }
        }
        (void)printf("%s\n", any ? "" : " nothing at all");
    }
    pipeline_stop(&run);
    return 0;
}

static int command_watch(const char *source, const settings *options)
{
    pipeline run;
    kod_ui *ui = NULL;
    kod_box boxes[KOD_BOX_MAX];
    kod_rect regions[KOD_REGION_MAX];
    float sound[KSD_CLASS_COUNT];
    const char *labels[KSD_CLASS_COUNT];
    uint8_t *copy = NULL;
    char safe[256];
    const time_t deadline =
        options->seconds > 0 ? time(NULL) + options->seconds : 0;
    bool rendered = false;
    bool headless = options->render != NULL;

    (void)memset(sound, 0, sizeof(sound));
    for (size_t i = 0u; i < KSD_CLASS_COUNT; i++) {
        labels[i] = ksd_label((int)i);
    }
    kod_redact(source, safe, sizeof(safe));
    if (!pipeline_start(&run, source, options, !headless)) {
        (void)fprintf(stderr, "kilix-look: cannot open that source\n");
        return 1;
    }
    /* --render is the way to see what this draws on a machine that cannot
     * show it, so it deliberately does not open a session at all. */
    if (!headless && !kod_ui_open(&ui)) {
        pipeline_stop(&run);
        return 1;
    }
    copy = malloc((size_t)run.width * (size_t)run.height * 4u);
    if (copy == NULL) {
        kod_ui_close(ui);
        pipeline_stop(&run);
        return 1;
    }
    for (;;) {
        const uint8_t *frame = NULL;
        size_t box_count = 0u;
        size_t region_count = 0u;
        struct timespec pause = {0, 20 * 1000 * 1000};
        kod_ui_status status;

        if (deadline > 0 && time(NULL) >= deadline) {
            break;
        }
        if (ui != NULL && kod_ui_key(ui) == 'q') {
            break;
        }
        if (!pipeline_step(&run, options, &frame, boxes, &box_count, regions,
                           &region_count)) {
            (void)nanosleep(&pause, NULL);
            if (krtsp_source_status(run.source) == KRTSP_FAILED) {
                break;
            }
            continue;
        }
        if (run.sound != NULL) {
            (void)sound_step(&run, sound);
        }
        (void)memcpy(copy, frame,
                     (size_t)run.width * (size_t)run.height * 4u);
        if (options->regions) {
            kod_draw_regions(copy, run.width, run.height, regions,
                             region_count);
        }
        kod_draw_boxes(copy, run.width, run.height, boxes, box_count);
        kod_draw_labels(copy, run.width, run.height, boxes, box_count);

        if (headless) {
            /* The first frame that found something: a picture of an empty
             * driveway proves only that the program starts. */
            if (box_count > 0u && !rendered) {
                if (kod_write_ppm(options->render, copy, run.width,
                                  run.height)) {
                    (void)printf("%s\n", options->render);
                    print_boxes(boxes, box_count);
                    rendered = true;
                }
                break;
            }
            continue;
        }
        (void)memset(&status, 0, sizeof(status));
        status.title = safe;
        status.frames = run.frames;
        status.motion_frames = run.motion_frames;
        status.crops = kod_crops(run.detector);
        status.boxes = box_count;
        status.regions = region_count;
        if (run.sound != NULL) {
            status.sound_scores = sound;
            status.sound_labels = labels;
            status.sound_count = KSD_CLASS_COUNT;
            status.sound_threshold = options->sound_threshold;
        }
        (void)kod_ui_present(ui, copy, run.width, run.height, &status);
    }
    free(copy);
    kod_ui_close(ui);
    if (headless && !rendered) {
        (void)fprintf(stderr, "kilix-look: nothing was found to render\n");
    }
    (void)fprintf(stderr,
                  "%llu frames, %llu with motion, %llu crops\n",
                  (unsigned long long)run.frames,
                  (unsigned long long)run.motion_frames,
                  (unsigned long long)kod_crops(run.detector));
    pipeline_stop(&run);
    return headless && !rendered ? 1 : 0;
}

/* -------------------------------- the rest ------------------------------- */

static int command_classes(void)
{
    int class_id = 0;

    (void)printf("%-4s %s\n", "COCO", "REPORTED AS");
    for (size_t i = 0u;; i++) {
        const char *label = kod_class_at(i, &class_id);

        if (label == NULL) {
            break;
        }
        (void)printf("%-4d %s\n", class_id, label);
    }
    (void)printf("\nAn allowlist, not a threshold: on still footage the "
                 "models invented\ntoilets and aeroplanes, and filtering by "
                 "name is what makes a low\nconfidence safe.\n");
    return 0;
}

#define TEST(name, condition)                                                 \
    do {                                                                      \
        const bool passed = (condition);                                      \
        (void)printf("%s %s\n", passed ? "ok" : "not ok", (name));            \
        if (!passed) {                                                        \
            return 1;                                                         \
        }                                                                     \
    } while (false)

/*
 * The render path's deliverable, byte for byte: a P6 header, then RGB
 * triplets top-left first with the alpha dropped.  Pinned here because a
 * PPM that is almost right opens fine in one viewer and not another,
 * and --render is how this program is checked on machines that cannot
 * show a window.
 */
static bool ppm_round_trips(void)
{
    static const uint8_t bgra[16] = {
        0x01, 0x02, 0x03, 0xFF,  0x11, 0x12, 0x13, 0x80,
        0x21, 0x22, 0x23, 0x00,  0xFF, 0x00, 0x7F, 0xFF
    };
    static const uint8_t expected[] = {
        'P', '6', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n',
        0x03, 0x02, 0x01,  0x13, 0x12, 0x11,
        0x23, 0x22, 0x21,  0x7F, 0x00, 0xFF
    };
    const char *tmpdir = getenv("TMPDIR");
    char path[512];
    uint8_t written[64];
    size_t got;
    FILE *file;

    (void)snprintf(path, sizeof(path), "%s/kilix-look-selftest-%ld.ppm",
                   tmpdir != NULL && tmpdir[0] != '\0' ? tmpdir : "/tmp",
                   (long)getpid());
    if (!kod_write_ppm(path, bgra, 2, 2)) {
        return false;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        (void)remove(path);
        return false;
    }
    got = fread(written, 1u, sizeof(written), file);
    (void)fclose(file);
    (void)remove(path);
    return got == sizeof(expected) &&
           memcmp(written, expected, sizeof(expected)) == 0;
}

static int selftest(void)
{
    char safe[256];
    kod_rect motion[4];
    kod_rect regions[KOD_REGION_MAX];
    size_t dropped = 0u;
    size_t count;

    TEST("classes round-trip",
         kod_class_from_name("person") == 0 &&
             strcmp(kod_label(2), "car") == 0 &&
             kod_class_from_name("toilet") < 0 && kod_label(61) == NULL);

    kod_redact("rtsp://someone:secret@192.0.2.10/stream", safe, sizeof(safe));
    TEST("a url is redacted",
         strstr(safe, "secret") == NULL && strstr(safe, "192.0.2.10") != NULL);

    motion[0].x = 100; motion[0].y = 100; motion[0].w = 20; motion[0].h = 40;
    count = kod_regions(motion, 1u, 640, 360, 320, regions, KOD_REGION_MAX,
                        &dropped);
    TEST("one motion box becomes one crop", count == 1u && dropped == 0u);
    TEST("the crop is square and at least the model's size",
         regions[0].w == regions[0].h && regions[0].w >= 320);
    TEST("and lies inside the frame",
         regions[0].x >= 0 && regions[0].y >= 0 &&
             regions[0].x + regions[0].w <= 640 &&
             regions[0].y + regions[0].h <= 360);

    motion[1].x = 105; motion[1].y = 110; motion[1].w = 20; motion[1].h = 40;
    count = kod_regions(motion, 2u, 640, 360, 320, regions, KOD_REGION_MAX,
                        NULL);
    TEST("two boxes on top of each other are one crop", count == 1u);

    motion[1].x = 600; motion[1].y = 300; motion[1].w = 20; motion[1].h = 40;
    count = kod_regions(motion, 2u, 1920, 1080, 320, regions, KOD_REGION_MAX,
                        NULL);
    TEST("two boxes far apart are two crops", count == 2u);

    TEST("a ppm is written byte for byte", ppm_round_trips());

    (void)printf("selftest passed\n");
    return 0;
}

int main(int argc, char **argv)
{
    settings options;
    const char *command;

    if (argc < 2) {
        return usage(stderr);
    }
    command = argv[1];
    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0 ||
        strcmp(command, "help") == 0) {
        (void)usage(stdout);
        return 0;
    }
    if (strcmp(command, "--selftest") == 0) {
        return selftest();
    }
    if (strcmp(command, "classes") == 0) {
        return command_classes();
    }
    settings_init(&options);
    if (argc < 3 || !parse(&options, argc, argv, 3)) {
        return usage(stderr);
    }
    if (strcmp(command, "image") == 0) {
        return command_image(argv[2], &options);
    }
    if (strcmp(command, "scan") == 0) {
        return command_scan(argv[2], &options);
    }
    if (strcmp(command, "watch") == 0) {
        return command_watch(argv[2], &options);
    }
    return usage(stderr);
}

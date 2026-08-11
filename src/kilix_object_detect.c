/*
 * A subprocess, a square, and the arithmetic between them.
 *
 * Everything hard here is coordinates.  A crop is taken from the frame,
 * scaled into the model's square, and the model answers in its own
 * normalised space; getting a box back into frame pixels means undoing
 * all three, and getting it wrong produces boxes that look plausible and
 * sit in the wrong place.  That failure has happened once already in this
 * family - rescaling boxes that were already in source coordinates
 * produced negative heights - so every step here is done in one function
 * and named.
 */

#include "kilix_object_detect.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ERROR_MAX 160
#define KOD_ARGV_MAX 13
#define KOD_DETECTOR_ENV "KILIX_OBJECT_DETECTOR"
#define KOD_DETECTOR_NAME "kilix-look-detect"

struct kod_detector {
    pid_t child;
    int to_child;
    int from_child;
    int size;
    float min_score;
    int timeout_ms;
    int warmup_ms;
    bool warm;
    bool broken;
    uint8_t *square;      /* size * size * 4, reused every call */
    uint64_t crops;
    char error[ERROR_MAX];
};

static const struct {
    int id;
    const char *label;
} ALLOWED[] = {
    {0, "person"},
    {1, "bicycle"},
    {2, "car"},
    {3, "motorcycle"},
    {5, "bus"},
    {7, "truck"},
    {14, "bird"},
    {15, "cat"},
    {16, "dog"},
    {17, "horse"},
    {18, "sheep"},
    {19, "cow"},
    {21, "bear"},
    {24, "backpack"},
    {26, "handbag"},
    {28, "suitcase"}
};

#define ALLOWED_COUNT (sizeof(ALLOWED) / sizeof(ALLOWED[0]))

const char *kod_label(int class_id)
{
    for (size_t i = 0u; i < ALLOWED_COUNT; i++) {
        if (ALLOWED[i].id == class_id) {
            return ALLOWED[i].label;
        }
    }
    return NULL;
}

int kod_class_from_name(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (size_t i = 0u; i < ALLOWED_COUNT; i++) {
        if (strcmp(ALLOWED[i].label, name) == 0) {
            return ALLOWED[i].id;
        }
    }
    return -1;
}

const char *kod_class_at(size_t index, int *class_id)
{
    if (index >= ALLOWED_COUNT) {
        return NULL;
    }
    if (class_id != NULL) {
        *class_id = ALLOWED[index].id;
    }
    return ALLOWED[index].label;
}

static bool fail(kod_detector *detector, const char *reason)
{
    if (detector != NULL) {
        (void)snprintf(detector->error, sizeof(detector->error), "%s", reason);
        detector->broken = true;
    }
    return false;
}

const char *kod_error(const kod_detector *detector)
{
    if (detector == NULL || detector->error[0] == '\0') {
        return NULL;
    }
    return detector->error;
}

void kod_options_init(kod_options *options)
{
    if (options == NULL) {
        return;
    }
    (void)memset(options, 0, sizeof(*options));
    options->size = 320;
    options->min_score = 0.25f;
    options->timeout_seconds = 5;
    options->warmup_seconds = 90;
}

/* ------------------------------- regions -------------------------------- */

static bool touching(const kod_rect *a, const kod_rect *b, int slack)
{
    return a->x - slack < b->x + b->w && b->x - slack < a->x + a->w &&
           a->y - slack < b->y + b->h && b->y - slack < a->y + a->h;
}

static void absorb(kod_rect *into, const kod_rect *other)
{
    const int x1 = into->x + into->w > other->x + other->w
                       ? into->x + into->w : other->x + other->w;
    const int y1 = into->y + into->h > other->y + other->h
                       ? into->y + into->h : other->y + other->h;

    if (other->x < into->x) { into->x = other->x; }
    if (other->y < into->y) { into->y = other->y; }
    into->w = x1 - into->x;
    into->h = y1 - into->y;
}

static int compare_area(const void *left, const void *right)
{
    const kod_rect *a = left;
    const kod_rect *b = right;
    const long area_a = (long)a->w * (long)a->h;
    const long area_b = (long)b->w * (long)b->h;

    if (area_a < area_b) { return 1; }
    if (area_a > area_b) { return -1; }
    return 0;
}

size_t kod_regions(
    const kod_rect *motion, size_t count, int frame_width, int frame_height,
    int min_size, kod_rect *out, size_t capacity, size_t *dropped)
{
    kod_rect merged[KOD_REGION_MAX * 4];
    size_t merged_count = 0u;
    size_t written = 0u;

    if (dropped != NULL) {
        *dropped = 0u;
    }
    if (motion == NULL || out == NULL || capacity == 0u || count == 0u ||
        frame_width <= 0 || frame_height <= 0) {
        return 0u;
    }
    if (min_size <= 0) {
        min_size = 320;
    }

    /* Merge what overlaps or nearly does.  The slack is deliberate: two
     * boxes a few pixels apart are one thing seen through a gap in the
     * foliage, and two crops of it cost twice and agree with each other
     * about half the time. */
    for (size_t i = 0u; i < count; i++) {
        kod_rect candidate = motion[i];
        bool absorbed = false;

        if (candidate.w <= 0 || candidate.h <= 0) {
            continue;
        }
        for (size_t j = 0u; j < merged_count; j++) {
            if (touching(&merged[j], &candidate, 16)) {
                absorb(&merged[j], &candidate);
                absorbed = true;
                break;
            }
        }
        if (!absorbed && merged_count < sizeof(merged) / sizeof(merged[0])) {
            merged[merged_count++] = candidate;
        }
    }
    /* Merging can bring two groups into contact, so settle it. */
    for (bool again = true; again;) {
        again = false;
        for (size_t i = 0u; i < merged_count && !again; i++) {
            for (size_t j = i + 1u; j < merged_count; j++) {
                if (touching(&merged[i], &merged[j], 16)) {
                    absorb(&merged[i], &merged[j]);
                    merged[j] = merged[merged_count - 1u];
                    merged_count--;
                    again = true;
                    break;
                }
            }
        }
    }

    /* Biggest first, so a capacity cut keeps what matters. */
    qsort(merged, merged_count, sizeof(merged[0]), compare_area);

    for (size_t i = 0u; i < merged_count; i++) {
        kod_rect region = merged[i];
        int side;
        int centre_x;
        int centre_y;

        if (written == capacity) {
            if (dropped != NULL) {
                (*dropped)++;
            }
            continue;
        }
        /* A fifth again as wide as what moved: a box drawn tight around
         * the moving pixels of a person is a box around their legs. */
        side = (region.w > region.h ? region.w : region.h) * 6 / 5;
        if (side < min_size) {
            side = min_size;
        }
        if (side > frame_width) { side = frame_width; }
        if (side > frame_height) { side = frame_height; }

        centre_x = region.x + region.w / 2;
        centre_y = region.y + region.h / 2;
        region.x = centre_x - side / 2;
        region.y = centre_y - side / 2;
        region.w = side;
        region.h = side;
        /* Shifted into the frame rather than shrunk: a square that leaves
         * the edge stays square, which is what the model was given. */
        if (region.x < 0) { region.x = 0; }
        if (region.y < 0) { region.y = 0; }
        if (region.x + side > frame_width) {
            region.x = frame_width - side;
        }
        if (region.y + side > frame_height) {
            region.y = frame_height - side;
        }
        out[written++] = region;
    }
    return written;
}

/* ------------------------------ the process ------------------------------ */

void kod_bundled_tool(const char *name, char *out, size_t size)
{
    char self[1024];
    ssize_t length;
    char *slash;
    char beside[1200];
    char in_tools[1200];

    (void)snprintf(out, size, "%s", name);
    length = readlink("/proc/self/exe", self, sizeof(self) - 1u);
    if (length <= 0) {
        return;
    }
    self[length] = '\0';
    slash = strrchr(self, '/');
    if (slash == NULL) {
        return;
    }
    *slash = '\0';
    (void)snprintf(beside, sizeof(beside), "%s/%s", self, name);
    (void)snprintf(in_tools, sizeof(in_tools), "%s/../tools/%s", self, name);
    if (access(beside, X_OK) == 0 && strlen(beside) < size) {
        (void)snprintf(out, size, "%s", beside);
    } else if (access(in_tools, X_OK) == 0 && strlen(in_tools) < size) {
        (void)snprintf(out, size, "%s", in_tools);
    }
}

static bool argv_from_env(const char *variable, char *storage,
                          size_t storage_size, const char **argv,
                          size_t capacity)
{
    const char *value = getenv(variable);
    size_t count = 0u;
    char *at;

    if (value == NULL || value[0] == '\0' || strlen(value) >= storage_size) {
        return false;
    }
    (void)snprintf(storage, storage_size, "%s", value);
    at = storage;
    while (*at != '\0') {
        while (*at == ' ' || *at == '\t') {
            at++;
        }
        if (*at == '\0') {
            break;
        }
        if (count + 1u >= capacity) {
            return false;   /* refused whole, never truncated */
        }
        argv[count++] = at;
        while (*at != '\0' && *at != ' ' && *at != '\t') {
            at++;
        }
        if (*at != '\0') {
            *at++ = '\0';
        }
    }
    if (count == 0u) {
        return false;
    }
    argv[count] = NULL;
    return true;
}

bool kod_open(kod_detector **out, const kod_options *options)
{
    kod_options defaults;
    const char *env_argv[KOD_ARGV_MAX + 1];
    const char *fallback[2];
    char env_storage[512];
    char bundled[1024];
    const char *const *chosen;
    kod_detector *detector;
    int to_child[2];
    int from_child[2];
    char geometry[32];

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    if (options == NULL) {
        kod_options_init(&defaults);
        options = &defaults;
    }
    if (options->size <= 0 || options->size > 4096) {
        return false;
    }
    detector = calloc(1u, sizeof(*detector));
    if (detector == NULL) {
        return false;
    }
    detector->size = options->size;
    detector->min_score = options->min_score;
    detector->timeout_ms =
        (options->timeout_seconds > 0 ? options->timeout_seconds : 5) * 1000;
    detector->warmup_ms =
        (options->warmup_seconds > 0 ? options->warmup_seconds : 90) * 1000;
    if (detector->warmup_ms < detector->timeout_ms) {
        detector->warmup_ms = detector->timeout_ms;
    }
    detector->to_child = -1;
    detector->from_child = -1;
    detector->square = malloc((size_t)detector->size *
                              (size_t)detector->size * 4u);
    if (detector->square == NULL) {
        free(detector);
        return false;
    }

    kod_bundled_tool(KOD_DETECTOR_NAME, bundled, sizeof(bundled));
    fallback[0] = bundled;
    fallback[1] = NULL;
    chosen = options->argv;
    if (chosen == NULL &&
        argv_from_env(KOD_DETECTOR_ENV, env_storage, sizeof(env_storage),
                      env_argv, KOD_ARGV_MAX + 1u)) {
        chosen = env_argv;
    }
    if (chosen == NULL) {
        chosen = fallback;
    }
    {
        size_t words = 0u;

        while (chosen[words] != NULL) {
            words++;
        }
        if (words == 0u || words > KOD_ARGV_MAX) {
            free(detector->square);
            free(detector);
            return false;
        }
    }
    if (pipe(to_child) != 0) {
        free(detector->square);
        free(detector);
        return false;
    }
    if (pipe(from_child) != 0) {
        (void)close(to_child[0]); (void)close(to_child[1]);
        free(detector->square);
        free(detector);
        return false;
    }
    (void)snprintf(geometry, sizeof(geometry), "%dx%d", detector->size,
                   detector->size);
    detector->child = fork();
    if (detector->child < 0) {
        (void)close(to_child[0]); (void)close(to_child[1]);
        (void)close(from_child[0]); (void)close(from_child[1]);
        free(detector->square);
        free(detector);
        return false;
    }
    if (detector->child == 0) {
        char *child_argv[KOD_ARGV_MAX + 3];
        size_t count = 0u;

        (void)dup2(to_child[0], STDIN_FILENO);
        (void)dup2(from_child[1], STDOUT_FILENO);
        (void)close(to_child[0]); (void)close(to_child[1]);
        (void)close(from_child[0]); (void)close(from_child[1]);
        if (options->log_path != NULL) {
            const int log = open(options->log_path,
                                 O_WRONLY | O_CREAT | O_APPEND, 0600);

            if (log >= 0) {
                (void)dup2(log, STDERR_FILENO);
                (void)close(log);
            }
        }
        while (chosen[count] != NULL && count < KOD_ARGV_MAX) {
            child_argv[count] = (char *)chosen[count];
            count++;
        }
        child_argv[count++] = (char *)"--geometry";
        child_argv[count++] = geometry;
        child_argv[count] = NULL;
        (void)execvp(child_argv[0], child_argv);
        _exit(127);
    }
    (void)close(to_child[0]);
    (void)close(from_child[1]);
    detector->to_child = to_child[1];
    detector->from_child = from_child[0];
    (void)signal(SIGPIPE, SIG_IGN);
    *out = detector;
    return true;
}

void kod_close(kod_detector *detector)
{
    int status;

    if (detector == NULL) {
        return;
    }
    if (detector->to_child >= 0) { (void)close(detector->to_child); }
    if (detector->from_child >= 0) { (void)close(detector->from_child); }
    if (detector->child > 0) {
        (void)kill(detector->child, SIGTERM);
        (void)waitpid(detector->child, &status, 0);
    }
    free(detector->square);
    free(detector);
}

uint64_t kod_crops(const kod_detector *detector)
{
    return detector == NULL ? 0u : detector->crops;
}

/* ------------------------------- the pixels ------------------------------ */

/*
 * A source rectangle into the model's square, by area averaging.
 *
 * Area averaging rather than point sampling, and that is not a detail:
 * the motion module measured point sampling reporting movement on 100% of
 * frames of a still scene where averaging reported 24%.  A detector fed
 * aliased pixels is being asked a different question than the one the
 * camera answered.
 *
 * `pad` letterboxes: the source is fitted inside the square preserving
 * aspect and the remainder is left black, so nothing is stretched.  The
 * scale and offsets used are handed back for undoing afterwards.
 */
typedef struct fit {
    float scale;      /* source pixels per square pixel */
    int offset_x;     /* where the image starts inside the square */
    int offset_y;
} fit;

static fit scale_into_square(
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

static bool write_all(int fd, const uint8_t *bytes, size_t size)
{
    size_t offset = 0u;

    while (offset < size) {
        const ssize_t put = write(fd, bytes + offset, size - offset);

        if (put > 0) {
            offset += (size_t)put;
            continue;
        }
        if (put < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static bool read_all(int fd, uint8_t *bytes, size_t size, int timeout_ms)
{
    size_t offset = 0u;

    while (offset < size) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1u, timeout_ms);
        ssize_t got;

        if (ready <= 0) {
            return false;
        }
        got = read(fd, bytes + offset, size - offset);
        if (got > 0) {
            offset += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

/*
 * One crop through the model, with every coordinate transform undone.
 *
 * normalised square -> square pixels -> minus the letterbox -> times the
 * scale -> plus the region's own origin.  Four steps, one place.
 */
static bool run_one(
    kod_detector *detector, const uint8_t *bgra, int width, int height,
    const kod_rect *region, int region_index, kod_box *out, size_t capacity,
    size_t *written)
{
    uint8_t reply[KOD_REPLY_BYTES];
    float rows[KOD_REPLY_ROWS][KOD_REPLY_COLUMNS];
    const size_t bytes = (size_t)detector->size * (size_t)detector->size * 4u;
    fit placement;

    placement = scale_into_square(bgra, width, height, region,
                                  detector->square, detector->size);
    if (!write_all(detector->to_child, detector->square, bytes)) {
        return fail(detector, "the detector stopped reading");
    }
    if (!read_all(detector->from_child, reply, sizeof(reply),
                  detector->warm ? detector->timeout_ms
                                 : detector->warmup_ms)) {
        return fail(detector, detector->warm
                                  ? "the detector did not answer in time"
                                  : "the detector never started (no model?)");
    }
    detector->warm = true;
    detector->crops++;
    (void)memcpy(rows, reply, sizeof(rows));
    for (size_t i = 0u; i < KOD_REPLY_ROWS && *written < capacity; i++) {
        const int class_id = (int)rows[i][0];
        const float score = rows[i][1];
        float y0 = rows[i][2];
        float x0 = rows[i][3];
        float y1 = rows[i][4];
        float x1 = rows[i][5];
        kod_box *box;

        if (score < detector->min_score) {
            continue;
        }
        if (kod_label(class_id) == NULL) {
            continue;
        }
        /* Normalised, in the square this crop was drawn into. */
        x0 = x0 * (float)detector->size - (float)placement.offset_x;
        x1 = x1 * (float)detector->size - (float)placement.offset_x;
        y0 = y0 * (float)detector->size - (float)placement.offset_y;
        y1 = y1 * (float)detector->size - (float)placement.offset_y;

        box = &out[*written];
        box->class_id = class_id;
        box->score = score;
        box->region = region_index;
        box->at.x = region->x + (int)(x0 * placement.scale);
        box->at.y = region->y + (int)(y0 * placement.scale);
        box->at.w = (int)((x1 - x0) * placement.scale);
        box->at.h = (int)((y1 - y0) * placement.scale);
        if (box->at.w <= 0 || box->at.h <= 0) {
            continue;   /* a degenerate row is not a detection */
        }
        /* Clamped into the frame: a box mapped out of a letterboxed crop
         * can land a pixel or two outside it. */
        if (box->at.x < 0) { box->at.w += box->at.x; box->at.x = 0; }
        if (box->at.y < 0) { box->at.h += box->at.y; box->at.y = 0; }
        if (box->at.x + box->at.w > width) {
            box->at.w = width - box->at.x;
        }
        if (box->at.y + box->at.h > height) {
            box->at.h = height - box->at.y;
        }
        if (box->at.w <= 0 || box->at.h <= 0) {
            continue;
        }
        (*written)++;
    }
    return true;
}

bool kod_detect(
    kod_detector *detector, const uint8_t *bgra, int width, int height,
    kod_box *out, size_t capacity, size_t *count)
{
    kod_rect whole;
    size_t written = 0u;

    if (count != NULL) {
        *count = 0u;
    }
    if (detector == NULL || detector->broken || bgra == NULL || out == NULL ||
        width <= 0 || height <= 0) {
        return false;
    }
    whole.x = 0;
    whole.y = 0;
    whole.w = width;
    whole.h = height;
    if (!run_one(detector, bgra, width, height, &whole, -1, out, capacity,
                 &written)) {
        return false;
    }
    if (count != NULL) {
        *count = written;
    }
    return true;
}

static float overlap(const kod_rect *a, const kod_rect *b)
{
    const int x0 = a->x > b->x ? a->x : b->x;
    const int y0 = a->y > b->y ? a->y : b->y;
    const int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
    const int y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    long intersection;
    long combined;

    if (x1 <= x0 || y1 <= y0) {
        return 0.0f;
    }
    intersection = (long)(x1 - x0) * (long)(y1 - y0);
    combined = (long)a->w * (long)a->h + (long)b->w * (long)b->h -
               intersection;
    if (combined <= 0) {
        return 0.0f;
    }
    return (float)((double)intersection / (double)combined);
}

bool kod_detect_regions(
    kod_detector *detector, const uint8_t *bgra, int width, int height,
    const kod_rect *regions, size_t region_count, kod_box *out,
    size_t capacity, size_t *count)
{
    size_t written = 0u;

    if (count != NULL) {
        *count = 0u;
    }
    if (detector == NULL || detector->broken || bgra == NULL || out == NULL ||
        width <= 0 || height <= 0) {
        return false;
    }
    if (regions == NULL || region_count == 0u) {
        return true;   /* nothing moved: not an error, and not a detection */
    }
    for (size_t i = 0u; i < region_count && written < capacity; i++) {
        if (!run_one(detector, bgra, width, height, &regions[i], (int)i, out,
                     capacity, &written)) {
            return false;
        }
    }
    /*
     * The same object seen by two overlapping crops is one object.  Kept
     * within a class and by overlap, and the stronger score wins: a car
     * that a tight crop scores 0.9 and a wide one scores 0.4 is a car,
     * reported once, at 0.9.
     */
    for (size_t i = 0u; i < written; i++) {
        for (size_t j = i + 1u; j < written;) {
            if (out[i].class_id == out[j].class_id &&
                overlap(&out[i].at, &out[j].at) > 0.45f) {
                if (out[j].score > out[i].score) {
                    out[i] = out[j];
                }
                out[j] = out[written - 1u];
                written--;
                continue;
            }
            j++;
        }
    }
    if (count != NULL) {
        *count = written;
    }
    return true;
}

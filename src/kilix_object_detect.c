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
#include <time.h>
#include <unistd.h>

#define ERROR_MAX 160
#define KOD_ARGV_MAX 13
#define KOD_DETECTOR_ENV "KILIX_OBJECT_DETECTOR"
#define KOD_DETECTOR_NAME "kilix-look-detect"

typedef struct fit {
    float scale;      /* source pixels per square pixel */
    int offset_x;     /* where the image starts inside the square */
    int offset_y;
} fit;

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

    /*
     * A batch in flight, for the callers that cannot wait.
     *
     * The crops are scaled up front and held here rather than re-derived
     * when their turn comes: by the time the second reply arrives the
     * caller has long since released the frame these were cut from, and a
     * detector that keeps a pointer into a borrowed frame is a detector
     * that reads a later frame's pixels.
     */
    uint8_t *queued[KOD_REGION_MAX];
    fit placements[KOD_REGION_MAX];
    kod_rect regions[KOD_REGION_MAX];
    int frame_width;
    int frame_height;
    size_t batch;         /* crops in this batch */
    size_t sent;          /* ...written to the child */
    kod_box collected[KOD_BOX_MAX];
    size_t collected_count;
    int64_t sent_at_ms;   /* when the outstanding crop went out */
    bool busy;
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

/*
 * Refused, not broken.  A blocking call that arrives while a batch is in
 * flight has asked at a bad moment, but the batch itself is healthy and
 * kod_take() will complete it; recording the refusal as a fault would
 * disable a working detector until close.  The reason is still written,
 * so a caller wondering why the call returned false can ask.
 */
static bool refuse(kod_detector *detector, const char *reason)
{
    if (detector != NULL) {
        (void)snprintf(detector->error, sizeof(detector->error), "%s", reason);
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

static void reap(pid_t child);

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
    int status[2];
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
    /* Close-on-exec, so a successful exec closes it and the parent reads
     * end-of-file.  Anything that arrives instead is the errno that
     * stopped it. */
    if (pipe(status) != 0) {
        (void)close(to_child[0]); (void)close(to_child[1]);
        (void)close(from_child[0]); (void)close(from_child[1]);
        free(detector->square);
        free(detector);
        return false;
    }
    (void)fcntl(status[1], F_SETFD, FD_CLOEXEC);
    detector->child = fork();
    if (detector->child < 0) {
        (void)close(to_child[0]); (void)close(to_child[1]);
        (void)close(from_child[0]); (void)close(from_child[1]);
        (void)close(status[0]); (void)close(status[1]);
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
        /*
         * Exec failed, and this is the only moment anything knows why.
         *
         * The log first, then the signal down the status pipe: kod_open()
         * returns as soon as it reads that, and a caller looking at the
         * log would otherwise find it empty.  Losing this is how a
         * detector nobody installed becomes "the detector stopped
         * reading" seconds later, against a blank log.
         */
        {
            const int reason = errno;

            (void)dprintf(STDERR_FILENO,
                          "kilix-object-detect: cannot run %s: %s\n",
                          child_argv[0], strerror(reason));
            (void)write(status[1], &reason, sizeof(reason));
        }
        _exit(127);
    }
    (void)close(to_child[0]);
    (void)close(from_child[1]);
    (void)close(status[1]);
    {
        int reason = 0;
        const ssize_t got = read(status[0], &reason, sizeof(reason));

        (void)close(status[0]);
        if (got == (ssize_t)sizeof(reason)) {
            /* It never started.  Refusing here beats a detector that
             * looks open and reports a broken pipe on the first crop -
             * which is indistinguishable from a model that crashed. */
            (void)close(to_child[1]);
            (void)close(from_child[0]);
            reap(detector->child);
            free(detector->square);
            free(detector);
            return false;
        }
    }
    detector->to_child = to_child[1];
    detector->from_child = from_child[0];
    (void)signal(SIGPIPE, SIG_IGN);
    *out = detector;
    return true;
}

/*
 * Ask, wait, insist.
 *
 * A plain kill-and-wait can hang forever: a model mid-inference on a busy
 * machine does not return to its interpreter to notice a signal.  A
 * program that cannot be closed is worse than a child that had to be
 * killed, so the wait is bounded and then it is not a request any more.
 */
static void reap(pid_t child)
{
    int status;

    if (child <= 0) {
        return;
    }
    (void)kill(child, SIGTERM);
    for (int waited = 0; waited < 2000; waited += 20) {
        struct timespec pause = {0, 20 * 1000 * 1000};

        if (waitpid(child, &status, WNOHANG) == child) {
            return;
        }
        (void)nanosleep(&pause, NULL);
    }
    (void)kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
}

static void free_queued(kod_detector *detector)
{
    for (size_t i = 0u; i < KOD_REGION_MAX; i++) {
        free(detector->queued[i]);
        detector->queued[i] = NULL;
    }
}

void kod_close(kod_detector *detector)
{
    if (detector == NULL) {
        return;
    }
    if (detector->to_child >= 0) { (void)close(detector->to_child); }
    if (detector->from_child >= 0) { (void)close(detector->from_child); }
    reap(detector->child);
    free_queued(detector);
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

/* A clock that does not step, for deciding a reply is never coming. */
static int64_t now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/*
 * The timeout is a deadline on the whole read, not on each piece of it.
 *
 * Restarting the budget at every chunk would let a detector that drips a
 * byte per interval keep a "bounded" wait alive for as long as it liked -
 * a dying ssh connection to a remote detector trickles exactly like that.
 * Whatever has already arrived when the deadline passes still counts, so
 * a reply that is sitting in the pipe complete is never refused.
 */
static bool read_all(int fd, uint8_t *bytes, size_t size, int timeout_ms)
{
    const int64_t deadline = now_ms() + timeout_ms;
    size_t offset = 0u;

    while (offset < size) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        int64_t remaining = deadline - now_ms();
        int ready;
        ssize_t got;

        if (remaining < 0) {
            remaining = 0;
        }
        ready = poll(&descriptor, 1u, (int)remaining);
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
 * Every region scaled into its own square, ready to go out one at a time.
 *
 * Done up front because it is the cheap half - arithmetic over 320x320 -
 * and because it is the half that needs the caller's frame.  What the
 * caller cannot afford is the other half: waiting for the model.
 */
static bool scale_all(
    kod_detector *detector, const uint8_t *bgra, int width, int height,
    const kod_rect *regions, size_t count)
{
    const size_t bytes = (size_t)detector->size * (size_t)detector->size * 4u;

    if (count > KOD_REGION_MAX) {
        count = KOD_REGION_MAX;
    }
    for (size_t i = 0u; i < count; i++) {
        if (detector->queued[i] == NULL) {
            /* Lazily: a detector used only whole-frame never pays for a
             * batch it will not have. */
            detector->queued[i] = malloc(bytes);
            if (detector->queued[i] == NULL) {
                return fail(detector, "out of memory for a crop");
            }
        }
        detector->placements[i] = scale_into_square(
            bgra, width, height, &regions[i], detector->queued[i],
            detector->size);
        detector->regions[i] = regions[i];
    }
    detector->frame_width = width;
    detector->frame_height = height;
    detector->batch = count;
    detector->sent = 0u;
    detector->collected_count = 0u;
    return true;
}

/* The next queued crop to the child.  Fast: the child is always reading. */
static bool send_next(kod_detector *detector)
{
    const size_t bytes = (size_t)detector->size * (size_t)detector->size * 4u;

    if (detector->sent >= detector->batch) {
        return true;
    }
    if (!write_all(detector->to_child, detector->queued[detector->sent],
                   bytes)) {
        return fail(detector, "the detector stopped reading");
    }
    detector->sent++;
    detector->sent_at_ms = now_ms();
    return true;
}

/*
 * One reply, with every coordinate transform undone.
 *
 * normalised square -> square pixels -> minus the letterbox -> times the
 * scale -> plus the region's own origin.  Four steps, one place.
 */
static bool take_reply(kod_detector *detector, int timeout_ms)
{
    uint8_t reply[KOD_REPLY_BYTES];
    float rows[KOD_REPLY_ROWS][KOD_REPLY_COLUMNS];
    const size_t slot = detector->sent - 1u;
    const fit placement = detector->placements[slot];
    const kod_rect *region = &detector->regions[slot];
    const int width = detector->frame_width;
    const int height = detector->frame_height;

    if (!read_all(detector->from_child, reply, sizeof(reply), timeout_ms)) {
        return fail(detector, detector->warm
                                  ? "the detector did not answer in time"
                                  : "the detector never started (no model?)");
    }
    detector->warm = true;
    detector->crops++;
    (void)memcpy(rows, reply, sizeof(rows));
    for (size_t i = 0u;
         i < KOD_REPLY_ROWS && detector->collected_count < KOD_BOX_MAX; i++) {
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

        box = &detector->collected[detector->collected_count];
        box->class_id = class_id;
        box->score = score;
        box->region = (int)slot;
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
        detector->collected_count++;
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

/*
 * The same object seen by two overlapping crops is one object.
 *
 * Kept within a class and by overlap, and the stronger score wins: a car
 * that a tight crop scores 0.9 and a wide one scores 0.4 is a car,
 * reported once, at 0.9.
 */
static void dedupe(kod_box *boxes, size_t *count)
{
    for (size_t i = 0u; i < *count; i++) {
        for (size_t j = i + 1u; j < *count;) {
            if (boxes[i].class_id == boxes[j].class_id &&
                overlap(&boxes[i].at, &boxes[j].at) > 0.45f) {
                if (boxes[j].score > boxes[i].score) {
                    boxes[i] = boxes[j];
                }
                boxes[j] = boxes[*count - 1u];
                (*count)--;
                continue;
            }
            j++;
        }
    }
}

static size_t deliver(kod_detector *detector, kod_box *out, size_t capacity)
{
    size_t written = detector->collected_count;

    dedupe(detector->collected, &written);
    if (written > capacity) {
        written = capacity;
    }
    (void)memcpy(out, detector->collected, written * sizeof(*out));
    detector->collected_count = 0u;
    detector->batch = 0u;
    detector->sent = 0u;
    detector->busy = false;
    return written;
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
    if (detector->busy) {
        return refuse(detector, "a batch is already in flight");
    }
    whole.x = 0;
    whole.y = 0;
    whole.w = width;
    whole.h = height;
    if (!scale_all(detector, bgra, width, height, &whole, 1u) ||
        !send_next(detector) ||
        !take_reply(detector, detector->warm ? detector->timeout_ms
                                             : detector->warmup_ms)) {
        detector->batch = 0u;
        detector->sent = 0u;
        return false;
    }
    written = deliver(detector, out, capacity);
    /* -1 rather than crop 0: this looked at the whole frame, and a caller
     * tuning its regions needs to be able to tell those apart. */
    for (size_t i = 0u; i < written; i++) {
        out[i].region = -1;
    }
    if (count != NULL) {
        *count = written;
    }
    return true;
}

bool kod_detect_regions(
    kod_detector *detector, const uint8_t *bgra, int width, int height,
    const kod_rect *regions, size_t region_count, kod_box *out,
    size_t capacity, size_t *count)
{
    if (count != NULL) {
        *count = 0u;
    }
    if (detector == NULL || detector->broken || bgra == NULL || out == NULL ||
        width <= 0 || height <= 0) {
        return false;
    }
    if (detector->busy) {
        return refuse(detector, "a batch is already in flight");
    }
    if (regions == NULL || region_count == 0u) {
        return true;   /* nothing moved: not an error, and not a detection */
    }
    /*
     * However many regions arrive, every one of them is inferred.
     *
     * The batch arrays hold KOD_REGION_MAX crops, so a longer list is
     * taken that many at a time.  Truncating instead would silently drop
     * whichever regions happened to come last - and this call can afford
     * to go around again, because waiting is what it does.
     */
    {
        size_t begun = 0u;
        size_t kept = 0u;

        while (begun < region_count) {
            size_t chunk = region_count - begun;

            if (chunk > KOD_REGION_MAX) {
                chunk = KOD_REGION_MAX;
            }
            if (!scale_all(detector, bgra, width, height, regions + begun,
                           chunk)) {
                return false;
            }
            detector->collected_count = kept;   /* earlier rounds stay */
            while (detector->sent < detector->batch) {
                if (!send_next(detector) ||
                    !take_reply(detector, detector->warm
                                              ? detector->timeout_ms
                                              : detector->warmup_ms)) {
                    detector->batch = 0u;
                    detector->sent = 0u;
                    return false;
                }
            }
            /* The replies name crops by their place in this round; the
             * caller knows them by their place in the whole list. */
            for (size_t i = kept; i < detector->collected_count; i++) {
                detector->collected[i].region += (int)begun;
            }
            kept = detector->collected_count;
            begun += chunk;
        }
    }
    if (count != NULL) {
        *count = deliver(detector, out, capacity);
    } else {
        (void)deliver(detector, out, capacity);
    }
    return true;
}

/* ------------------------------ not waiting ------------------------------ */

bool kod_offer(
    kod_detector *detector, const uint8_t *bgra, int width, int height,
    const kod_rect *regions, size_t region_count)
{
    if (detector == NULL || detector->broken || bgra == NULL || width <= 0 ||
        height <= 0 || regions == NULL || region_count == 0u) {
        return false;
    }
    if (detector->busy) {
        /* Not a failure: a caller offering every frame is the expected
         * use, and "still working on the last one" is the answer most of
         * the time.  It must not poison the detector's error. */
        return false;
    }
    if (!scale_all(detector, bgra, width, height, regions, region_count)) {
        return false;
    }
    if (!send_next(detector)) {
        detector->batch = 0u;
        return false;
    }
    detector->busy = true;
    return true;
}

bool kod_busy(const kod_detector *detector)
{
    return detector != NULL && detector->busy;
}

bool kod_take(
    kod_detector *detector, kod_box *out, size_t capacity, size_t *count,
    bool *done)
{
    struct pollfd descriptor;
    const int limit = detector != NULL && detector->warm
                          ? detector->timeout_ms
                          : (detector != NULL ? detector->warmup_ms : 0);

    if (count != NULL) {
        *count = 0u;
    }
    if (done != NULL) {
        *done = false;
    }
    if (detector == NULL || detector->broken || out == NULL) {
        return false;
    }
    if (!detector->busy) {
        if (done != NULL) {
            *done = true;
        }
        return true;
    }
    descriptor.fd = detector->from_child;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    if (poll(&descriptor, 1u, 0) <= 0) {
        /*
         * Nothing yet, which is the ordinary answer.  The deadline is
         * still enforced here, because a child that died holding a crop
         * would otherwise leave the detector busy forever and silently
         * stop detecting - the failure this whole path exists to avoid.
         */
        if (now_ms() - detector->sent_at_ms > (int64_t)limit) {
            detector->busy = false;
            detector->batch = 0u;
            detector->sent = 0u;
            return fail(detector, detector->warm
                                      ? "the detector did not answer in time"
                                      : "the detector never started "
                                        "(no model?)");
        }
        return true;
    }
    /* Readable, so the reply is on its way and this cannot stall.  What
     * is left of the deadline is what it gets: the time spent polling
     * already came out of the same budget. */
    {
        int64_t remaining =
            (int64_t)limit - (now_ms() - detector->sent_at_ms);

        if (remaining < 0) {
            remaining = 0;
        }
        if (!take_reply(detector, (int)remaining)) {
            detector->busy = false;
            detector->batch = 0u;
            detector->sent = 0u;
            return false;
        }
    }
    if (detector->sent < detector->batch) {
        if (!send_next(detector)) {
            detector->busy = false;
            detector->batch = 0u;
            detector->sent = 0u;
            return false;
        }
        return true;
    }
    if (count != NULL) {
        *count = deliver(detector, out, capacity);
    } else {
        (void)deliver(detector, out, capacity);
    }
    if (done != NULL) {
        *done = true;
    }
    return true;
}

/* ------------------------------- drawing --------------------------------- */

static void put(uint8_t *bgra, int width, int height, int x, int y,
                uint32_t colour, float alpha)
{
    uint8_t *pixel;

    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    pixel = bgra + ((size_t)y * (size_t)width + (size_t)x) * 4u;
    pixel[0] = (uint8_t)((float)pixel[0] * (1.0f - alpha) +
                         (float)(colour & 0xFFu) * alpha);
    pixel[1] = (uint8_t)((float)pixel[1] * (1.0f - alpha) +
                         (float)((colour >> 8) & 0xFFu) * alpha);
    pixel[2] = (uint8_t)((float)pixel[2] * (1.0f - alpha) +
                         (float)((colour >> 16) & 0xFFu) * alpha);
}

static void outline(uint8_t *bgra, int width, int height, const kod_rect *at,
                    uint32_t colour, float alpha, int thickness)
{
    for (int t = 0; t < thickness; t++) {
        const int x0 = at->x + t;
        const int y0 = at->y + t;
        const int x1 = at->x + at->w - 1 - t;
        const int y1 = at->y + at->h - 1 - t;

        for (int x = x0; x <= x1; x++) {
            put(bgra, width, height, x, y0, colour, alpha);
            put(bgra, width, height, x, y1, colour, alpha);
        }
        for (int y = y0; y <= y1; y++) {
            put(bgra, width, height, x0, y, colour, alpha);
            put(bgra, width, height, x1, y, colour, alpha);
        }
    }
}

/* Person green, vehicles blue, animals amber: three groups is what the
 * eye can read at a glance, where sixteen colours is a legend. */
uint32_t kod_class_colour(int class_id)
{
    switch (class_id) {
    case 0:
        return 0x60FF80u;
    case 1: case 2: case 3: case 5: case 7:
        return 0x60B0FFu;
    case 14: case 15: case 16: case 17: case 18: case 19: case 21:
        return 0xFFC040u;
    default:
        return 0xFF80C0u;
    }
}

void kod_draw_regions(
    uint8_t *bgra, int width, int height, const kod_rect *regions,
    size_t count)
{
    if (bgra == NULL || regions == NULL) {
        return;
    }
    for (size_t i = 0u; i < count; i++) {
        outline(bgra, width, height, &regions[i], 0x808080u, 0.35f, 1);
    }
}

void kod_draw_boxes(
    uint8_t *bgra, int width, int height, const kod_box *boxes, size_t count)
{
    if (bgra == NULL || boxes == NULL || width <= 0 || height <= 0) {
        return;
    }
    for (size_t i = 0u; i < count; i++) {
        outline(bgra, width, height, &boxes[i].at,
                kod_class_colour(boxes[i].class_id), 1.0f, 2);
    }
}

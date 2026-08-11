#include "kod_app.h"

#include "soft_raster.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------------------------------- paths -------------------------------- */

bool kod_paths_home(char *out, size_t size)
{
    const char *override_home = getenv("KILIX_LOOK_HOME");

    if (out == NULL || size == 0u) {
        return false;
    }
    if (override_home != NULL && override_home[0] != '\0') {
        if (snprintf(out, size, "%s", override_home) < 0) {
            return false;
        }
    } else {
        const char *base = getenv("XDG_DATA_HOME");

        if (base != NULL && base[0] != '\0') {
            if (snprintf(out, size, "%s/gpu_terminal/kilix-object-detect",
                         base) < 0) {
                return false;
            }
        } else {
            const char *home = getenv("HOME");

            if (home == NULL || home[0] == '\0') {
                return false;
            }
            if (snprintf(out, size,
                         "%s/.local/gpu_terminal/kilix-object-detect",
                         home) < 0) {
                return false;
            }
        }
    }
    if (mkdir(out, 0700) != 0) {
        struct stat info;

        if (stat(out, &info) != 0 || !S_ISDIR(info.st_mode)) {
            return false;
        }
    }
    return true;
}

bool kod_paths_file(char *out, size_t size, const char *name)
{
    char home[KOD_PATH_MAX];

    if (out == NULL || name == NULL || !kod_paths_home(home, sizeof(home))) {
        return false;
    }
    return snprintf(out, size, "%s/%s", home, name) > 0;
}

void kod_redact(const char *source, char *out, size_t size)
{
    const char *scheme_end;
    const char *at;

    if (out == NULL || size == 0u) {
        return;
    }
    if (source == NULL) {
        out[0] = '\0';
        return;
    }
    scheme_end = strstr(source, "://");
    at = strrchr(source, '@');
    if (scheme_end == NULL || at == NULL || at < scheme_end) {
        (void)snprintf(out, size, "%s", source);
        return;
    }
    (void)snprintf(out, size, "%.*s://***@%s", (int)(scheme_end - source),
                   source, at + 1);
}

/* --------------------------------- grabbing ------------------------------ */

/* The geometry of whatever this is, from ffprobe. */
static bool probe_size(const char *source, int *width, int *height)
{
    int pipes[2];
    pid_t child;
    char buffer[128];
    ssize_t got;
    int status = 0;

    if (pipe(pipes) != 0) {
        return false;
    }
    child = fork();
    if (child < 0) {
        (void)close(pipes[0]); (void)close(pipes[1]);
        return false;
    }
    if (child == 0) {
        const int null_fd = open("/dev/null", O_WRONLY);

        (void)dup2(pipes[1], STDOUT_FILENO);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDERR_FILENO);
            (void)close(null_fd);
        }
        (void)close(pipes[0]);
        (void)close(pipes[1]);
        (void)execlp("ffprobe", "ffprobe", "-v", "error", "-select_streams",
                     "v:0", "-show_entries", "stream=width,height", "-of",
                     "csv=p=0:s=x", source, (char *)NULL);
        _exit(127);
    }
    (void)close(pipes[1]);
    got = read(pipes[0], buffer, sizeof(buffer) - 1u);
    (void)close(pipes[0]);
    (void)waitpid(child, &status, 0);
    if (got <= 0) {
        return false;
    }
    buffer[got] = '\0';
    return sscanf(buffer, "%dx%d", width, height) == 2 && *width > 0 &&
           *height > 0;
}

uint8_t *kod_grab(const char *source, int *width, int *height,
                  const char **reason)
{
    int pipes[2];
    pid_t child;
    uint8_t *frame;
    size_t bytes;
    size_t filled = 0u;
    int status = 0;

    if (reason != NULL) {
        *reason = NULL;
    }
    if (source == NULL || width == NULL || height == NULL) {
        return NULL;
    }
    if (!probe_size(source, width, height)) {
        if (reason != NULL) {
            *reason = "ffprobe could not read it (is it an image or a video?)";
        }
        return NULL;
    }
    bytes = (size_t)*width * (size_t)*height * 4u;
    frame = malloc(bytes);
    if (frame == NULL) {
        return NULL;
    }
    if (pipe(pipes) != 0) {
        free(frame);
        return NULL;
    }
    child = fork();
    if (child < 0) {
        (void)close(pipes[0]); (void)close(pipes[1]);
        free(frame);
        return NULL;
    }
    if (child == 0) {
        const int null_fd = open("/dev/null", O_WRONLY);

        (void)dup2(pipes[1], STDOUT_FILENO);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDERR_FILENO);
            (void)close(null_fd);
        }
        (void)close(pipes[0]);
        (void)close(pipes[1]);
        (void)execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "error",
                     "-nostdin", "-i", source, "-frames:v", "1", "-f",
                     "rawvideo", "-pix_fmt", "bgra", "-", (char *)NULL);
        _exit(127);
    }
    (void)close(pipes[1]);
    while (filled < bytes) {
        const ssize_t got = read(pipes[0], frame + filled, bytes - filled);

        if (got > 0) {
            filled += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    (void)close(pipes[0]);
    (void)waitpid(child, &status, 0);
    if (filled != bytes) {
        free(frame);
        if (reason != NULL) {
            *reason = "ffmpeg did not produce a whole frame";
        }
        return NULL;
    }
    return frame;
}

/* --------------------------------- drawing ------------------------------- */

/*
 * Labels only.  The outlines and their colours come from the library, so
 * a box drawn by the recorder and a box drawn by the analyzer cannot
 * disagree about what colour a person is.
 */
void kod_draw_labels(
    uint8_t *bgra, int width, int height, const kod_box *boxes, size_t count)
{
    sr_canvas canvas;

    if (bgra == NULL || boxes == NULL || width <= 0 || height <= 0) {
        return;
    }
    /* Wrapped rather than copied: sr_canvas is 32-bit BGRA in memory,
     * which is exactly what arrived from the decoder. */
    sr_canvas_wrap(&canvas, (uint32_t *)(void *)bgra, width, height);
    for (size_t i = 0u; i < count; i++) {
        const char *label = kod_label(boxes[i].class_id);
        char text[64];
        int text_y = boxes[i].at.y - 14;

        if (label == NULL) {
            continue;
        }
        (void)snprintf(text, sizeof(text), "%s %.2f", label,
                       (double)boxes[i].score);
        /* Inside the box when there is no room above it, which there
         * never is for anything standing at the top of the frame. */
        if (text_y < 0) {
            text_y = boxes[i].at.y + 2;
        }
        sr_fill_rect(&canvas, (float)boxes[i].at.x, (float)text_y,
                     (float)sr_text_width_in(SR_FONT_FIXED_8X16, text, 1) + 4.0f,
                     14.0f, 0x000000u, 0.55f);
        sr_text(&canvas, (float)boxes[i].at.x + 2.0f, (float)text_y, text,
                kod_class_colour(boxes[i].class_id), 1.0f, 1);
    }
}

bool kod_write_ppm(const char *path, const uint8_t *bgra, int width,
                   int height)
{
    FILE *file;

    if (path == NULL || bgra == NULL) {
        return false;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    (void)fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        (void)fputc(bgra[i * 4 + 2], file);
        (void)fputc(bgra[i * 4 + 1], file);
        (void)fputc(bgra[i * 4 + 0], file);
    }
    return fclose(file) == 0;
}

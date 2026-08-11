#ifndef KOD_APP_H
#define KOD_APP_H

/*
 * The parts of the command that are not the library: where its files
 * live, how a frame is got out of a source, and how a frame with boxes on
 * it is written somewhere a person can look at it.
 */

#include "kilix_object_detect.h"

#include <stdbool.h>
#include <stddef.h>

#define KOD_PATH_MAX 1024

/* The data directory, created 0700.  `KILIX_LOOK_HOME` overrides it. */
bool kod_paths_home(char *out, size_t size);
bool kod_paths_file(char *out, size_t size, const char *name);

/*
 * A source, safe to print.  An RTSP url carries `user:password@` and this
 * program has a title bar, error messages and a log; the only way to
 * honour that is for nothing to print a source without coming through
 * here.
 */
void kod_redact(const char *source, char *out, size_t size);

/*
 * One frame out of anything ffmpeg can open - a jpeg, a png, a frame of a
 * recording, a camera.
 *
 * Returns a malloc'd BGRA buffer the caller frees, with the geometry it
 * discovered.  Deliberately not the streaming path: a still image is one
 * frame and then end-of-file, which every supervisor in this family
 * correctly treats as a dead source.
 */
uint8_t *kod_grab(const char *source, int *width, int *height,
                  const char **reason);

/* Labels over the boxes.  The outlines themselves are kod_draw_boxes(),
 * in the library, so every caller draws them identically. */
void kod_draw_labels(
    uint8_t *bgra, int width, int height, const kod_box *boxes, size_t count);

/* Write a BGRA frame out as a PPM, for looking at without a terminal. */
bool kod_write_ppm(const char *path, const uint8_t *bgra, int width,
                   int height);

#endif /* KOD_APP_H */

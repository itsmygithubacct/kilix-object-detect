#ifndef KOD_UI_H
#define KOD_UI_H

/*
 * Showing a video stream with what was found drawn on it.
 *
 * The frame itself is the picture, so the overlay is drawn into the frame
 * rather than composed beside it; the only thing this adds is a status
 * strip and, when something is listening as well as looking, the sound
 * classes along the bottom.  Audio and video on one screen is the whole
 * reason to run them together: a bark and a dog in the same second is a
 * different fact from either alone.
 */

#include "kilix_object_detect.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct kod_ui kod_ui;

typedef struct kod_ui_status {
    const char *title;       /* already redacted by the caller */
    uint64_t frames;
    uint64_t motion_frames;
    uint64_t crops;
    size_t boxes;
    size_t regions;
    /* KSD_CLASS_COUNT scores and their labels, or NULL when not
     * listening.  Passed as plain arrays so this does not have to depend
     * on the sound module. */
    const float *sound_scores;
    const char *const *sound_labels;
    size_t sound_count;
    float sound_threshold;
} kod_ui_status;

/* Returns NULL when this terminal cannot show graphics, with the reason
 * already printed. */
bool kod_ui_open(kod_ui **ui);
void kod_ui_close(kod_ui *ui);

/*
 * Present one frame, scaled to fit the terminal.  The frame is not
 * modified: boxes are expected to be drawn into it already, because the
 * same picture is what a --render writes and the two must not differ.
 */
bool kod_ui_present(
    kod_ui *ui, const uint8_t *bgra, int width, int height,
    const kod_ui_status *status);

/* The next key pressed, or 0.  'q' and escape both mean quit. */
int kod_ui_key(kod_ui *ui);

#endif /* KOD_UI_H */

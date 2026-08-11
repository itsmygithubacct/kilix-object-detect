#include "kod_ui.h"

#include "kitty_terminal_session.h"
#include "soft_raster.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BACKDROP 0x00101014u
#define PANEL 0x00181820u
#define TEXT 0x00C8C8D4u
#define DIM 0x00707884u
#define TRACK 0x00242430u
#define HOT 0x0060FF80u

#define BAR_HEIGHT 14

struct kod_ui {
    kittyts_session session;
    sr_canvas frame;
    sr_canvas picture;      /* the incoming frame, wrapped */
    uint8_t *rgba;
    int width;
    int height;
};

bool kod_ui_open(kod_ui **out)
{
    kittyts_options options;
    kod_ui *ui;

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    ui = calloc(1u, sizeof(*ui));
    if (ui == NULL) {
        return false;
    }
    kittyts_session_init(&ui->session);
    kittyts_options_init(&options);
    if (kittyts_start(&ui->session, STDIN_FILENO, STDOUT_FILENO,
                      &options) != 0) {
        (void)fprintf(stderr, "kilix-look: %s\n",
                      errno == ENOTSUP
                          ? "this terminal does not support graphics"
                          : strerror(errno));
        free(ui);
        return false;
    }
    ui->width = kittyts_width(&ui->session);
    ui->height = kittyts_height(&ui->session);
    if (!sr_canvas_init(&ui->frame, ui->width, ui->height)) {
        kittyts_stop(&ui->session);
        free(ui);
        return false;
    }
    ui->rgba = malloc((size_t)ui->width * (size_t)ui->height * 4u);
    if (ui->rgba == NULL) {
        sr_canvas_free(&ui->frame);
        kittyts_stop(&ui->session);
        free(ui);
        return false;
    }
    *out = ui;
    return true;
}

void kod_ui_close(kod_ui *ui)
{
    if (ui == NULL) {
        return;
    }
    free(ui->rgba);
    sr_canvas_free(&ui->frame);
    kittyts_stop(&ui->session);
    free(ui);
}

static void draw_sound(sr_canvas *canvas, const kod_ui_status *status, int y)
{
    const int label_width = 72;
    const int left = 8 + label_width;
    const int width = canvas->w - left - 56;

    if (status->sound_scores == NULL || status->sound_count == 0u) {
        return;
    }
    sr_text(canvas, 8.0f, (float)(y - 16), "hearing", DIM, 1.0f, 1);
    for (size_t i = 0u; i < status->sound_count; i++) {
        const int row = y + (int)i * BAR_HEIGHT;
        const float score = status->sound_scores[i];
        const bool over = score >= status->sound_threshold;
        char value[16];

        sr_text(canvas, 8.0f, (float)row, status->sound_labels[i],
                over ? TEXT : DIM, 1.0f, 1);
        sr_fill_rect(canvas, (float)left, (float)row, (float)width, 10.0f,
                     TRACK, 1.0f);
        if (score > 0.0f) {
            sr_fill_rect(canvas, (float)left, (float)row,
                         (float)width * score, 10.0f, HOT,
                         over ? 1.0f : 0.4f);
        }
        (void)snprintf(value, sizeof(value), "%.2f", (double)score);
        sr_text(canvas, (float)(left + width + 8), (float)row, value,
                over ? TEXT : DIM, 1.0f, 1);
    }
    {
        /* The line, so a threshold is a thing you can see rather than a
         * number you have to remember. */
        const float x = (float)left + (float)width * status->sound_threshold;

        sr_line(canvas, x, (float)y - 2.0f, x,
                (float)(y + (int)status->sound_count * BAR_HEIGHT), 1.0f,
                0x00FF6060u, 0.7f, 4, 4);
    }
}

bool kod_ui_present(
    kod_ui *ui, const uint8_t *bgra, int width, int height,
    const kod_ui_status *status)
{
    char line[256];
    int sound_rows;
    int reserved;
    int area_h;
    float scale;
    int drawn_w;
    int drawn_h;

    if (ui == NULL || bgra == NULL || status == NULL) {
        return false;
    }
    sound_rows = status->sound_scores != NULL ? (int)status->sound_count : 0;
    reserved = 26 + (sound_rows > 0 ? sound_rows * BAR_HEIGHT + 24 : 0) + 20;
    area_h = ui->height - reserved;
    if (area_h < 32) {
        area_h = 32;
    }
    sr_clear(&ui->frame, BACKDROP);

    /* The frame, fitted rather than cropped: a view that has had its
     * edges cut off is a view that can hide the thing being looked for. */
    {
        const float scale_x = (float)(ui->width - 16) / (float)width;
        const float scale_y = (float)area_h / (float)height;

        scale = scale_x < scale_y ? scale_x : scale_y;
        drawn_w = (int)((float)width * scale);
        drawn_h = (int)((float)height * scale);
    }
    sr_canvas_wrap(&ui->picture, (uint32_t *)(void *)(uintptr_t)bgra, width,
                   height);
    sr_blit_scaled(&ui->frame, &ui->picture, 8, 30, drawn_w, drawn_h, 1.0f);

    sr_fill_rect(&ui->frame, 0.0f, 0.0f, (float)ui->width, 26.0f, PANEL,
                 1.0f);
    (void)snprintf(line, sizeof(line), "looking at %s", status->title);
    sr_text(&ui->frame, 8.0f, 7.0f, line, TEXT, 1.0f, 1);
    (void)snprintf(line, sizeof(line),
                   "%llu frames  %llu moved  %llu crops  %zu found",
                   (unsigned long long)status->frames,
                   (unsigned long long)status->motion_frames,
                   (unsigned long long)status->crops, status->boxes);
    sr_text(&ui->frame,
            (float)(ui->width - 8 -
                    sr_text_width_in(SR_FONT_FIXED_8X16, line, 1)),
            7.0f, line, DIM, 1.0f, 1);

    if (sound_rows > 0) {
        draw_sound(&ui->frame, status, 30 + drawn_h + 28);
    }
    sr_text(&ui->frame, 8.0f, (float)(ui->height - 16), "q quit", DIM, 1.0f,
            1);
    if (!sr_pack_rgba(&ui->frame, ui->rgba,
                      (size_t)ui->width * (size_t)ui->height * 4u)) {
        return false;
    }
    return kittyts_present(&ui->session, ui->rgba, ui->width, ui->height) == 0;
}

int kod_ui_key(kod_ui *ui)
{
    struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
    kittykb_event key;

    if (ui == NULL) {
        return 0;
    }
    if (poll(&descriptor, 1u, 0) > 0) {
        (void)kittyts_read_input(&ui->session);
    }
    while (kittyts_next_key_event(&ui->session, &key)) {
        if (key.action == KITTYKB_ACTION_RELEASE) {
            continue;
        }
        if (key.key == KITTYKB_KEY_ESCAPE) {
            return 'q';
        }
        return (int)key.key;
    }
    return 0;
}

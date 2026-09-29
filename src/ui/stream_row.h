/*
 * stream_row.h - one row of the stream table: what the stream is, and what
 * to do with it (copy / transcode with an encoder / drop) for the current
 * container. Only choices the container can take are offered.
 */
#ifndef UI_STREAM_ROW_H
#define UI_STREAM_ROW_H

#include <gtk/gtk.h>

#include "caps.h"
#include "job.h"
#include "option_editor.h"
#include "probe.h"

typedef struct StreamRow StreamRow;

typedef void (*StreamRowChanged)(void *user);

/* options: encoder options from the stream's previous row (taken over, see
 * stream_row_take_options), or NULL.
 * prev_action / prev_encoder: the user's previous choice for this stream
 * (kept when the container still allows it), or -1 / NULL for the default. */
StreamRow *stream_row_new(const Caps *caps, const CapsMuxer *mux, const MediaStream *ms,
                          GHashTable *options,
                          int prev_action, const char *prev_encoder,
                          StreamRowChanged changed, void *user);
void       stream_row_free(StreamRow *row);

/* Options the user set for the selected encoder (NULL if none / not transcoding). */
const AVDictionary *stream_row_options(const StreamRow *row);
/* Hand the per-encoder options over to the row that replaces this one. */
GHashTable *stream_row_take_options(StreamRow *row);
/* Open the option editor for the selected encoder. */
GtkWindow  *stream_row_edit_options(StreamRow *row, OptionEditor **editor);

GtkWidget   *stream_row_widget(const StreamRow *row);
int          stream_row_input_index(const StreamRow *row);
JobAction    stream_row_action(const StreamRow *row);
const char  *stream_row_encoder(const StreamRow *row);   /* NULL unless transcoding */

/* Select an action (and encoder) if the row offers it; returns FALSE otherwise. */
gboolean     stream_row_select(StreamRow *row, JobAction action, const char *encoder);

/* One-line description of a stream ("h264 (High) · 1920×1080 · 29.97 fps"). */
char *stream_describe(const MediaStream *ms);

#endif /* UI_STREAM_ROW_H */

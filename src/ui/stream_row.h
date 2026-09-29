/*
 * stream_row.h - one row of the stream table: what the stream is, and what
 * to do with it (copy / transcode with an encoder / drop) for the current
 * container. Only choices the container can take are offered.
 */
#ifndef UI_STREAM_ROW_H
#define UI_STREAM_ROW_H

#include <gtk/gtk.h>

#include "caps.h"
#include "filter_chain.h"
#include "job.h"
#include "option_editor.h"
#include "probe.h"

typedef struct StreamRow StreamRow;

typedef void (*StreamRowChanged)(void *user);

/* options: encoder options from the stream's previous row (taken over, see
 * stream_row_take_options), or NULL.
 * filters: the stream's filter chain from the previous row (taken over, see
 * stream_row_take_filters), or NULL for none.
 * prev_action / prev_encoder: the user's previous choice for this stream
 * (kept when the container still allows it), or -1 / NULL for the default. */
StreamRow *stream_row_new(const Caps *caps, const CapsMuxer *mux, const MediaStream *ms,
                          GHashTable *options, FilterChain *filters,
                          int prev_action, const char *prev_encoder,
                          StreamRowChanged changed, void *user);
void       stream_row_free(StreamRow *row);

/* Options the user set for the selected encoder (NULL if none / not transcoding). */
const AVDictionary *stream_row_options(const StreamRow *row);
/* Hand the per-encoder options over to the row that replaces this one. */
GHashTable *stream_row_take_options(StreamRow *row);
/* Open the option editor for the selected encoder. */
GtkWindow  *stream_row_edit_options(StreamRow *row, OptionEditor **editor);

/* The stream's filter chain (used when transcoding audio or video). */
FilterChain *stream_row_filters(const StreamRow *row);
FilterChain *stream_row_take_filters(StreamRow *row);
/* Open the filter editor; NULL if the stream cannot be filtered as chosen. */
GtkWindow   *stream_row_edit_filters(StreamRow *row);
/* The chain applies: audio/video being converted. */
gboolean     stream_row_filterable(const StreamRow *row);

GtkWidget   *stream_row_widget(const StreamRow *row);
/* The action and encoder dropdowns (test hooks). */
GtkWidget   *stream_row_action_dropdown(const StreamRow *row);
GtkWidget   *stream_row_encoder_dropdown(const StreamRow *row);
int          stream_row_input_index(const StreamRow *row);
JobAction    stream_row_action(const StreamRow *row);
const char  *stream_row_encoder(const StreamRow *row);   /* NULL unless transcoding */

/* Can muxer `m` store this stream as currently chosen? If not, *why says so
 * ("cannot store stream #1 as is (opus)"). A dropped stream always fits. */
gboolean     stream_row_fits(const StreamRow *row, const CapsMuxer *m, char *why, size_t size);

/* Select an action (and encoder), greyed or not; FALSE if the row has no such choice. */
gboolean     stream_row_select(StreamRow *row, JobAction action, const char *encoder);

/* One-line description of a stream ("h264 (High) · 1920×1080 · 29.97 fps"). */
char *stream_describe(const MediaStream *ms);

#endif /* UI_STREAM_ROW_H */

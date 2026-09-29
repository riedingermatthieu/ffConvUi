/*
 * window.h - main window: input file, stream table, container, output file,
 * live validation and the Convert button.
 */
#ifndef UI_WINDOW_H
#define UI_WINDOW_H

#include <gtk/gtk.h>

#include "caps.h"
#include "job.h"
#include "option_editor.h"
#include "stream_row.h"

typedef struct ConvWindow ConvWindow;

/* `caps` must outlive the window. */
ConvWindow *conv_window_new(GtkApplication *app, const Caps *caps);
GtkWindow  *conv_window_get(ConvWindow *w);
/* The ConvWindow of a GtkWindow, or NULL (e.g. from GtkApplication::window-added). */
ConvWindow *conv_window_from_window(GtkWindow *win);
void        conv_window_open(ConvWindow *w, GFile *file);

/* ------------------------------------------------------------------------- */
/* Events, for code that drives or observes the window (main thread).        */

typedef struct ConvWindowListener {
    /* A file was opened: its containers and stream rows are in place. */
    void (*file_loaded)(ConvWindow *w, void *user);
    /* Convert was pressed: `job` is being run in `progress` (valid until
     * conversion_finished). */
    void (*conversion_started)(ConvWindow *w, const ConvJob *job, GtkWindow *progress, void *user);
    void (*conversion_finished)(ConvWindow *w, int ret, void *user);
    /* The choices changed (and were checked): conv_window_build_job() gives
     * the new job. */
    void (*job_changed)(ConvWindow *w, void *user);
} ConvWindowListener;

/* Any number of listeners; returns an id for conv_window_remove_listener(). */
guint conv_window_add_listener(ConvWindow *w, const ConvWindowListener *l, void *user);
void  conv_window_remove_listener(ConvWindow *w, guint id);

/* ------------------------------------------------------------------------- */
/* What a user can do, as operations. Each goes through the same widgets and
 * change handlers as the user's action would.                               */

gboolean    conv_window_select_container(ConvWindow *w, const char *key);
const char *conv_window_container_key(ConvWindow *w);        /* NULL before a file is open */
GtkWidget  *conv_window_container_dropdown(ConvWindow *w);

int         conv_window_nb_streams(ConvWindow *w);
StreamRow  *conv_window_stream(ConvWindow *w, int input_index);   /* NULL if none */
StreamRow  *conv_window_stream_at(ConvWindow *w, int position);

void        conv_window_set_output(ConvWindow *w, const char *path, gboolean overwrite);
GtkWindow  *conv_window_edit_muxer_options(ConvWindow *w, OptionEditor **editor);

gboolean    conv_window_can_convert(ConvWindow *w);            /* Convert is enabled */
void        conv_window_convert(ConvWindow *w);                 /* press Convert */
char       *conv_window_command(ConvWindow *w);                 /* the command shown (g_free) */

/* The job as currently chosen (job_free it), NULL before a file is open;
 * and the input it applies to. */
ConvJob    *conv_window_build_job(ConvWindow *w);
const MediaInfo *conv_window_media(ConvWindow *w);

/* Open (or bring up) the preview window; returns it. */
GtkWindow  *conv_window_open_preview(ConvWindow *w);

#endif /* UI_WINDOW_H */

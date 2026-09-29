/*
 * progress.h - the conversion window: validates the job (dry run) and runs it
 * on a worker thread, showing progress, FFmpeg's log, and the result.
 */
#ifndef UI_PROGRESS_H
#define UI_PROGRESS_H

#include <gtk/gtk.h>

#include "caps.h"
#include "engine.h"
#include "job.h"

typedef void (*ProgressFinished)(int ret, void *user);

/* Takes ownership of `job`. `caps` must outlive the conversion (read-only).
 * `finished` is called on the main thread when the worker is done. */
GtkWindow *progress_start(GtkWindow *parent, ConvJob *job, const Caps *caps,
                          ProgressFinished finished, void *user);

/* Events, for code that observes the conversion (main thread). */
typedef struct ProgressListener {
    void (*progress)(GtkWindow *win, const EngineProgress *p, void *user);
    /* The window shows the result (called before the owner's `finished`). */
    void (*finished)(GtkWindow *win, int ret, void *user);
} ProgressListener;

/* One listener per window; NULL removes it. */
void progress_set_listener(GtkWindow *win, const ProgressListener *l, void *user);

/* What the user can do: press Cancel, open or close the FFmpeg log. */
void progress_cancel(GtkWindow *win);
void progress_show_log(GtkWindow *win, gboolean shown);

#endif /* UI_PROGRESS_H */

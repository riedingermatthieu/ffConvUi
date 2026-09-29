/*
 * progress.h - the conversion window: validates the job (dry run) and runs it
 * on a worker thread, showing progress, FFmpeg's log, and the result.
 */
#ifndef UI_PROGRESS_H
#define UI_PROGRESS_H

#include <gtk/gtk.h>

#include "caps.h"
#include "job.h"

typedef void (*ProgressFinished)(int ret, void *user);

/* Takes ownership of `job`. `caps` must outlive the conversion (read-only).
 * `finished` is called on the main thread when the worker is done. */
GtkWindow *progress_start(GtkWindow *parent, ConvJob *job, const Caps *caps,
                          ProgressFinished finished, void *user);

#endif /* UI_PROGRESS_H */

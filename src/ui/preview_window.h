/*
 * preview_window.h - one video frame before and after conversion.
 *
 * Follows the main window: any change there (encoder, options, container...)
 * renders the preview again. Rendering runs on a worker thread; a render
 * that became outdated is cancelled.
 */
#ifndef UI_PREVIEW_WINDOW_H
#define UI_PREVIEW_WINDOW_H

#include <gtk/gtk.h>

typedef struct ConvWindow ConvWindow;

GtkWindow *preview_window_new(ConvWindow *owner);

/* Events, for code that observes the window (main thread). */
typedef struct PreviewWindowListener {
    /* A preview is shown (ok) or failed (the window says why). */
    void (*rendered)(GtkWindow *win, gboolean ok, void *user);
} PreviewWindowListener;

void preview_window_set_listener(GtkWindow *win, const PreviewWindowListener *l, void *user);

/* What the user can do: move the time slider, show actual pixels. */
void preview_window_set_time(GtkWindow *win, double seconds);
void preview_window_set_actual_pixels(GtkWindow *win, gboolean actual);

#endif /* UI_PREVIEW_WINDOW_H */

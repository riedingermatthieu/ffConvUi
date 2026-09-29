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

/* What the user can do: move the time slider; zoom (scale of the original
 * frame: 1 = actual pixels, 0 = fit the view); pan (the point of the frame
 * to put in the middle of the view, as fractions of its width and height;
 * kept for the first frame if none is shown yet). */
void   preview_window_set_time(GtkWindow *win, double seconds);
void   preview_window_set_zoom(GtkWindow *win, double zoom);
double preview_window_get_zoom(GtkWindow *win);
void   preview_window_center_on(GtkWindow *win, double fx, double fy);

#endif /* UI_PREVIEW_WINDOW_H */

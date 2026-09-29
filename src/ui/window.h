/*
 * window.h - main window: input file, stream table, container, output file,
 * live validation and the Convert button.
 */
#ifndef UI_WINDOW_H
#define UI_WINDOW_H

#include <gtk/gtk.h>

#include "caps.h"

typedef struct ConvWindow ConvWindow;

/* `caps` must outlive the window. */
ConvWindow *conv_window_new(GtkApplication *app, const Caps *caps);
GtkWindow  *conv_window_get(ConvWindow *w);
void        conv_window_open(ConvWindow *w, GFile *file);

#endif /* UI_WINDOW_H */

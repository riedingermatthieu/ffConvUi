/*
 * uilog.h - show FFmpeg's log in the UI.
 *
 * Installs logcap's fallback so every FFmpeg message (from any thread) that
 * passes av_log_get_level() is appended, on the main thread, to the current
 * text buffer (the progress window's log). Messages arriving while no buffer
 * is set are dropped.
 */
#ifndef UI_LOG_H
#define UI_LOG_H

#include <gtk/gtk.h>

void uilog_init(void);

/* Main thread only. NULL detaches the current buffer. */
void uilog_set_buffer(GtkTextBuffer *buffer);

/* Append a line of our own (main thread). */
void uilog_append(const char *text);

#endif /* UI_LOG_H */

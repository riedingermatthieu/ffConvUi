/*
 * ui_util.h - small helpers shared by the UI modules.
 */
#ifndef UI_UTIL_H
#define UI_UTIL_H

#include <stdint.h>

#include <gtk/gtk.h>

/* "1:02:03", "02:03" (seconds < 0: "--:--") */
void ui_format_time(double seconds, char *buf, size_t size);
/* "12.3 MiB" */
void ui_format_size(int64_t bytes, char *buf, size_t size);

#endif /* UI_UTIL_H */

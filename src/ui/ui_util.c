/*
 * ui_util.c - UI helpers.
 */
#include "ui_util.h"

#include <math.h>

void ui_format_time(double seconds, char *buf, size_t size)
{
    long s;

    if (seconds < 0 || isnan(seconds)) {
        g_strlcpy(buf, "--:--", size);
        return;
    }
    s = lround(seconds);
    if (s >= 3600)
        g_snprintf(buf, size, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else
        g_snprintf(buf, size, "%02ld:%02ld", s / 60, s % 60);
}

void ui_format_size(int64_t bytes, char *buf, size_t size)
{
    char *s = g_format_size_full(bytes > 0 ? (guint64)bytes : 0, G_FORMAT_SIZE_IEC_UNITS);
    g_strlcpy(buf, s, size);
    g_free(s);
}


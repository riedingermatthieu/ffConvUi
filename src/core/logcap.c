/*
 * logcap.c - per-thread capture of FFmpeg log messages.
 */
#include "logcap.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/avstring.h>
#include <libavutil/log.h>

static _Thread_local LogCapture *tl_capture;
static _Atomic(LogCallback) g_fallback;
static atomic_int g_installed;

static void capture_message(LogCapture *c, void *avcl, int level, const char *fmt, va_list vl)
{
    char chunk[512];
    int print_prefix = 0;
    size_t len;

    av_log_format_line2(NULL, level, fmt, vl, chunk, sizeof(chunk), &print_prefix);

    /* start of a message: prefix it with the component name ("libx264: ") */
    if (!c->pending[0] && avcl && *(const AVClass **)avcl) {
        const AVClass *cls = *(const AVClass **)avcl;
        const char *name = cls->item_name ? cls->item_name(avcl) : cls->class_name;
        if (name && *name)
            snprintf(c->pending, sizeof(c->pending), "%s: ", name);
    }
    av_strlcat(c->pending, chunk, sizeof(c->pending));

    len = strlen(c->pending);
    if (!len || c->pending[len - 1] != '\n')
        return;                           /* continued by the next av_log call */
    while (len && (c->pending[len - 1] == '\n' || c->pending[len - 1] == ' ' ||
                   c->pending[len - 1] == '\r'))
        c->pending[--len] = '\0';
    if (len)
        av_strlcpy(c->last, c->pending, sizeof(c->last));
    if (len && !c->first[0])
        av_strlcpy(c->first, c->pending, sizeof(c->first));
    c->pending[0] = '\0';
}

static void callback(void *avcl, int level, const char *fmt, va_list vl)
{
    LogCapture *c = tl_capture;
    LogCallback fallback;

    if (c && level <= c->level) {
        va_list copy;
        va_copy(copy, vl);
        capture_message(c, avcl, level, fmt, copy);
        va_end(copy);
        if (!c->forward)
            return;
    }
    fallback = atomic_load(&g_fallback);
    (fallback ? fallback : av_log_default_callback)(avcl, level, fmt, vl);
}

void logcap_install(LogCallback fallback)
{
    atomic_store(&g_fallback, fallback);
    if (!atomic_exchange(&g_installed, 1))
        av_log_set_callback(callback);
}

void logcap_ensure_installed(void)
{
    if (!atomic_exchange(&g_installed, 1))
        av_log_set_callback(callback);
}

void logcap_begin(LogCapture *c)
{
    logcap_clear(c);
    tl_capture = c;
}

void logcap_end(void)
{
    tl_capture = NULL;
}

void logcap_clear(LogCapture *c)
{
    c->first[0]   = '\0';
    c->last[0]    = '\0';
    c->pending[0] = '\0';
}

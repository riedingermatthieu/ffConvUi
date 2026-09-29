/*
 * uilog.c - FFmpeg log -> GtkTextBuffer.
 */
#include "uilog.h"

#include <libavutil/log.h>

#include "logcap.h"

#define MAX_LOG_CHARS (256 * 1024)

static GtkTextBuffer *g_buffer;      /* main thread only */
static _Thread_local int print_prefix = 1;

void uilog_append(const char *text)
{
    GtkTextIter end;
    int chars;

    if (!g_buffer)
        return;
    gtk_text_buffer_get_end_iter(g_buffer, &end);
    gtk_text_buffer_insert(g_buffer, &end, text, -1);

    /* keep the log bounded: drop the oldest lines */
    chars = gtk_text_buffer_get_char_count(g_buffer);
    if (chars > MAX_LOG_CHARS) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(g_buffer, &start);
        gtk_text_buffer_get_iter_at_offset(g_buffer, &cut, chars - MAX_LOG_CHARS / 2);
        gtk_text_iter_forward_line(&cut);
        gtk_text_buffer_delete(g_buffer, &start, &cut);
    }
}

static gboolean append_idle(gpointer data)
{
    uilog_append(data);
    return G_SOURCE_REMOVE;
}

/* any thread */
static void fallback(void *avcl, int level, const char *fmt, va_list vl)
{
    char line[1024];

    if (level > av_log_get_level())
        return;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &print_prefix);
    if (line[0])
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, append_idle, g_strdup(line), g_free);
}

void uilog_init(void)
{
    logcap_install(fallback);
}

void uilog_set_buffer(GtkTextBuffer *buffer)
{
    if (buffer)
        g_object_ref(buffer);
    if (g_buffer)
        g_object_unref(g_buffer);
    g_buffer = buffer;
}

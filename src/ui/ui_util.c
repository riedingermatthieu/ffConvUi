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

gboolean ui_save_snapshot(GtkWidget *widget, const char *path)
{
    int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    GdkPaintable *paintable;
    GtkSnapshot *snapshot;
    GskRenderNode *node;
    GskRenderer *renderer;
    GdkTexture *texture;
    GtkNative *native;
    gboolean ok;

    if (w <= 0 || h <= 0 || !(native = gtk_widget_get_native(widget)) ||
        !(renderer = gtk_native_get_renderer(native)))
        return FALSE;

    paintable = gtk_widget_paintable_new(widget);
    snapshot  = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), w, h);
    node = gtk_snapshot_free_to_node(snapshot);
    if (!node) {
        g_object_unref(paintable);
        return FALSE;
    }
    texture = gsk_renderer_render_texture(renderer, node, &GRAPHENE_RECT_INIT(0, 0, w, h));
    ok = gdk_texture_save_to_png(texture, path);

    g_object_unref(texture);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
    return ok;
}

const char *ui_test_env(const char *name)
{
    const char *v = g_getenv(name);
    return v && *v ? v : NULL;
}

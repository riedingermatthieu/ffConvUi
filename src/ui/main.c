/*
 * ffconv - GTK 4 front end for the converter core (milestones M5-M6).
 *
 *   ffconv [file]
 *
 * Test builds (CONV_TEST_HOOKS) can be driven by FFCONV_TEST_* environment
 * variables: see testhooks.c.
 */
#include <locale.h>
#include <stdio.h>

#include <gtk/gtk.h>
#include <libavutil/error.h>
#include <libavutil/log.h>

#include "caps.h"
#include "testhooks.h"
#include "uilog.h"
#include "window.h"

/* also the icon name: see src/ui/ffconv.gresource.xml */
#define APP_ID "io.github.ffconv.FFConv"

static Caps *g_caps;

static void on_startup(GApplication *app, gpointer data)
{
    int ret;

    /* GTK sets the locale from the environment; FFmpeg parses option values
     * such as "23.5" with strtod(), which would expect "23,5" in French. */
    setlocale(LC_NUMERIC, "C");

    /* every window (main, options, progress) gets the app icon, which
     * GtkApplication finds in the compiled-in resources */
    gtk_window_set_default_icon_name(APP_ID);

    av_log_set_level(AV_LOG_INFO);
    uilog_init();

    /* GTK's default theme styles .error but not .success/.warning labels */
    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_string(css,
            "label.success, image.success { color: #26a269; }\n"
            "label.warning, image.warning { color: #c64600; }\n"
            "image.error { color: #c01c28; }\n");
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }

    if ((ret = caps_build(&g_caps)) < 0) {
        g_printerr("cannot enumerate the FFmpeg build: %s\n", av_err2str(ret));
        g_application_quit(app);
        return;
    }

    testhooks_install(GTK_APPLICATION(app));   /* a no-op unless built with CONV_TEST_HOOKS */
}

static ConvWindow *new_window(GApplication *app)
{
    ConvWindow *w = conv_window_new(GTK_APPLICATION(app), g_caps);

    gtk_window_present(conv_window_get(w));
    return w;
}

static void on_activate(GApplication *app, gpointer data)
{
    if (g_caps)
        new_window(app);
}

static void on_open(GApplication *app, GFile **files, int nb, const char *hint, gpointer data)
{
    if (g_caps && nb > 0)
        conv_window_open(new_window(app), files[0]);
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new(APP_ID,
                                              G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    int status;

    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    caps_free(&g_caps);
    return status;
}

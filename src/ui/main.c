/*
 * ffconv - GTK 4 front end for the converter core (milestones M5-M6).
 *
 *   ffconv [file]
 *
 * Test hooks (environment variables), used to check the UI without a user:
 *   FFCONV_TEST_INPUT=<file>          open this file at startup
 *   FFCONV_TEST_CONTAINER=<key>       then select this container
 *   FFCONV_TEST_STREAMS=0=libx264,1=copy,2=drop   then set the streams' actions
 *   FFCONV_TEST_OPTIONS=0:crf=30;0:preset=fast;mux:movflags=+faststart
 *                                     then set options through the option dialogs
 *   FFCONV_TEST_OPTIONS_SHOT=<png>    save the first option dialog to a PNG
 *   FFCONV_TEST_OUTPUT=<file>         then set this output (and "overwrite")
 *   FFCONV_TEST_SHOT=<png>            then save the main window to a PNG
 *   FFCONV_TEST_CONVERT=1             then click Convert, and quit when done
 *   FFCONV_TEST_JOB_JSON=<file>       write the job Convert runs, as JSON
 *   FFCONV_TEST_PROGRESS_SHOT=<png>   save the progress window at >= 30 %
 *   FFCONV_TEST_DONE_SHOT=<png>       save the progress window when done
 *   FFCONV_TEST_CANCEL_AT=<percent>   press Cancel once progress reaches it
 *   FFCONV_TEST_EXPAND_LOG=1          open the FFmpeg log in the progress window
 * Without FFCONV_TEST_CONVERT, the application quits after the shot.
 */
#include <locale.h>
#include <stdio.h>

#include <gtk/gtk.h>
#include <libavutil/error.h>
#include <libavutil/log.h>

#include "caps.h"
#include "uilog.h"
#include "ui_util.h"
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
    }
}

static ConvWindow *new_window(GApplication *app)
{
    ConvWindow *w = conv_window_new(GTK_APPLICATION(app), g_caps);

    gtk_window_present(conv_window_get(w));
    return w;
}

static gboolean empty_shot(gpointer data)
{
    GtkWindow *win = conv_window_get(data);

    ui_save_snapshot(GTK_WIDGET(win), ui_test_env("FFCONV_TEST_SHOT"));
    g_application_quit(G_APPLICATION(gtk_window_get_application(win)));
    return G_SOURCE_REMOVE;
}

static void on_activate(GApplication *app, gpointer data)
{
    const char *input = ui_test_env("FFCONV_TEST_INPUT");
    ConvWindow *w;

    if (!g_caps)
        return;
    w = new_window(app);
    if (input) {
        GFile *file = g_file_new_for_path(input);
        conv_window_open(w, file);
        g_object_unref(file);
    } else if (ui_test_env("FFCONV_TEST_SHOT")) {
        g_timeout_add(800, empty_shot, w);   /* the window with no file */
    }
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

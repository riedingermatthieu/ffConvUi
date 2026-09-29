/*
 * testhooks.c - drive the UI from FFCONV_TEST_* environment variables.
 *
 * Used by tests/ui_test.cmake to check the UI without a user or a screen
 * capture: windows render themselves to PNG files. The steps run in this
 * order, each one given time for the window to update (rows, validation):
 *
 *   FFCONV_TEST_INPUT=<file>          open this file at startup
 *   FFCONV_TEST_CONTAINER=<key>       select this container
 *   FFCONV_TEST_STREAMS=0=libx264,1=copy,2=drop
 *                                     set the streams' actions / encoders
 *   FFCONV_TEST_OPTIONS=0:crf=30;0:preset=fast;mux:movflags=+faststart
 *                                     set options through the option dialogs
 *   FFCONV_TEST_OPTIONS_SHOT=<png>    save the first option dialog
 *   FFCONV_TEST_OUTPUT=<file>         set the output (and "overwrite")
 *   FFCONV_TEST_COMMAND_FILE=<file>   write the ffmpeg command shown
 *   FFCONV_TEST_SHOT=<png>            save the main window (without INPUT:
 *                                     the empty window, then quit)
 *   FFCONV_TEST_STATES_FILE=<file>    write every dropdown entry and its
 *                                     greyed state
 *   FFCONV_TEST_POPUP=container|action<N>|encoder<N>
 *   FFCONV_TEST_POPUP_SHOT=<png>      open that list, save it, and quit
 *   FFCONV_TEST_PREVIEW=<seconds>     open the preview at that time and, once
 *   FFCONV_TEST_PREVIEW_SHOT=<png>    rendered, save it, and quit
 *   FFCONV_TEST_PREVIEW_ZOOM=1        ... with "Actual pixels" on
 *   FFCONV_TEST_CONVERT=1             press Convert, and quit when done
 *   FFCONV_TEST_JOB_JSON=<file>       write the job Convert runs, as JSON
 *   FFCONV_TEST_EXPAND_LOG=1          open the FFmpeg log in the progress window
 *   FFCONV_TEST_PROGRESS_SHOT=<png>   save the progress window at >= 30 %
 *   FFCONV_TEST_CANCEL_AT=<percent>   press Cancel once progress reaches it
 *   FFCONV_TEST_DONE_SHOT=<png>       save the progress window when done
 *
 * Without FFCONV_TEST_CONVERT (or a popup), the application quits after the
 * last step.
 */
#include "testhooks.h"

#include <stdlib.h>
#include <string.h>

#include <libavutil/bprint.h>

#include "choice_item.h"
#include "job.h"
#include "option_editor.h"
#include "preview_window.h"
#include "progress.h"
#include "stream_row.h"
#include "window.h"

#define STEP_DELAY_MS 600   /* let rows, the container list and validation update */

typedef struct Driver {
    GtkApplication *app;
    ConvWindow     *w;
    GtkWindow      *dialog;             /* option dialog being filled in */
    int             options_shot_taken;
    int             progress_shot_taken;
} Driver;

static Driver g_driver;

static gboolean step(gpointer data);

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */

static const char *env(const char *name)
{
    const char *v = g_getenv(name);
    return v && *v ? v : NULL;
}

/* Take a step's variable: each step runs once. */
static char *take(const char *name)
{
    char *v = g_strdup(env(name));

    g_unsetenv(name);
    return v;
}

/* Render a realized widget (window, popover) to a PNG file. */
static gboolean save_snapshot(GtkWidget *widget, const char *path)
{
    int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    GdkPaintable *paintable;
    GtkSnapshot *snapshot;
    GskRenderNode *node;
    GskRenderer *renderer;
    GdkTexture *texture;
    GtkNative *native;
    gboolean ok;

    if (!path || w <= 0 || h <= 0 || !(native = gtk_widget_get_native(widget)) ||
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

static gboolean quit(gpointer data)
{
    g_application_quit(G_APPLICATION(g_driver.app));
    return G_SOURCE_REMOVE;
}

static void quit_soon(void)
{
    g_timeout_add(100, quit, NULL);
}

static void next_step(guint delay)
{
    g_timeout_add(delay, step, NULL);
}

/* ------------------------------------------------------------------------- */
/* streams and options                                                       */

/* "0=libx264,1=copy,2=drop": input stream -> copy / drop / encoder */
static void set_streams(const char *spec)
{
    char **items = g_strsplit(spec, ",", -1);

    for (char **it = items; *it; it++) {
        char **kv = g_strsplit(*it, "=", 2);
        StreamRow *row = kv[0] && kv[1] ? conv_window_stream(g_driver.w, atoi(kv[0])) : NULL;

        if (row) {
            if (!strcmp(kv[1], "copy"))
                stream_row_select(row, JOB_COPY, NULL);
            else if (!strcmp(kv[1], "drop"))
                stream_row_select(row, JOB_DROP, NULL);
            else if (!stream_row_select(row, JOB_TRANSCODE, kv[1]))
                g_printerr("test: stream %s has no encoder %s\n", kv[0], kv[1]);
        }
        g_strfreev(kv);
    }
    g_strfreev(items);
}

static gboolean close_dialog(gpointer data)
{
    const char *shot = env("FFCONV_TEST_OPTIONS_SHOT");

    if (g_driver.dialog) {
        if (shot && !g_driver.options_shot_taken++)      /* the first dialog only */
            save_snapshot(GTK_WIDGET(g_driver.dialog), shot);
        gtk_window_destroy(g_driver.dialog);
        g_driver.dialog = NULL;
    }
    next_step(400);
    return G_SOURCE_REMOVE;
}

/* "0:crf=30;0:preset=veryfast;mux:movflags=+faststart": open the option
 * dialog of the first target (stream index or "mux"), set its values through
 * the widgets, close it; the other targets are left for the next step. */
static void set_options_step(void)
{
    char **items = g_strsplit(env("FFCONV_TEST_OPTIONS"), ";", -1);
    GString *rest = g_string_new(NULL);
    OptionEditor *ed = NULL;
    char *target = NULL;

    for (char **it = items; *it; it++) {
        const char *colon = strchr(*it, ':'), *eq;
        char *tgt;

        if (!colon || !(eq = strchr(colon, '=')))
            continue;
        tgt = g_strndup(*it, colon - *it);
        if (!target) {
            target = g_strdup(tgt);
            if (!strcmp(target, "mux")) {
                g_driver.dialog = conv_window_edit_muxer_options(g_driver.w, &ed);
            } else {
                StreamRow *row = conv_window_stream(g_driver.w, atoi(target));
                g_driver.dialog = row ? stream_row_edit_options(row, &ed) : NULL;
            }
        }
        if (strcmp(tgt, target)) {
            g_string_append_printf(rest, "%s%s", rest->len ? ";" : "", *it);
        } else if (ed) {
            char *name = g_strndup(colon + 1, eq - colon - 1);
            if (!option_editor_set_text(ed, name, eq + 1))
                g_printerr("test: cannot set %s=%s\n", name, eq + 1);
            g_free(name);
        }
        g_free(tgt);
    }
    if (rest->len)
        g_setenv("FFCONV_TEST_OPTIONS", rest->str, TRUE);
    else
        g_unsetenv("FFCONV_TEST_OPTIONS");
    g_string_free(rest, TRUE);
    g_free(target);
    g_strfreev(items);
    g_timeout_add(500, close_dialog, NULL);
}

/* ------------------------------------------------------------------------- */
/* checks: dropdown states, popups                                           */

/* One line per entry: "<list> <on|off> <label> | <reason>" */
static void dump_store(GString *out, const char *list, GListModel *model)
{
    guint n = g_list_model_get_n_items(model);

    for (guint i = 0; i < n; i++) {
        ConvChoiceItem *it = g_list_model_get_item(model, i);
        gboolean on = conv_choice_item_get_enabled(it);
        g_string_append_printf(out, "%s %s %s%s%s\n", list, on ? "on " : "off",
                               conv_choice_item_get_label(it), on ? "" : " | ",
                               on ? "" : conv_choice_item_get_reason(it));
        g_object_unref(it);
    }
}

static void write_states(const char *path)
{
    ConvWindow *w = g_driver.w;
    GString *out = g_string_new(NULL);
    int n = conv_window_nb_streams(w);

    g_string_append_printf(out, "selected container %s\n",
                           conv_window_container_key(w) ? conv_window_container_key(w) : "-");
    for (int i = 0; i < n; i++) {
        StreamRow *row = conv_window_stream_at(w, i);
        g_string_append_printf(out, "selected #%d %s %s\n", stream_row_input_index(row),
                               job_action_name(stream_row_action(row)),
                               stream_row_encoder(row) ? stream_row_encoder(row) : "-");
    }
    g_string_append_printf(out, "convert %s\n", conv_window_can_convert(w) ? "enabled" : "disabled");
    dump_store(out, "container", gtk_drop_down_get_model(GTK_DROP_DOWN(conv_window_container_dropdown(w))));
    for (int i = 0; i < n; i++) {
        StreamRow *row = conv_window_stream_at(w, i);
        char name[32];
        g_snprintf(name, sizeof(name), "action#%d", stream_row_input_index(row));
        dump_store(out, name, gtk_drop_down_get_model(GTK_DROP_DOWN(stream_row_action_dropdown(row))));
        g_snprintf(name, sizeof(name), "encoder#%d", stream_row_input_index(row));
        dump_store(out, name, gtk_drop_down_get_model(GTK_DROP_DOWN(stream_row_encoder_dropdown(row))));
    }
    g_file_set_contents(path, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

/* "container", "action<N>" or "encoder<N>" */
static GtkWidget *popup_dropdown(const char *which)
{
    ConvWindow *w = g_driver.w;
    StreamRow *row;

    if (!strcmp(which, "container"))
        return conv_window_container_dropdown(w);
    if (!strncmp(which, "action", 6) && (row = conv_window_stream(w, atoi(which + 6))))
        return stream_row_action_dropdown(row);
    if (!strncmp(which, "encoder", 7) && (row = conv_window_stream(w, atoi(which + 7))))
        return stream_row_encoder_dropdown(row);
    return NULL;
}

static gboolean popup_shot(gpointer data)
{
    GtkWidget *dd = data;

    /* the list is a popover, a child of the dropdown with its own surface */
    for (GtkWidget *c = gtk_widget_get_first_child(dd); c; c = gtk_widget_get_next_sibling(c))
        if (GTK_IS_POPOVER(c))
            save_snapshot(c, env("FFCONV_TEST_POPUP_SHOT"));
    quit_soon();
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */
/* preview                                                                   */

static gboolean preview_shot(gpointer data)
{
    save_snapshot(GTK_WIDGET(data), env("FFCONV_TEST_PREVIEW_SHOT"));
    g_object_unref(data);
    quit_soon();
    return G_SOURCE_REMOVE;
}

static void on_preview_rendered(GtkWindow *win, gboolean ok, void *user)
{
    preview_window_set_listener(win, NULL, NULL);        /* the first result only */
    g_timeout_add(300, preview_shot, g_object_ref(win));  /* after it is drawn */
}

static const PreviewWindowListener preview_listener = { .rendered = on_preview_rendered };

static void open_preview(const char *seconds)
{
    GtkWindow *win = conv_window_open_preview(g_driver.w);

    if (!win) {
        g_printerr("test: no preview\n");
        quit_soon();
        return;
    }
    if (env("FFCONV_TEST_PREVIEW_ZOOM"))
        preview_window_set_actual_pixels(win, TRUE);
    preview_window_set_time(win, g_ascii_strtod(seconds, NULL));   /* renders */
    preview_window_set_listener(win, &preview_listener, NULL);
}

/* ------------------------------------------------------------------------- */
/* the steps                                                                 */

static gboolean step(gpointer data)
{
    ConvWindow *w = g_driver.w;
    char *v;

    if ((v = take("FFCONV_TEST_CONTAINER"))) {
        if (!conv_window_select_container(w, v))
            g_printerr("test: no container %s\n", v);
        g_free(v);
        next_step(STEP_DELAY_MS);
        return G_SOURCE_REMOVE;
    }
    if ((v = take("FFCONV_TEST_STREAMS"))) {
        set_streams(v);
        g_free(v);
        next_step(STEP_DELAY_MS);
        return G_SOURCE_REMOVE;
    }
    if (env("FFCONV_TEST_OPTIONS")) {        /* one dialog per step */
        set_options_step();
        return G_SOURCE_REMOVE;
    }
    if ((v = take("FFCONV_TEST_OUTPUT"))) {
        conv_window_set_output(w, v, TRUE);
        g_free(v);
        next_step(STEP_DELAY_MS);
        return G_SOURCE_REMOVE;
    }

    /* the window is set up: record, then convert or quit */
    if (env("FFCONV_TEST_COMMAND_FILE")) {
        char *cmd = conv_window_command(w);
        g_file_set_contents(env("FFCONV_TEST_COMMAND_FILE"), cmd, -1, NULL);
        g_free(cmd);
    }
    save_snapshot(GTK_WIDGET(conv_window_get(w)), env("FFCONV_TEST_SHOT"));
    if (env("FFCONV_TEST_STATES_FILE"))
        write_states(env("FFCONV_TEST_STATES_FILE"));

    if (env("FFCONV_TEST_PREVIEW")) {
        open_preview(env("FFCONV_TEST_PREVIEW"));
    } else if (env("FFCONV_TEST_POPUP")) {
        GtkWidget *dd = popup_dropdown(env("FFCONV_TEST_POPUP"));
        if (dd) {
            g_signal_emit_by_name(dd, "activate");   /* pops the list up */
            g_timeout_add(800, popup_shot, dd);
        } else {
            g_printerr("test: no list %s\n", env("FFCONV_TEST_POPUP"));
            quit_soon();
        }
    } else if (env("FFCONV_TEST_CONVERT") && conv_window_can_convert(w)) {
        conv_window_convert(w);                      /* quits when the conversion is done */
    } else {
        quit_soon();
    }
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */
/* events                                                                    */

static gboolean progress_shot(gpointer data)
{
    save_snapshot(GTK_WIDGET(data), env("FFCONV_TEST_PROGRESS_SHOT"));
    g_object_unref(data);
    return G_SOURCE_REMOVE;
}

static void on_progress(GtkWindow *win, const EngineProgress *p, void *user)
{
    const char *cancel_at = env("FFCONV_TEST_CANCEL_AT");

    if (cancel_at && p->percent >= atof(cancel_at))
        progress_cancel(win);
    if (!g_driver.progress_shot_taken && p->percent >= 30 && env("FFCONV_TEST_PROGRESS_SHOT")) {
        g_driver.progress_shot_taken = 1;
        g_timeout_add(200, progress_shot, g_object_ref(win));   /* after the labels redraw */
    }
}

static gboolean done_shot(gpointer data)
{
    save_snapshot(GTK_WIDGET(data), env("FFCONV_TEST_DONE_SHOT"));
    g_object_unref(data);
    quit_soon();
    return G_SOURCE_REMOVE;
}

static void on_progress_finished(GtkWindow *win, int ret, void *user)
{
    /* let the window draw the result first */
    g_timeout_add(300, done_shot, g_object_ref(win));
}

static const ProgressListener progress_listener = {
    .progress = on_progress,
    .finished = on_progress_finished,
};

static void on_file_loaded(ConvWindow *w, void *user)
{
    next_step(800);
}

static void on_conversion_started(ConvWindow *w, const ConvJob *job, GtkWindow *progress, void *user)
{
    if (env("FFCONV_TEST_JOB_JSON")) {   /* what the widgets produced */
        AVBPrint bp;
        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        job_to_json(job, &bp);
        g_file_set_contents(env("FFCONV_TEST_JOB_JSON"), bp.str, bp.len, NULL);
        av_bprint_finalize(&bp, NULL);
    }
    if (env("FFCONV_TEST_EXPAND_LOG"))
        progress_show_log(progress, TRUE);
    progress_set_listener(progress, &progress_listener, NULL);
}

static const ConvWindowListener window_listener = {
    .file_loaded        = on_file_loaded,
    .conversion_started = on_conversion_started,
};

static gboolean empty_shot(gpointer data)
{
    save_snapshot(GTK_WIDGET(conv_window_get(g_driver.w)), env("FFCONV_TEST_SHOT"));
    quit_soon();
    return G_SOURCE_REMOVE;
}

static gboolean open_input(gpointer data)
{
    GFile *file = g_file_new_for_path(env("FFCONV_TEST_INPUT"));

    conv_window_open(g_driver.w, file);   /* -> on_file_loaded */
    g_object_unref(file);
    return G_SOURCE_REMOVE;
}

static gboolean attach(gpointer data)
{
    GtkWindow *win = data;
    ConvWindow *w = conv_window_from_window(win);

    if (w && !g_driver.w) {   /* the first main window only (not dialogs) */
        g_driver.w = w;
        conv_window_add_listener(w, &window_listener, NULL);
        if (env("FFCONV_TEST_INPUT"))
            open_input(NULL);
        else if (env("FFCONV_TEST_SHOT"))
            g_timeout_add(800, empty_shot, NULL);    /* the window with no file */
    }
    g_object_unref(win);
    return G_SOURCE_REMOVE;
}

static void on_window_added(GtkApplication *app, GtkWindow *win, gpointer data)
{
    /* GtkApplication adds a window while it is being constructed, before
     * conv_window_new() has finished: look it up once that is done */
    g_idle_add(attach, g_object_ref(win));
}

void testhooks_install(GtkApplication *app)
{
    static const char *const vars[] = { "FFCONV_TEST_INPUT", "FFCONV_TEST_SHOT", NULL };
    int any = 0;

    for (const char *const *v = vars; *v; v++)
        any |= env(*v) != NULL;
    if (!any)
        return;   /* a normal run */
    g_driver.app = app;
    g_signal_connect(app, "window-added", G_CALLBACK(on_window_added), NULL);
}

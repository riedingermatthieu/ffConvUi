/*
 * window.c - main window.
 *
 * State flow: open a file -> probe it (worker thread) -> list the containers
 * that can hold its streams -> one row per stream with the choices the
 * selected container allows -> every change re-runs the static validation
 * (debounced) -> Convert runs the dry run and the conversion in the
 * progress window.
 */
#include "window.h"

#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/avstring.h>

#include "job.h"
#include "probe.h"
#include "progress.h"
#include "stream_row.h"
#include "ui_util.h"
#include "validate.h"

#define VALIDATE_DELAY_MS 250
#define MAX_SHOWN_ISSUES  8

struct ConvWindow {
    GtkApplication  *app;
    GtkWindow       *win;
    const Caps      *caps;
    MediaInfo       *mi;
    const CapsMuxer *mux;

    GtkWidget       *input_label, *summary_label;
    GtkWidget       *stream_list;
    GtkWidget       *output_grid;
    GtkWidget       *container_dd;
    const CapsMuxer **containers;      /* dropdown position -> muxer */
    int              nb_containers;
    GtkWidget       *output_entry, *overwrite_check;
    GtkWidget       *issues_box;
    GtkWidget       *convert_btn;
    GtkWindow       *progress_win;

    StreamRow      **rows;
    int              nb_rows;
    guint            validate_id;
    int              nb_errors;
    int              converting;
    int              loading;
    int              updating;         /* programmatic widget changes: no callbacks */
};

static void schedule_validate(ConvWindow *w);

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */

static GtkWidget *heading(const char *text)
{
    GtkWidget *l = gtk_label_new(NULL);
    char *markup = g_markup_printf_escaped("<b>%s</b>", text);

    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    g_free(markup);
    return l;
}

static void alert(ConvWindow *w, const char *message, const char *detail)
{
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", message);

    if (detail)
        gtk_alert_dialog_set_detail(d, detail);
    gtk_alert_dialog_show(d, w->win);
    g_object_unref(d);
}

static const char *muxer_ext(const CapsMuxer *m)
{
    return m->extensions && *m->extensions ? m->extensions : NULL;
}

static void clear_box(GtkWidget *box)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(box)))
        gtk_box_remove(GTK_BOX(box), child);
}

/* ------------------------------------------------------------------------- */
/* job <- widgets                                                            */

static ConvJob *build_job(ConvWindow *w)
{
    ConvJob *job;

    if (!w->mi || !w->mux || !(job = job_alloc()))
        return NULL;
    job->input     = av_strdup(w->mi->path);
    job->output    = av_strdup(gtk_editable_get_text(GTK_EDITABLE(w->output_entry)));
    job->muxer     = av_strdup(w->mux->key);
    job->overwrite = gtk_check_button_get_active(GTK_CHECK_BUTTON(w->overwrite_check));
    if (!job->input || !job->output || !job->muxer)
        goto fail;

    for (int i = 0; i < w->nb_rows; i++) {
        JobAction a = stream_row_action(w->rows[i]);
        JobStream *js;

        if (a == JOB_DROP)
            continue;   /* unlisted streams are dropped */
        if (!(js = job_add_stream(job, stream_row_input_index(w->rows[i]), a)))
            goto fail;
        if (a == JOB_TRANSCODE && !(js->encoder = av_strdup(stream_row_encoder(w->rows[i]))))
            goto fail;
    }
    return job;

fail:
    job_free(&job);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* validation display                                                        */

static void add_issue_line(ConvWindow *w, const char *icon, const char *css, const char *text)
{
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_hexpand(label, TRUE);
    if (css) {
        gtk_widget_add_css_class(img, css);
        gtk_widget_add_css_class(label, css);
    }
    gtk_box_append(GTK_BOX(line), img);
    gtk_box_append(GTK_BOX(line), label);
    gtk_box_append(GTK_BOX(w->issues_box), line);
}

static void update_convert_button(ConvWindow *w)
{
    gtk_widget_set_sensitive(w->convert_btn, w->mi && !w->loading && !w->converting && !w->nb_errors);
}

static gboolean do_validate(gpointer data)
{
    ConvWindow *w = data;
    ConvJob *job;
    ValReport rep;
    int shown = 0;

    w->validate_id = 0;
    clear_box(w->issues_box);
    w->nb_errors = 1;

    if (!w->mi || w->loading) {
        update_convert_button(w);
        return G_SOURCE_REMOVE;
    }
    if (!(job = build_job(w)) || validate_job(job, w->caps, w->mi, 0, &rep) < 0) {
        add_issue_line(w, "dialog-error-symbolic", "error", "Out of memory");
        job_free(&job);
        update_convert_button(w);
        return G_SOURCE_REMOVE;
    }

    /* errors first, then warnings; information is left for the progress window */
    for (int sev = VAL_ERROR; sev >= VAL_WARNING; sev--) {
        for (int i = 0; i < rep.nb_issues; i++) {
            const ValIssue *is = &rep.issues[i];
            char text[1024];

            if ((int)is->severity != sev || shown++ >= MAX_SHOWN_ISSUES)
                continue;
            if (is->stream >= 0)
                g_snprintf(text, sizeof(text), "Stream #%d: %s", job->streams[is->stream].input_index,
                           is->message);
            else
                g_strlcpy(text, is->message, sizeof(text));
            add_issue_line(w, sev == VAL_ERROR ? "dialog-error-symbolic" : "dialog-warning-symbolic",
                           sev == VAL_ERROR ? "error" : "warning", text);
        }
    }
    if (shown > MAX_SHOWN_ISSUES) {
        char text[64];
        g_snprintf(text, sizeof(text), "… and %d more", shown - MAX_SHOWN_ISSUES);
        add_issue_line(w, "view-more-symbolic", "dim-label", text);
    }
    if (!rep.nb_errors && !rep.nb_warnings)
        add_issue_line(w, "object-select-symbolic", "success",
                       "Ready to convert. Convert checks the job once more with a dry run first.");

    w->nb_errors = rep.nb_errors;
    validate_report_free(&rep);
    job_free(&job);
    update_convert_button(w);
    return G_SOURCE_REMOVE;
}

static void schedule_validate(ConvWindow *w)
{
    if (w->validate_id)
        g_source_remove(w->validate_id);
    w->validate_id = g_timeout_add(VALIDATE_DELAY_MS, do_validate, w);
    gtk_widget_set_sensitive(w->convert_btn, FALSE);   /* until the check is done */
}

static void on_row_changed(void *user)
{
    schedule_validate(user);
}

/* ------------------------------------------------------------------------- */
/* stream rows                                                               */

static void rebuild_rows(ConvWindow *w)
{
    int nb = w->mi ? w->mi->nb_streams : 0;
    int *prev_action = g_new0(int, nb ? nb : 1);
    char **prev_encoder = g_new0(char *, nb ? nb : 1);

    /* keep the user's choices where the new container allows them */
    for (int i = 0; i < nb; i++)
        prev_action[i] = -1;
    for (int i = 0; i < w->nb_rows; i++) {
        int idx = stream_row_input_index(w->rows[i]);
        if (idx < nb) {
            prev_action[idx]  = stream_row_action(w->rows[i]);
            prev_encoder[idx] = g_strdup(stream_row_encoder(w->rows[i]));
        }
    }

    gtk_list_box_remove_all(GTK_LIST_BOX(w->stream_list));
    for (int i = 0; i < w->nb_rows; i++)
        stream_row_free(w->rows[i]);
    g_free(w->rows);
    w->rows    = NULL;
    w->nb_rows = 0;

    if (w->mi && w->mux) {
        w->rows = g_new0(StreamRow *, nb ? nb : 1);
        for (int i = 0; i < nb; i++) {
            StreamRow *row = stream_row_new(w->caps, w->mux, &w->mi->streams[i], prev_action[i],
                                            prev_encoder[i], on_row_changed, w);
            GtkWidget *lbrow;

            w->rows[w->nb_rows++] = row;
            gtk_list_box_append(GTK_LIST_BOX(w->stream_list), stream_row_widget(row));
            lbrow = gtk_widget_get_parent(stream_row_widget(row));
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(lbrow), FALSE);
        }
    }
    for (int i = 0; i < nb; i++)
        g_free(prev_encoder[i]);
    g_free(prev_encoder);
    g_free(prev_action);
}

/* ------------------------------------------------------------------------- */
/* containers                                                                */

static void set_output_extension(ConvWindow *w, const CapsMuxer *old, const CapsMuxer *new_mux)
{
    const char *cur = gtk_editable_get_text(GTK_EDITABLE(w->output_entry));
    char *path;

    if (!muxer_ext(new_mux) || !*cur)
        return;
    /* only when the extension was the old container's (not one the user chose) */
    if (old && muxer_ext(old) && !av_match_ext(cur, old->extensions))
        return;
    if (av_match_ext(cur, new_mux->extensions))
        return;
    if ((path = job_replace_extension(cur, new_mux->extensions))) {
        gtk_editable_set_text(GTK_EDITABLE(w->output_entry), path);
        av_free(path);
    }
}

static void on_container_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
    ConvWindow *w = data;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(w->container_dd));
    const CapsMuxer *old = w->mux;

    if (w->updating || i >= (guint)w->nb_containers)
        return;
    w->mux = w->containers[i];
    w->updating = 1;
    set_output_extension(w, old, w->mux);
    w->updating = 0;
    rebuild_rows(w);
    schedule_validate(w);
}

/* Containers that can keep at least one stream, best first, with the one
 * matching the input's extension (or Matroska) selected. */
static void fill_containers(ConvWindow *w)
{
    GtkStringList *model = gtk_string_list_new(NULL);
    const AVOutputFormat *same = av_guess_format(NULL, w->mi->path, NULL);
    CapsMuxerFit *fits = NULL;
    int nb = 0, sel = -1, mkv = -1;

    caps_muxers_for(w->caps, w->mi, &fits, &nb);
    g_free(w->containers);
    w->containers    = g_new0(const CapsMuxer *, nb ? nb : 1);
    w->nb_containers = nb;

    for (int i = 0; i < nb; i++) {
        const CapsMuxerFit *f = &fits[i];
        char label[256];

        if (f->nb_kept < f->nb_considered)
            g_snprintf(label, sizeof(label), "%s — %s  (keeps %d of %d streams)", f->mux->key,
                       f->mux->long_name, f->nb_kept, f->nb_considered);
        else
            g_snprintf(label, sizeof(label), "%s — %s", f->mux->key, f->mux->long_name);
        gtk_string_list_append(model, label);
        w->containers[i] = f->mux;

        if (w->mux && f->mux == w->mux)
            sel = i;                          /* keep the current choice */
        if (sel < 0 && same && f->mux->fmt == same && f->nb_kept == f->nb_considered)
            sel = i;
        if (!strcmp(f->mux->key, "matroska"))
            mkv = i;
    }
    if (sel < 0)
        sel = mkv >= 0 ? mkv : 0;

    w->updating = 1;
    gtk_drop_down_set_model(GTK_DROP_DOWN(w->container_dd), G_LIST_MODEL(model));
    if (nb)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(w->container_dd), sel);
    w->updating = 0;
    w->mux = nb ? w->containers[sel] : NULL;

    g_object_unref(model);
    av_free(fits);
}

/* ------------------------------------------------------------------------- */
/* input                                                                     */

static void show_summary(ConvWindow *w)
{
    char dur[32], size[32], *text;

    ui_format_time(w->mi->duration_us != AV_NOPTS_VALUE ? w->mi->duration_us / 1e6 : -1, dur, sizeof(dur));
    ui_format_size(w->mi->file_size, size, sizeof(size));
    text = g_strdup_printf("%s  ·  %s  ·  %s  ·  %d stream%s", w->mi->format_long_name, dur, size,
                           w->mi->nb_streams, w->mi->nb_streams == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(w->summary_label), text);
    g_free(text);
}

static gboolean test_step_done(gpointer data)
{
    g_application_quit(G_APPLICATION(((ConvWindow *)data)->app));
    return G_SOURCE_REMOVE;
}

static void on_progress_finished(int ret, void *user);
static void start_conversion(ConvWindow *w);

/* FFCONV_TEST_*: drive the window without a user (see main.c) */
static gboolean test_step(gpointer data)
{
    ConvWindow *w = data;
    const char *container = ui_test_env("FFCONV_TEST_CONTAINER");
    const char *output    = ui_test_env("FFCONV_TEST_OUTPUT");
    const char *shot      = ui_test_env("FFCONV_TEST_SHOT");

    if (container) {
        for (int i = 0; i < w->nb_containers; i++)
            if (!strcmp(w->containers[i]->key, container))
                gtk_drop_down_set_selected(GTK_DROP_DOWN(w->container_dd), i);
        g_unsetenv("FFCONV_TEST_CONTAINER");
        g_timeout_add(600, test_step, w);     /* let rows and validation update */
        return G_SOURCE_REMOVE;
    }
    if (ui_test_env("FFCONV_TEST_STREAMS")) {
        /* "0=libx264,1=copy,2=drop": input stream -> copy / drop / encoder */
        char **items = g_strsplit(ui_test_env("FFCONV_TEST_STREAMS"), ",", -1);
        for (char **it = items; *it; it++) {
            char **kv = g_strsplit(*it, "=", 2);
            if (kv[0] && kv[1]) {
                int idx = atoi(kv[0]);
                for (int i = 0; i < w->nb_rows; i++) {
                    if (stream_row_input_index(w->rows[i]) != idx)
                        continue;
                    if (!strcmp(kv[1], "copy"))
                        stream_row_select(w->rows[i], JOB_COPY, NULL);
                    else if (!strcmp(kv[1], "drop"))
                        stream_row_select(w->rows[i], JOB_DROP, NULL);
                    else
                        stream_row_select(w->rows[i], JOB_TRANSCODE, kv[1]);
                }
            }
            g_strfreev(kv);
        }
        g_strfreev(items);
        g_unsetenv("FFCONV_TEST_STREAMS");
        g_timeout_add(600, test_step, w);
        return G_SOURCE_REMOVE;
    }
    if (output) {
        gtk_editable_set_text(GTK_EDITABLE(w->output_entry), output);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(w->overwrite_check), TRUE);
        g_unsetenv("FFCONV_TEST_OUTPUT");
        g_timeout_add(600, test_step, w);
        return G_SOURCE_REMOVE;
    }
    if (shot)
        ui_save_snapshot(GTK_WIDGET(w->win), shot);
    if (ui_test_env("FFCONV_TEST_CONVERT") && gtk_widget_get_sensitive(w->convert_btn))
        start_conversion(w);
    else
        g_timeout_add(100, test_step_done, w);
    return G_SOURCE_REMOVE;
}

static void probe_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    MediaInfo *mi = NULL;
    char err[512];

    if (mi_probe(data, &mi, err, sizeof(err)) < 0)
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", err);
    else
        g_task_return_pointer(task, mi, NULL);
}

static void on_probed(GObject *src, GAsyncResult *res, gpointer data)
{
    ConvWindow *w = data;
    GError *error = NULL;
    MediaInfo *mi = g_task_propagate_pointer(G_TASK(res), &error);
    const char *path = g_task_get_task_data(G_TASK(res));
    char *out;

    w->loading = 0;
    if (!mi) {
        gtk_label_set_text(GTK_LABEL(w->summary_label), "");
        if (!w->mi)
            gtk_label_set_text(GTK_LABEL(w->input_label), "No file");
        alert(w, "This file cannot be opened", error ? error->message : path);
        g_clear_error(&error);
        update_convert_button(w);
        return;
    }

    mi_free(&w->mi);
    w->mi  = mi;
    w->mux = NULL;
    gtk_label_set_text(GTK_LABEL(w->input_label), mi->path);
    gtk_widget_set_tooltip_text(w->input_label, mi->path);
    show_summary(w);
    fill_containers(w);

    w->updating = 1;
    out = w->mux ? job_default_output(mi->path, muxer_ext(w->mux) ? w->mux->extensions : w->mux->name) : NULL;
    gtk_editable_set_text(GTK_EDITABLE(w->output_entry), out ? out : "");
    av_free(out);
    w->updating = 0;

    gtk_widget_set_sensitive(w->output_grid, TRUE);
    rebuild_rows(w);
    schedule_validate(w);

    if (ui_test_env("FFCONV_TEST_INPUT"))
        g_timeout_add(800, test_step, w);
}

void conv_window_open(ConvWindow *w, GFile *file)
{
    char *path = g_file_get_path(file);
    GTask *task;

    if (!path)
        path = g_file_get_uri(file);   /* FFmpeg reads http://, etc. too */
    if (w->converting) {
        g_free(path);
        return;
    }
    w->loading = 1;
    update_convert_button(w);
    gtk_label_set_text(GTK_LABEL(w->input_label), path);
    gtk_label_set_text(GTK_LABEL(w->summary_label), "Reading the file…");

    task = g_task_new(NULL, NULL, on_probed, w);
    g_task_set_task_data(task, path, g_free);
    g_task_run_in_thread(task, probe_thread);
    g_object_unref(task);
}

/* ------------------------------------------------------------------------- */
/* actions                                                                   */

static void on_open_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);

    if (file) {
        conv_window_open(data, file);
        g_object_unref(file);
    }
}

static void on_open_clicked(GtkButton *b, gpointer data)
{
    ConvWindow *w = data;
    GtkFileDialog *d = gtk_file_dialog_new();

    gtk_file_dialog_set_title(d, "Open a media file");
    gtk_file_dialog_open(d, w->win, NULL, on_open_done, w);
    g_object_unref(d);
}

static void on_save_done(GObject *src, GAsyncResult *res, gpointer data)
{
    ConvWindow *w = data;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);

    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            gtk_editable_set_text(GTK_EDITABLE(w->output_entry), path);
            /* the save dialog already asked about replacing an existing file */
            gtk_check_button_set_active(GTK_CHECK_BUTTON(w->overwrite_check), TRUE);
        }
        g_free(path);
        g_object_unref(file);
    }
}

static void on_browse_clicked(GtkButton *b, gpointer data)
{
    ConvWindow *w = data;
    GtkFileDialog *d = gtk_file_dialog_new();
    const char *cur = gtk_editable_get_text(GTK_EDITABLE(w->output_entry));

    gtk_file_dialog_set_title(d, "Save the converted file as");
    if (*cur) {
        char *dir = g_path_get_dirname(cur), *name = g_path_get_basename(cur);
        GFile *folder = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(d, folder);
        gtk_file_dialog_set_initial_name(d, name);
        g_object_unref(folder);
        g_free(dir);
        g_free(name);
    }
    gtk_file_dialog_save(d, w->win, NULL, on_save_done, w);
    g_object_unref(d);
}

static void on_output_changed(GtkEditable *e, gpointer data)
{
    ConvWindow *w = data;

    if (!w->updating)
        schedule_validate(w);
}

static void on_overwrite_toggled(GtkCheckButton *c, gpointer data)
{
    schedule_validate(data);
}

static void on_progress_finished(int ret, void *user)
{
    ConvWindow *w = user;

    w->converting   = 0;
    w->progress_win = NULL;
    schedule_validate(w);   /* e.g. the output now exists */
    if (ui_test_env("FFCONV_TEST_CONVERT"))
        g_timeout_add(100, test_step_done, w);
}

static void start_conversion(ConvWindow *w)
{
    ConvJob *job;

    if (w->converting || w->nb_errors || !(job = build_job(w)))
        return;
    w->converting = 1;
    update_convert_button(w);
    w->progress_win = progress_start(w->win, job, w->caps, on_progress_finished, w);
}

static void on_convert_clicked(GtkButton *b, gpointer data)
{
    start_conversion(data);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x, double y, gpointer data)
{
    ConvWindow *w = data;
    GFile *file = NULL;

    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
        if (files)
            file = files->data;
        g_slist_free(files);
    } else if (G_VALUE_HOLDS(value, G_TYPE_FILE)) {
        file = g_value_get_object(value);
    }
    if (!file)
        return FALSE;
    conv_window_open(w, file);
    return TRUE;
}

static gboolean on_close_request(GtkWindow *win, gpointer data)
{
    ConvWindow *w = data;

    if (w->converting) {            /* cancel first, from the progress window */
        if (w->progress_win)
            gtk_window_present(w->progress_win);
        return TRUE;
    }
    return FALSE;
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    ConvWindow *w = data;

    if (w->validate_id)
        g_source_remove(w->validate_id);
    for (int i = 0; i < w->nb_rows; i++)
        stream_row_free(w->rows[i]);
    g_free(w->rows);
    g_free(w->containers);
    mi_free(&w->mi);
    g_free(w);
}

/* ------------------------------------------------------------------------- */

ConvWindow *conv_window_new(GtkApplication *app, const Caps *caps)
{
    ConvWindow *w = g_new0(ConvWindow, 1);
    GtkWidget *header, *open_btn, *box, *frame, *placeholder, *grid, *browse, *bottom, *label;
    GtkDropTarget *drop;
    GType types[] = { GDK_TYPE_FILE_LIST, G_TYPE_FILE };

    w->app  = app;
    w->caps = caps;
    w->win  = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(w->win, "ffConv");
    gtk_window_set_default_size(w->win, 900, 640);

    header = gtk_header_bar_new();
    open_btn = gtk_button_new_with_mnemonic("_Open…");
    gtk_widget_set_tooltip_text(open_btn, "Open a media file (or drop one on the window)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open_btn);
    gtk_window_set_titlebar(w->win, header);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_window_set_child(w->win, box);

    /* input */
    gtk_box_append(GTK_BOX(box), heading("Input"));
    w->input_label = gtk_label_new("No file");
    gtk_label_set_xalign(GTK_LABEL(w->input_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(w->input_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(box), w->input_label);
    w->summary_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(w->summary_label), 0);
    gtk_widget_add_css_class(w->summary_label, "dim-label");
    gtk_box_append(GTK_BOX(box), w->summary_label);

    /* streams */
    label = heading("Streams");
    gtk_widget_set_margin_top(label, 10);
    gtk_box_append(GTK_BOX(box), label);
    frame = gtk_frame_new(NULL);
    w->stream_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(w->stream_list), GTK_SELECTION_NONE);
    placeholder = gtk_label_new("Open a media file, or drop one here.");
    gtk_widget_add_css_class(placeholder, "dim-label");
    gtk_widget_set_margin_top(placeholder, 36);
    gtk_widget_set_margin_bottom(placeholder, 36);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(w->stream_list), placeholder);
    {
        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), w->stream_list);
        gtk_frame_set_child(GTK_FRAME(frame), scroll);
    }
    gtk_box_append(GTK_BOX(box), frame);

    /* output */
    label = heading("Output");
    gtk_widget_set_margin_top(label, 10);
    gtk_box_append(GTK_BOX(box), label);
    grid = w->output_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);

    label = gtk_label_new("Container");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
    w->container_dd = gtk_drop_down_new(NULL,
        gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string"));
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(w->container_dd), TRUE);
    gtk_widget_set_hexpand(w->container_dd, TRUE);
    gtk_widget_set_tooltip_text(w->container_dd, "Containers that can hold this file's streams, "
                                "most common first (type to search)");
    gtk_grid_attach(GTK_GRID(grid), w->container_dd, 1, 0, 2, 1);

    label = gtk_label_new("File");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
    w->output_entry = gtk_entry_new();
    gtk_widget_set_hexpand(w->output_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->output_entry, 1, 1, 1, 1);
    browse = gtk_button_new_with_mnemonic("_Browse…");
    gtk_grid_attach(GTK_GRID(grid), browse, 2, 1, 1, 1);

    w->overwrite_check = gtk_check_button_new_with_mnemonic("Over_write the file if it exists");
    gtk_grid_attach(GTK_GRID(grid), w->overwrite_check, 1, 2, 2, 1);
    gtk_widget_set_sensitive(grid, FALSE);
    gtk_box_append(GTK_BOX(box), grid);

    /* validation + convert */
    w->issues_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(w->issues_box, 12);
    gtk_widget_set_vexpand(w->issues_box, TRUE);
    gtk_widget_set_valign(w->issues_box, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), w->issues_box);

    bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(bottom, GTK_ALIGN_END);
    w->convert_btn = gtk_button_new_with_mnemonic("_Convert");
    gtk_widget_add_css_class(w->convert_btn, "suggested-action");
    gtk_widget_set_size_request(w->convert_btn, 120, -1);
    gtk_widget_set_sensitive(w->convert_btn, FALSE);
    gtk_box_append(GTK_BOX(bottom), w->convert_btn);
    gtk_box_append(GTK_BOX(box), bottom);

    /* signals */
    g_signal_connect(open_btn, "clicked", G_CALLBACK(on_open_clicked), w);
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_clicked), w);
    g_signal_connect(w->container_dd, "notify::selected", G_CALLBACK(on_container_changed), w);
    g_signal_connect(w->output_entry, "changed", G_CALLBACK(on_output_changed), w);
    g_signal_connect(w->overwrite_check, "toggled", G_CALLBACK(on_overwrite_toggled), w);
    g_signal_connect(w->convert_btn, "clicked", G_CALLBACK(on_convert_clicked), w);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close_request), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
    gtk_drop_target_set_gtypes(drop, types, G_N_ELEMENTS(types));
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), GTK_EVENT_CONTROLLER(drop));

    return w;
}

GtkWindow *conv_window_get(ConvWindow *w)
{
    return w->win;
}

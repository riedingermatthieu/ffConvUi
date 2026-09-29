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

#include "choice_item.h"
#include "cmdline.h"
#include "job.h"
#include "option_editor.h"
#include "probe.h"
#include "preview_window.h"
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
    GtkWidget       *stream_scroll;
    GtkWidget       *output_grid;
    GtkWidget       *container_dd;
    GtkWidget       *mux_options_btn;
    GHashTable      *mux_options;      /* muxer key -> OptBox (only what the user set) */
    const CapsMuxer **containers;      /* dropdown position -> muxer */
    int              nb_containers;
    GListStore      *container_store;  /* ConvChoiceItem per container, greyed when incompatible */
    GtkWidget       *output_entry, *overwrite_check;
    GtkWidget       *issues_box;
    GtkWidget       *cmd_box, *cmd_view, *cmd_shell_dd, *cmd_copy_btn;
    guint            cmd_copied_id;
    GtkWidget       *convert_btn, *preview_btn;
    GtkWindow       *progress_win;
    GArray          *listeners;        /* ListenerEntry */
    guint            next_listener_id;
    GtkWindow       *preview_win;

    StreamRow      **rows;
    int              nb_rows;
    guint            validate_id;
    int              nb_errors;
    int              converting;
    int              loading;
    int              updating;         /* programmatic widget changes: no callbacks */
};

typedef struct ListenerEntry {
    ConvWindowListener l;
    void              *user;
    guint              id;
} ListenerEntry;

/* Call event `ev` of every listener; ARGS may use `le` (the entry). */
#define EMIT(w, ev, ARGS)                                                         \
    do {                                                                          \
        for (guint i_ = 0; i_ < (w)->listeners->len; i_++) {                      \
            ListenerEntry *le = &g_array_index((w)->listeners, ListenerEntry, i_); \
            if (le->l.ev)                                                         \
                le->l.ev ARGS;                                                    \
        }                                                                         \
    } while (0)

static void schedule_validate(ConvWindow *w);
static void update_container_states(ConvWindow *w);

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

/* ------------------------------------------------------------------------- */
/* muxer options, kept per container                                         */

typedef struct OptBox {
    AVDictionary *dict;
} OptBox;

static void opt_box_free(gpointer data)
{
    OptBox *b = data;

    av_dict_free(&b->dict);
    g_free(b);
}

static OptBox *mux_box(ConvWindow *w, gboolean create)
{
    OptBox *b;

    if (!w->mux)
        return NULL;
    if (!(b = g_hash_table_lookup(w->mux_options, w->mux->key)) && create) {
        b = g_new0(OptBox, 1);
        g_hash_table_insert(w->mux_options, g_strdup(w->mux->key), b);
    }
    return b;
}

static const AVDictionary *current_mux_options(ConvWindow *w)
{
    OptBox *b = mux_box(w, FALSE);
    return b ? b->dict : NULL;
}

static void update_mux_options_label(ConvWindow *w)
{
    int n = av_dict_count(current_mux_options(w));
    char label[32];

    if (n)
        g_snprintf(label, sizeof(label), "Options (%d)", n);
    else
        g_strlcpy(label, "Options", sizeof(label));
    gtk_button_set_label(GTK_BUTTON(w->mux_options_btn), label);
}

static void on_mux_options_edited(void *user)
{
    ConvWindow *w = user;

    update_mux_options_label(w);
    schedule_validate(w);
}

static GtkWindow *edit_mux_options(ConvWindow *w, OptionEditor **editor)
{
    OptBox *b = mux_box(w, TRUE);
    OptionTarget t = { 0 };

    if (!b)
        return NULL;
    t.muxer     = w->mux->fmt;
    t.muxer_key = w->mux->key;
    return option_dialog_show(w->win, t, &b->dict, on_mux_options_edited, w, editor);
}

static void on_mux_options_clicked(GtkButton *btn, gpointer data)
{
    edit_mux_options(data, NULL);
}

/* ------------------------------------------------------------------------- */

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
    if (current_mux_options(w) && av_dict_copy(&job->muxer_options, current_mux_options(w), 0) < 0)
        goto fail;

    for (int i = 0; i < w->nb_rows; i++) {
        JobAction a = stream_row_action(w->rows[i]);
        JobStream *js;

        if (a == JOB_DROP)
            continue;   /* unlisted streams are dropped */
        if (!(js = job_add_stream(job, stream_row_input_index(w->rows[i]), a)))
            goto fail;
        /* Convert with no encoder chosen: "" lets validation say so */
        if (a == JOB_TRANSCODE && (!(js->encoder = av_strdup(stream_row_encoder(w->rows[i]) ? stream_row_encoder(w->rows[i]) : "")) ||
                                   av_dict_copy(&js->encoder_options, stream_row_options(w->rows[i]), 0) < 0))
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
    /* previewing works on one stream: other streams' problems do not matter */
    gtk_widget_set_sensitive(w->preview_btn, w->mi && !w->loading);
}

/* ------------------------------------------------------------------------- */
/* equivalent ffmpeg command                                                 */

static CmdShell selected_shell(ConvWindow *w)
{
    static const CmdShell shells[] = { CMD_SHELL_POWERSHELL, CMD_SHELL_CMD, CMD_SHELL_POSIX };
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(w->cmd_shell_dd));
    return i < G_N_ELEMENTS(shells) ? shells[i] : cmd_default_shell();
}

static void show_command(ConvWindow *w, const ConvJob *job)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w->cmd_view));
    AVBPrint bp;

    gtk_widget_set_visible(w->cmd_box, job != NULL);
    if (!job)
        return;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    job_to_ffmpeg_command(job, w->mi, selected_shell(w), &bp);
    gtk_text_buffer_set_text(buf, bp.str, -1);
    av_bprint_finalize(&bp, NULL);
}

static void on_shell_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
    ConvWindow *w = data;
    ConvJob *job = build_job(w);

    show_command(w, job);
    job_free(&job);
}

static gboolean reset_copy_label(gpointer data)
{
    ConvWindow *w = data;

    gtk_button_set_label(GTK_BUTTON(w->cmd_copy_btn), "Copy");
    w->cmd_copied_id = 0;
    return G_SOURCE_REMOVE;
}

static void on_copy_command(GtkButton *b, gpointer data)
{
    ConvWindow *w = data;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w->cmd_view));
    GtkTextIter start, end;
    char *text;

    gtk_text_buffer_get_bounds(buf, &start, &end);
    text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(b)), text);
    g_free(text);

    gtk_button_set_label(b, "Copied");
    if (w->cmd_copied_id)
        g_source_remove(w->cmd_copied_id);
    w->cmd_copied_id = g_timeout_add(1500, reset_copy_label, w);
}

/* ------------------------------------------------------------------------- */

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
        show_command(w, NULL);
        update_convert_button(w);
        return G_SOURCE_REMOVE;
    }
    job = build_job(w);
    show_command(w, job);   /* even when the job has errors: it shows what was chosen */
    if (!job || validate_job(job, w->caps, w->mi, 0, &rep) < 0) {
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
    EMIT(w, job_changed, (w, le->user));
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
    update_container_states(user);   /* a stream's choice changes what each container can take */
    schedule_validate(user);
}

/* ------------------------------------------------------------------------- */
/* stream rows                                                               */

static void rebuild_rows(ConvWindow *w)
{
    int nb = w->mi ? w->mi->nb_streams : 0;
    int *prev_action = g_new0(int, nb ? nb : 1);
    char **prev_encoder = g_new0(char *, nb ? nb : 1);
    GHashTable **prev_options = g_new0(GHashTable *, nb ? nb : 1);

    /* keep the user's choices where the new container allows them */
    for (int i = 0; i < nb; i++)
        prev_action[i] = -1;
    for (int i = 0; i < w->nb_rows; i++) {
        int idx = stream_row_input_index(w->rows[i]);
        if (idx < nb) {
            prev_action[idx]  = stream_row_action(w->rows[i]);
            prev_encoder[idx] = g_strdup(stream_row_encoder(w->rows[i]));
            prev_options[idx] = stream_row_take_options(w->rows[i]);
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
            StreamRow *row = stream_row_new(w->caps, w->mux, &w->mi->streams[i], prev_options[i],
                                            prev_action[i], prev_encoder[i], on_row_changed, w);
            GtkWidget *lbrow;

            prev_options[i] = NULL;   /* taken over by the new row */
            w->rows[w->nb_rows++] = row;
            gtk_list_box_append(GTK_LIST_BOX(w->stream_list), stream_row_widget(row));
            lbrow = gtk_widget_get_parent(stream_row_widget(row));
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(lbrow), FALSE);
        }
    }
    for (int i = 0; i < nb; i++) {
        g_free(prev_encoder[i]);
        if (prev_options[i])
            g_hash_table_unref(prev_options[i]);
    }
    g_free(prev_options);
    g_free(prev_encoder);
    g_free(prev_action);
}

/* Keep up to four rows visible: the window's other sections must not squeeze
 * the stream list (it scrolls beyond four). */
static void fit_stream_list(ConvWindow *w)
{
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(w->stream_scroll),
                                               MIN(MAX(w->nb_rows, 1), 4) * 50);
}

/* A new file: nothing from the previous file's streams carries over. */
static void clear_rows(ConvWindow *w)
{
    gtk_list_box_remove_all(GTK_LIST_BOX(w->stream_list));
    for (int i = 0; i < w->nb_rows; i++)
        stream_row_free(w->rows[i]);
    g_free(w->rows);
    w->rows    = NULL;
    w->nb_rows = 0;
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

/* Grey out the containers that cannot store the streams as currently chosen.
 * They stay selectable: choosing one keeps every stream's choice as it is,
 * and validation explains what does not fit. */
static void update_container_states(ConvWindow *w)
{
    for (int i = 0; i < w->nb_containers; i++) {
        ConvChoiceItem *it = g_list_model_get_item(G_LIST_MODEL(w->container_store), i);
        char why[256] = "", first[256] = "";
        int bad = 0;

        for (int r = 0; r < w->nb_rows; r++)
            if (!stream_row_fits(w->rows[r], w->containers[i], why, sizeof(why)) && !bad++)
                g_strlcpy(first, why, sizeof(first));
        if (bad > 1)
            g_snprintf(why, sizeof(why), "%s: %s, and %d more", w->containers[i]->key, first, bad - 1);
        else if (bad)
            g_snprintf(why, sizeof(why), "%s: %s", w->containers[i]->key, first);
        if (bad)   /* "mp4: Cannot store..." */
            why[strlen(w->containers[i]->key) + 2] = g_ascii_toupper(why[strlen(w->containers[i]->key) + 2]);
        conv_choice_item_set_state(it, !bad, bad ? why : NULL);
        g_object_unref(it);
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
    rebuild_rows(w);          /* same choices, re-checked against the new container */
    update_container_states(w);
    fit_stream_list(w);
    update_mux_options_label(w);
    schedule_validate(w);
}

/* Every container: first those that can keep at least one of the file's
 * streams (common first, most streams kept), then the others. The one
 * matching the input's extension (or Matroska) is selected. */
static void fill_containers(ConvWindow *w)
{
    GListStore *store = g_list_store_new(CONV_TYPE_CHOICE_ITEM);
    const AVOutputFormat *same = av_guess_format(NULL, w->mi->path, NULL);
    CapsMuxerFit *fits = NULL;
    int nb = 0, n = 0, sel = -1, mkv = -1;
    GtkWidget *dd;

    caps_muxers_for(w->caps, w->mi, &fits, &nb);
    g_free(w->containers);
    w->containers = g_new0(const CapsMuxer *, w->caps->nb_muxers ? w->caps->nb_muxers : 1);

    for (int i = 0; i < nb; i++) {
        const CapsMuxerFit *f = &fits[i];
        if (w->mux && f->mux == w->mux)
            sel = n;                          /* keep the current choice */
        if (sel < 0 && same && f->mux->fmt == same && f->nb_kept == f->nb_considered)
            sel = n;
        if (!strcmp(f->mux->key, "matroska"))
            mkv = n;
        w->containers[n++] = f->mux;
    }
    for (int i = 0; i < w->caps->nb_muxers; i++) {
        const CapsMuxer *m = &w->caps->muxers[i];
        int listed = 0;
        for (int j = 0; j < nb && !listed; j++)
            listed = fits[j].mux == m;
        if (!listed)
            w->containers[n++] = m;
    }
    w->nb_containers = n;
    if (sel < 0)
        sel = mkv >= 0 ? mkv : 0;

    for (int i = 0; i < n; i++) {
        char label[256];
        ConvChoiceItem *it;
        g_snprintf(label, sizeof(label), "%s — %s", w->containers[i]->key, w->containers[i]->long_name);
        it = conv_choice_item_new(label, w->containers[i]);
        g_list_store_append(store, it);
        g_object_unref(it);
    }

    /* a new dropdown over the new store keeps the factory set up by
     * conv_choice_dropdown_new; swap it in place */
    w->updating = 1;
    dd = conv_choice_dropdown_new(store, TRUE);
    gtk_widget_set_hexpand(dd, TRUE);
    gtk_widget_set_tooltip_text(dd, gtk_widget_get_tooltip_text(w->container_dd));
    gtk_grid_remove(GTK_GRID(w->output_grid), w->container_dd);
    gtk_grid_attach(GTK_GRID(w->output_grid), dd, 1, 0, 1, 1);
    w->container_dd = dd;
    if (n)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), sel);
    g_signal_connect(dd, "notify::selected", G_CALLBACK(on_container_changed), w);
    w->updating = 0;

    if (w->container_store)
        g_object_unref(w->container_store);
    w->container_store = store;
    w->mux = n ? w->containers[sel] : NULL;
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

    clear_rows(w);          /* before freeing the MediaInfo the rows point into */
    mi_free(&w->mi);
    w->mi  = mi;
    w->mux = NULL;
    gtk_label_set_text(GTK_LABEL(w->input_label), mi->path);
    gtk_widget_set_tooltip_text(w->input_label, mi->path);
    show_summary(w);
    fill_containers(w);
    update_mux_options_label(w);

    w->updating = 1;
    out = w->mux ? job_default_output(mi->path, muxer_ext(w->mux) ? w->mux->extensions : w->mux->name) : NULL;
    gtk_editable_set_text(GTK_EDITABLE(w->output_entry), out ? out : "");
    av_free(out);
    w->updating = 0;

    gtk_widget_set_sensitive(w->output_grid, TRUE);
    rebuild_rows(w);
    update_container_states(w);
    fit_stream_list(w);
    schedule_validate(w);

    EMIT(w, file_loaded, (w, le->user));
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
    EMIT(w, conversion_finished, (w, ret, le->user));
}

static void start_conversion(ConvWindow *w)
{
    ConvJob *job;

    if (w->converting || w->nb_errors || !(job = build_job(w)))
        return;
    w->converting = 1;
    update_convert_button(w);
    w->progress_win = progress_start(w->win, job, w->caps, on_progress_finished, w);
    /* job: owned by the progress window now, valid until it finishes */
    EMIT(w, conversion_started, (w, job, w->progress_win, le->user));
}

static void on_convert_clicked(GtkButton *b, gpointer data)
{
    start_conversion(data);
}

static void on_preview_destroyed(GtkWidget *win, gpointer data)
{
    ((ConvWindow *)data)->preview_win = NULL;
}

GtkWindow *conv_window_open_preview(ConvWindow *w)
{
    if (!w->mi)
        return NULL;
    if (!w->preview_win) {
        w->preview_win = preview_window_new(w);
        g_signal_connect(w->preview_win, "destroy", G_CALLBACK(on_preview_destroyed), w);
    }
    gtk_window_present(w->preview_win);
    return w->preview_win;
}

static void on_preview_clicked(GtkButton *b, gpointer data)
{
    conv_window_open_preview(data);
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

    if (w->preview_win)   /* it listens to this window: close it first */
        gtk_window_destroy(w->preview_win);
    if (w->cmd_copied_id)
        g_source_remove(w->cmd_copied_id);
    if (w->validate_id)
        g_source_remove(w->validate_id);
    for (int i = 0; i < w->nb_rows; i++)
        stream_row_free(w->rows[i]);
    g_free(w->rows);
    g_free(w->containers);
    if (w->container_store)
        g_object_unref(w->container_store);
    g_hash_table_unref(w->mux_options);
    g_array_free(w->listeners, TRUE);
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
    w->mux_options = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, opt_box_free);
    w->listeners   = g_array_new(FALSE, FALSE, sizeof(ListenerEntry));
    w->win  = GTK_WINDOW(gtk_application_window_new(app));
    g_object_set_data(G_OBJECT(w->win), "conv-window", w);
    gtk_window_set_title(w->win, "ffConv");
    gtk_window_set_default_size(w->win, 900, 720);

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
        w->stream_scroll = scroll;
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
    gtk_grid_attach(GTK_GRID(grid), w->container_dd, 1, 0, 1, 1);
    w->mux_options_btn = gtk_button_new_with_label("Options");
    gtk_widget_set_tooltip_text(w->mux_options_btn, "Container (muxer) options");
    gtk_grid_attach(GTK_GRID(grid), w->mux_options_btn, 2, 0, 1, 1);

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

    /* equivalent ffmpeg command */
    {
        GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8), *title, *frame2, *scroll;
        GtkStringList *shells = gtk_string_list_new((const char *[]){ "PowerShell", "cmd", "Bash", NULL });

        w->cmd_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_top(w->cmd_box, 12);

        title = heading("Equivalent ffmpeg command");
        gtk_widget_set_hexpand(title, TRUE);
        gtk_widget_set_tooltip_text(title, "The ffmpeg command-line tool runs the same conversion with "
                                    "this command. It updates with every change.");
        gtk_box_append(GTK_BOX(head), title);
        w->cmd_shell_dd = gtk_drop_down_new(G_LIST_MODEL(shells), NULL);
        gtk_widget_set_tooltip_text(w->cmd_shell_dd, "Quote the arguments for this shell");
        gtk_drop_down_set_selected(GTK_DROP_DOWN(w->cmd_shell_dd),
                                   cmd_default_shell() == CMD_SHELL_POSIX ? 2 : 0);
        gtk_box_append(GTK_BOX(head), w->cmd_shell_dd);
        w->cmd_copy_btn = gtk_button_new_with_label("Copy");
        gtk_widget_set_tooltip_text(w->cmd_copy_btn, "Copy the command to the clipboard");
        gtk_box_append(GTK_BOX(head), w->cmd_copy_btn);
        gtk_box_append(GTK_BOX(w->cmd_box), head);

        w->cmd_view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(w->cmd_view), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(w->cmd_view), TRUE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(w->cmd_view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(w->cmd_view), 6);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(w->cmd_view), 6);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(w->cmd_view), 8);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(w->cmd_view), 8);
        scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 110);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), w->cmd_view);
        frame2 = gtk_frame_new(NULL);
        gtk_frame_set_child(GTK_FRAME(frame2), scroll);
        gtk_box_append(GTK_BOX(w->cmd_box), frame2);

        gtk_widget_set_visible(w->cmd_box, FALSE);   /* until a file is open */
        gtk_box_append(GTK_BOX(box), w->cmd_box);
        g_signal_connect(w->cmd_shell_dd, "notify::selected", G_CALLBACK(on_shell_changed), w);
        g_signal_connect(w->cmd_copy_btn, "clicked", G_CALLBACK(on_copy_command), w);
    }

    /* validation + convert */
    w->issues_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(w->issues_box, 12);
    gtk_widget_set_vexpand(w->issues_box, TRUE);
    gtk_widget_set_valign(w->issues_box, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), w->issues_box);

    bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(bottom, GTK_ALIGN_END);
    w->preview_btn = gtk_button_new_with_mnemonic("_Preview…");
    gtk_widget_set_tooltip_text(w->preview_btn, "See one frame before and after conversion");
    gtk_widget_set_sensitive(w->preview_btn, FALSE);
    gtk_box_append(GTK_BOX(bottom), w->preview_btn);
    g_signal_connect(w->preview_btn, "clicked", G_CALLBACK(on_preview_clicked), w);
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
    g_signal_connect(w->mux_options_btn, "clicked", G_CALLBACK(on_mux_options_clicked), w);
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

ConvWindow *conv_window_from_window(GtkWindow *win)
{
    return win ? g_object_get_data(G_OBJECT(win), "conv-window") : NULL;
}

guint conv_window_add_listener(ConvWindow *w, const ConvWindowListener *l, void *user)
{
    ListenerEntry le = { *l, user, ++w->next_listener_id };

    g_array_append_val(w->listeners, le);
    return le.id;
}

void conv_window_remove_listener(ConvWindow *w, guint id)
{
    for (guint i = 0; i < w->listeners->len; i++)
        if (g_array_index(w->listeners, ListenerEntry, i).id == id) {
            g_array_remove_index(w->listeners, i);
            return;
        }
}

ConvJob *conv_window_build_job(ConvWindow *w)
{
    return build_job(w);
}

const MediaInfo *conv_window_media(ConvWindow *w)
{
    return w->mi;
}

/* ------------------------------------------------------------------------- */
/* operations                                                                */

gboolean conv_window_select_container(ConvWindow *w, const char *key)
{
    for (int i = 0; i < w->nb_containers; i++) {
        if (!strcmp(w->containers[i]->key, key)) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(w->container_dd), i);   /* -> on_container_changed */
            return TRUE;
        }
    }
    return FALSE;
}

const char *conv_window_container_key(ConvWindow *w)
{
    return w->mux ? w->mux->key : NULL;
}

GtkWidget *conv_window_container_dropdown(ConvWindow *w)
{
    return w->container_dd;
}

int conv_window_nb_streams(ConvWindow *w)
{
    return w->nb_rows;
}

StreamRow *conv_window_stream_at(ConvWindow *w, int position)
{
    return position >= 0 && position < w->nb_rows ? w->rows[position] : NULL;
}

StreamRow *conv_window_stream(ConvWindow *w, int input_index)
{
    for (int i = 0; i < w->nb_rows; i++)
        if (stream_row_input_index(w->rows[i]) == input_index)
            return w->rows[i];
    return NULL;
}

void conv_window_set_output(ConvWindow *w, const char *path, gboolean overwrite)
{
    gtk_editable_set_text(GTK_EDITABLE(w->output_entry), path);          /* -> on_output_changed */
    gtk_check_button_set_active(GTK_CHECK_BUTTON(w->overwrite_check), overwrite);
}

GtkWindow *conv_window_edit_muxer_options(ConvWindow *w, OptionEditor **editor)
{
    return edit_mux_options(w, editor);
}

gboolean conv_window_can_convert(ConvWindow *w)
{
    return gtk_widget_get_sensitive(w->convert_btn);
}

void conv_window_convert(ConvWindow *w)
{
    if (conv_window_can_convert(w))
        start_conversion(w);
}

char *conv_window_command(ConvWindow *w)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w->cmd_view));
    GtkTextIter a, b;

    gtk_text_buffer_get_bounds(buf, &a, &b);
    return gtk_text_buffer_get_text(buf, &a, &b, FALSE);
}

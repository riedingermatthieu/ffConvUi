/*
 * progress.c - conversion window and worker thread.
 *
 * The worker validates the job (dry run included) and runs it. It never
 * touches widgets: progress and phase changes are posted to the main thread
 * with g_idle_add(). The Run state is reference-counted because idle
 * callbacks may still be pending when the task completes.
 */
#include "progress.h"

#include <stdatomic.h>
#include <stdlib.h>

#include <glib/gstdio.h>
#include <libavutil/error.h>

#include "engine.h"
#include "ui_util.h"
#include "uilog.h"
#include "validate.h"

typedef struct Run {
    ConvJob          *job;
    const Caps       *caps;
    atomic_int        cancel;

    /* results, written by the worker, read after completion */
    ValReport         report;
    int               validation_failed;
    int               ret;
    EngineStats       stats;
    char              err[1024];
    double            elapsed;

    /* main thread only */
    GtkWindow        *win;
    GtkWidget        *status, *bar, *details, *issues, *log_view;
    GtkWidget        *cancel_btn, *close_btn, *folder_btn;
    int               running;
    int               progress_shot_taken;
    ProgressFinished  finished;
    void             *user;
} Run;

enum { MSG_CONVERTING, MSG_PROGRESS };

static void on_cancel(GtkButton *b, gpointer data);

typedef struct Msg {
    Run           *run;
    int            kind;
    EngineProgress p;
} Msg;

static void run_clear(gpointer data)
{
    Run *r = data;

    validate_report_free(&r->report);
    job_free(&r->job);
}

static void run_release(Run *r)
{
    g_rc_box_release_full(r, run_clear);
}

/* ------------------------------------------------------------------------- */
/* main thread                                                               */

static void set_status(Run *r, const char *text, const char *css_class)
{
    gtk_label_set_text(GTK_LABEL(r->status), text);
    gtk_widget_remove_css_class(r->status, "error");
    gtk_widget_remove_css_class(r->status, "success");
    gtk_widget_remove_css_class(r->status, "warning");
    if (css_class)
        gtk_widget_add_css_class(r->status, css_class);
}

static gboolean progress_shot(gpointer data)
{
    Run *r = data;
    const char *path = ui_test_env("FFCONV_TEST_PROGRESS_SHOT");

    if (r->win && path)
        ui_save_snapshot(GTK_WIDGET(r->win), path);
    run_release(r);
    return G_SOURCE_REMOVE;
}

static void show_progress(Run *r, const EngineProgress *p)
{
    char t[32], d[32], size[32], eta[32], text[256];

    if (p->percent >= 0) {
        char pct[16];
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(r->bar), p->percent / 100.0);
        g_snprintf(pct, sizeof(pct), "%.0f %%", p->percent);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(r->bar), pct);
    } else {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(r->bar));
    }

    ui_format_time(p->out_time, t, sizeof(t));
    ui_format_time(p->duration > 0 ? p->duration : -1, d, sizeof(d));
    ui_format_size(p->out_bytes, size, sizeof(size));
    ui_format_time(p->duration > 0 && p->speed > 0.01 ? (p->duration - p->out_time) / p->speed : -1,
                   eta, sizeof(eta));
    if (p->frames)
        g_snprintf(text, sizeof(text), "%s / %s  ·  %.0f fps  ·  %.2f× realtime  ·  %s  ·  %s left",
                   t, d, p->fps, p->speed, size, eta);
    else
        g_snprintf(text, sizeof(text), "%s / %s  ·  %.2f× realtime  ·  %s  ·  %s left",
                   t, d, p->speed, size, eta);
    gtk_label_set_text(GTK_LABEL(r->details), text);

    if (ui_test_env("FFCONV_TEST_CANCEL_AT") && p->percent >= atof(ui_test_env("FFCONV_TEST_CANCEL_AT")))
        on_cancel(NULL, r);
    if (!r->progress_shot_taken && p->percent >= 30 && ui_test_env("FFCONV_TEST_PROGRESS_SHOT")) {
        r->progress_shot_taken = 1;
        g_timeout_add(200, progress_shot, g_rc_box_acquire(r));
    }
}

static gboolean msg_idle(gpointer data)
{
    Msg *m = data;
    Run *r = m->run;

    if (r->win && r->running) {
        if (m->kind == MSG_CONVERTING) {
            set_status(r, "Converting…", NULL);
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(r->bar), 0);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(r->bar), "0 %");
        } else {
            show_progress(r, &m->p);
        }
    }
    run_release(r);
    g_free(m);
    return G_SOURCE_REMOVE;
}

static void post(Run *r, int kind, const EngineProgress *p)
{
    Msg *m = g_new0(Msg, 1);

    m->run  = g_rc_box_acquire(r);
    m->kind = kind;
    if (p)
        m->p = *p;
    g_idle_add(msg_idle, m);
}

static void on_folder_done(GObject *src, GAsyncResult *res, gpointer data)
{
    gtk_file_launcher_open_containing_folder_finish(GTK_FILE_LAUNCHER(src), res, NULL);
    g_object_unref(src);
}

static void on_show_folder(GtkButton *b, gpointer data)
{
    Run *r = data;
    GFile *file = g_file_new_for_path(r->job->output);
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);

    gtk_file_launcher_open_containing_folder(launcher, r->win, NULL, on_folder_done, NULL);
    g_object_unref(file);
}

static void on_cancel(GtkButton *b, gpointer data)
{
    Run *r = data;

    if (!r->running)
        return;
    atomic_store(&r->cancel, 1);
    set_status(r, "Cancelling…", "warning");
    gtk_widget_set_sensitive(r->cancel_btn, FALSE);
}

static void on_close(GtkButton *b, gpointer data)
{
    Run *r = data;
    gtk_window_close(r->win);
}

static gboolean on_close_request(GtkWindow *win, gpointer data)
{
    Run *r = data;

    if (r->running) {           /* closing while converting = cancel first */
        on_cancel(NULL, r);
        return TRUE;
    }
    return FALSE;
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    Run *r = data;

    uilog_set_buffer(NULL);
    r->win = NULL;
    run_release(r);
}

static void show_issues(Run *r)
{
    GString *s = g_string_new(NULL);

    for (int i = 0; i < r->report.nb_issues; i++) {
        const ValIssue *is = &r->report.issues[i];
        if (is->severity != VAL_ERROR)
            continue;
        if (is->stream >= 0)
            g_string_append_printf(s, "• Stream #%d: %s\n", r->job->streams[is->stream].input_index,
                                   is->message);
        else
            g_string_append_printf(s, "• %s\n", is->message);
    }
    gtk_label_set_text(GTK_LABEL(r->issues), s->str);
    gtk_widget_set_visible(r->issues, TRUE);
    g_string_free(s, TRUE);
}

static gboolean done_shot(gpointer data)
{
    Run *r = data;
    const char *path = ui_test_env("FFCONV_TEST_DONE_SHOT");

    if (r->win && path)
        ui_save_snapshot(GTK_WIDGET(r->win), path);
    if (r->finished)
        r->finished(r->ret, r->user);
    run_release(r);
    return G_SOURCE_REMOVE;
}

static void on_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Run *r = data;
    char text[1400], t[32];

    r->running = 0;
    if (r->win) {
        gtk_widget_set_visible(r->cancel_btn, FALSE);
        gtk_widget_set_visible(r->close_btn, TRUE);
        gtk_window_set_default_widget(r->win, r->close_btn);
        ui_format_time(r->elapsed, t, sizeof(t));

        if (r->validation_failed) {
            set_status(r, "The job cannot run", "error");
            gtk_label_set_text(GTK_LABEL(r->details), "Fix these problems and try again:");
            show_issues(r);
        } else if (r->ret == AVERROR_EXIT) {
            set_status(r, "Cancelled", "warning");
            gtk_label_set_text(GTK_LABEL(r->details), r->job->keep_partial
                               ? "The partial output was kept." : "The partial output was removed.");
        } else if (r->ret < 0) {
            set_status(r, "Conversion failed", "error");
            gtk_label_set_text(GTK_LABEL(r->details), r->err[0] ? r->err : av_err2str(r->ret));
        } else {
            GStatBuf st;
            char size[32] = "";
            if (g_stat(r->job->output, &st) == 0)
                ui_format_size(st.st_size, size, sizeof(size));
            set_status(r, "Done", "success");
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(r->bar), 1.0);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(r->bar), "100 %");
            g_snprintf(text, sizeof(text), "Converted in %s  ·  %s  ·  %s", t, size, r->job->output);
            gtk_label_set_text(GTK_LABEL(r->details), text);
            gtk_widget_set_visible(r->folder_btn, TRUE);
        }
        if (!r->validation_failed &&
            (r->stats.decode_errors || r->stats.dropped_frames || r->stats.graph_reinits)) {
            g_snprintf(text, sizeof(text),
                       "Decode errors: %" G_GINT64_FORMAT "  ·  dropped frames: %" G_GINT64_FORMAT
                       "  ·  filter rebuilds: %d", r->stats.decode_errors, r->stats.dropped_frames,
                       r->stats.graph_reinits);
            gtk_label_set_text(GTK_LABEL(r->issues), text);
            gtk_widget_set_visible(r->issues, TRUE);
        }
    }
    /* let the window redraw before a test snapshot / the owner's callback */
    g_timeout_add(300, done_shot, r);   /* takes over the task's reference */
}

/* ------------------------------------------------------------------------- */
/* worker thread                                                             */

static void on_engine_progress(const EngineProgress *p, void *opaque)
{
    if (!p->finished)
        post(opaque, MSG_PROGRESS, p);
}

static void worker(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    Run *r = data;
    gint64 t0 = g_get_monotonic_time();

    if (validate_job(r->job, r->caps, NULL, VALIDATE_DRY_RUN, &r->report) < 0) {
        r->ret = AVERROR(ENOMEM);
    } else if (r->report.nb_errors) {
        r->validation_failed = 1;
        r->ret = AVERROR(EINVAL);
    } else if (atomic_load(&r->cancel)) {
        r->ret = AVERROR_EXIT;
    } else {
        EngineCallbacks cb = { on_engine_progress, r };
        post(r, MSG_CONVERTING, NULL);
        r->ret = engine_run(r->job, &cb, &r->cancel, &r->stats, r->err, sizeof(r->err));
    }
    r->elapsed = (g_get_monotonic_time() - t0) / 1e6;
    g_task_return_boolean(task, TRUE);
}

/* ------------------------------------------------------------------------- */

static GtkWidget *label_new(const char *text, gboolean wrap)
{
    GtkWidget *l = gtk_label_new(text);

    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), wrap);
    gtk_label_set_selectable(GTK_LABEL(l), wrap);
    return l;
}

static void on_log_changed(GtkTextBuffer *buf, gpointer data)
{
    GtkTextView *view = data;
    GtkTextMark *mark = gtk_text_buffer_get_mark(buf, "end");

    if (mark)
        gtk_text_view_scroll_mark_onscreen(view, mark);
}

GtkWindow *progress_start(GtkWindow *parent, ConvJob *job, const Caps *caps,
                          ProgressFinished finished, void *user)
{
    Run *r = g_rc_box_new0(Run);
    GtkWidget *box, *title, *expander, *scroll, *buttons;
    GtkTextBuffer *buf;
    GtkTextIter end;
    GTask *task;
    char *name_in, *name_out, *markup;

    r->job      = job;
    r->caps     = caps;
    r->finished = finished;
    r->user     = user;
    r->running  = 1;

    r->win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(r->win, "Converting");
    gtk_window_set_transient_for(r->win, parent);
    gtk_window_set_modal(r->win, TRUE);
    gtk_window_set_default_size(r->win, 620, -1);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_window_set_child(r->win, box);

    name_in  = g_path_get_basename(job->input);
    name_out = g_path_get_basename(job->output);
    markup = g_markup_printf_escaped("<b>%s</b>  →  <b>%s</b>", name_in, name_out);
    title = label_new(NULL, FALSE);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_MIDDLE);
    g_free(markup);
    g_free(name_in);
    g_free(name_out);
    gtk_box_append(GTK_BOX(box), title);

    r->status = label_new("Checking the job…", FALSE);
    gtk_widget_add_css_class(r->status, "title-4");
    gtk_box_append(GTK_BOX(box), r->status);

    r->bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(r->bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(r->bar), "");
    gtk_box_append(GTK_BOX(box), r->bar);

    r->details = label_new("", TRUE);
    gtk_label_set_selectable(GTK_LABEL(r->details), FALSE);   /* no caret on it */
    gtk_widget_add_css_class(r->details, "dim-label");
    gtk_box_append(GTK_BOX(box), r->details);

    r->issues = label_new("", TRUE);
    gtk_widget_set_visible(r->issues, FALSE);
    gtk_box_append(GTK_BOX(box), r->issues);

    /* FFmpeg log */
    expander = gtk_expander_new("FFmpeg log");
    if (ui_test_env("FFCONV_TEST_EXPAND_LOG"))
        gtk_expander_set_expanded(GTK_EXPANDER(expander), TRUE);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 180);
    gtk_widget_set_vexpand(scroll, TRUE);
    r->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(r->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(r->log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(r->log_view), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), r->log_view);
    gtk_expander_set_child(GTK_EXPANDER(expander), scroll);
    gtk_box_append(GTK_BOX(box), expander);

    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(r->log_view));
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_create_mark(buf, "end", &end, FALSE);
    g_signal_connect(buf, "changed", G_CALLBACK(on_log_changed), r->log_view);
    uilog_set_buffer(buf);

    /* buttons */
    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    gtk_widget_set_margin_top(buttons, 6);
    r->folder_btn = gtk_button_new_with_mnemonic("Show in _folder");
    r->cancel_btn = gtk_button_new_with_mnemonic("_Cancel");
    r->close_btn  = gtk_button_new_with_mnemonic("C_lose");
    gtk_widget_add_css_class(r->close_btn, "suggested-action");
    gtk_widget_set_visible(r->folder_btn, FALSE);
    gtk_widget_set_visible(r->close_btn, FALSE);
    gtk_box_append(GTK_BOX(buttons), r->folder_btn);
    gtk_box_append(GTK_BOX(buttons), r->cancel_btn);
    gtk_box_append(GTK_BOX(buttons), r->close_btn);
    gtk_box_append(GTK_BOX(box), buttons);

    g_signal_connect(r->folder_btn, "clicked", G_CALLBACK(on_show_folder), r);
    g_signal_connect(r->cancel_btn, "clicked", G_CALLBACK(on_cancel), r);
    g_signal_connect(r->close_btn, "clicked", G_CALLBACK(on_close), r);
    g_signal_connect(r->win, "close-request", G_CALLBACK(on_close_request), r);
    g_signal_connect(r->win, "destroy", G_CALLBACK(on_destroy), g_rc_box_acquire(r));

    gtk_window_present(r->win);

    /* the task's reference is released by done_shot() */
    task = g_task_new(NULL, NULL, on_done, g_rc_box_acquire(r));
    g_task_set_task_data(task, r, NULL);
    g_task_run_in_thread(task, worker);
    g_object_unref(task);

    run_release(r);   /* ours: the window and the task hold theirs */
    return r->win;
}

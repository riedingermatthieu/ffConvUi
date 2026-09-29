/*
 * preview_window.c - one video frame before and after conversion.
 *
 * The worker renders with preview_render() on a snapshot of the job; the
 * window never waits for it. A new request while a render runs cancels it
 * and runs again with the latest choices; only the latest result is shown.
 */
#include "preview_window.h"

#include <stdatomic.h>
#include <string.h>

#include <libavutil/mem.h>

#include "choice_item.h"
#include "preview.h"
#include "stream_row.h"
#include "window.h"

#define RENDER_DELAY_MS 300

typedef struct Render Render;

typedef struct PW {
    ConvWindow      *owner;
    guint            owner_listener;
    GtkWindow       *win;
    GtkWidget       *stream_dd, *scale, *time_label;
    GtkWidget       *spinner, *status;
    GtkWidget       *pic_before, *pic_after, *cap_before, *cap_after;
    GtkWidget       *zoom_btn;
    GListStore      *streams;          /* ConvChoiceItem, data: GINT_TO_POINTER(input index + 1) */
    int              stream_input;     /* input index of the previewed stream, -1 = none */
    guint            delay_id;
    int              updating;
    /* rendering */
    Render          *running;          /* the render in progress, or NULL */
    int              pending;          /* render again when it is done */
    int              destroyed;        /* the window is gone: free when the render ends */
    PreviewWindowListener listener;
    void            *listener_user;
} PW;

struct Render {
    PW            *pw;
    ConvJob       *job;
    PreviewRequest req;
    char          *scratch;
    atomic_int     cancel;
    PreviewResult  res;
    int            ret;
    char           err[1024];
};

static void request_render(PW *pw);
static void pw_free(PW *pw);

/* "01:04.50": frame times need hundredths */
static void format_time(double t, char *buf, size_t size)
{
    int m = t > 0 ? (int)(t / 60) : 0;
    g_snprintf(buf, size, "%02d:%05.2f", m, t > 0 ? t - m * 60 : 0);
}

static PW *pw_of(GtkWindow *win)
{
    return win ? g_object_get_data(G_OBJECT(win), "conv-preview") : NULL;
}

/* ------------------------------------------------------------------------- */
/* results                                                                   */

static GdkTexture *texture_of(PreviewImage *img)
{
    GBytes *bytes;
    GdkTexture *t;

    if (!img->rgba)
        return NULL;
    /* the texture takes the pixels over */
    bytes = g_bytes_new_with_free_func(img->rgba, (gsize)img->stride * img->height, av_free, img->rgba);
    img->rgba = NULL;
    t = gdk_memory_texture_new(img->width, img->height, GDK_MEMORY_R8G8B8A8, bytes, img->stride);
    g_bytes_unref(bytes);
    return t;
}

static void set_status(PW *pw, gboolean busy, const char *text, const char *css)
{
    gtk_spinner_set_spinning(GTK_SPINNER(pw->spinner), busy);
    gtk_widget_set_visible(pw->spinner, busy);
    gtk_label_set_text(GTK_LABEL(pw->status), text);
    gtk_widget_remove_css_class(pw->status, "error");
    gtk_widget_remove_css_class(pw->status, "dim-label");
    if (css)
        gtk_widget_add_css_class(pw->status, css);
}

static void show_result(PW *pw, Render *r)
{
    char t[32], text[512];
    GdkTexture *tex;

    if (r->ret < 0) {
        set_status(pw, FALSE, r->err[0] ? r->err : "The preview failed", "error");
        if (pw->listener.rendered)
            pw->listener.rendered(pw->win, FALSE, pw->listener_user);
        return;
    }

    format_time(r->res.before.time, t, sizeof(t));
    g_snprintf(text, sizeof(text), "Original  ·  %d×%d %s  ·  %s", r->res.before.coded_width,
               r->res.before.coded_height, r->res.before.pix_fmt, t);
    gtk_label_set_text(GTK_LABEL(pw->cap_before), text);
    if (r->res.copied)
        g_snprintf(text, sizeof(text), "Result  ·  copied: identical to the original");
    else
        g_snprintf(text, sizeof(text), "Result  ·  %s  ·  %d×%d %s  ·  %.0f kb/s", r->res.encoder,
                   r->res.after.coded_width, r->res.after.coded_height, r->res.after.pix_fmt, r->res.kbps);
    gtk_label_set_text(GTK_LABEL(pw->cap_after), text);

    tex = texture_of(&r->res.before);
    gtk_picture_set_paintable(GTK_PICTURE(pw->pic_before), GDK_PAINTABLE(tex));
    g_clear_object(&tex);
    tex = texture_of(&r->res.after);
    gtk_picture_set_paintable(GTK_PICTURE(pw->pic_after), GDK_PAINTABLE(tex));
    g_clear_object(&tex);

    if (r->res.copied)
        set_status(pw, FALSE, "This stream is copied: the output frame is the input frame.", "dim-label");
    else {
        g_snprintf(text, sizeof(text), "A %.1f s clip around this frame was converted with the chosen "
                   "settings in %.1f s. The bitrate is the clip's: rate control may settle "
                   "differently over the whole file.", r->res.clip_seconds, r->res.encode_seconds);
        set_status(pw, FALSE, text, "dim-label");
    }
    if (pw->listener.rendered)
        pw->listener.rendered(pw->win, TRUE, pw->listener_user);
}

/* ------------------------------------------------------------------------- */
/* worker                                                                    */

static void render_free(Render *r)
{
    preview_result_free(&r->res);
    job_free(&r->job);
    g_free(r->scratch);
    g_free(r);
}

static void render_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    Render *r = data;

    r->ret = preview_render(&r->req, &r->cancel, &r->res, r->err, sizeof(r->err));
    g_task_return_boolean(task, TRUE);
}

static void render_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Render *r = data;
    PW *pw = r->pw;

    pw->running = NULL;
    if (pw->destroyed) {
        render_free(r);
        pw_free(pw);
        return;
    }
    if (pw->pending) {            /* outdated: render the latest choices */
        pw->pending = 0;
        render_free(r);
        request_render(pw);
        return;
    }
    show_result(pw, r);
    render_free(r);
}

/* The job's stream for the previewed input stream, or -1. */
static int job_stream(const ConvJob *job, int input_index)
{
    for (int i = 0; job && i < job->nb_streams; i++)
        if (job->streams[i].input_index == input_index && job->streams[i].action != JOB_DROP)
            return i;
    return -1;
}

static void request_render(PW *pw)
{
    static guint counter;
    Render *r;
    GTask *task;
    char name[64];
    int si;

    if (pw->running) {            /* cancel it; render again when it stops */
        atomic_store(&pw->running->cancel, 1);
        pw->pending = 1;
        return;
    }
    r = g_new0(Render, 1);
    r->pw  = pw;
    r->job = conv_window_build_job(pw->owner);
    si = pw->stream_input >= 0 ? job_stream(r->job, pw->stream_input) : -1;
    if (!r->job || pw->stream_input < 0 || si < 0) {
        set_status(pw, FALSE, pw->stream_input < 0 ? "This file has no video stream to preview."
                                                   : "This stream is dropped: there is nothing to preview.",
                   "dim-label");
        gtk_picture_set_paintable(GTK_PICTURE(pw->pic_before), NULL);
        gtk_picture_set_paintable(GTK_PICTURE(pw->pic_after), NULL);
        render_free(r);
        if (pw->listener.rendered)
            pw->listener.rendered(pw->win, FALSE, pw->listener_user);
        return;
    }
    g_snprintf(name, sizeof(name), "ffconv-preview-%p-%u.mkv", (void *)pw, ++counter);
    r->scratch    = g_build_filename(g_get_tmp_dir(), name, NULL);
    r->req.job    = r->job;
    r->req.stream = si;
    r->req.time   = gtk_range_get_value(GTK_RANGE(pw->scale));
    r->req.scratch = r->scratch;

    set_status(pw, TRUE, r->job->streams[si].action == JOB_COPY ? "Reading the frame…"
                                                                : "Converting a short clip around this frame…",
               "dim-label");
    pw->running = r;
    task = g_task_new(NULL, NULL, render_done, r);
    g_task_set_task_data(task, r, NULL);
    g_task_run_in_thread(task, render_thread);
    g_object_unref(task);
}

static gboolean delayed_render(gpointer data)
{
    PW *pw = data;

    pw->delay_id = 0;
    request_render(pw);
    return G_SOURCE_REMOVE;
}

static void schedule_render(PW *pw)
{
    if (pw->delay_id)
        g_source_remove(pw->delay_id);
    pw->delay_id = g_timeout_add(RENDER_DELAY_MS, delayed_render, pw);
}

/* ------------------------------------------------------------------------- */
/* controls                                                                  */

/* The video streams of the file, with what each becomes. */
static void fill_streams(PW *pw)
{
    const MediaInfo *mi = conv_window_media(pw->owner);
    ConvJob *job = conv_window_build_job(pw->owner);
    guint sel = GTK_INVALID_LIST_POSITION;
    int n = 0;

    pw->updating = 1;
    g_list_store_remove_all(pw->streams);
    for (int i = 0; mi && i < mi->nb_streams; i++) {
        const MediaStream *ms = &mi->streams[i];
        int si = job_stream(job, ms->index);
        ConvChoiceItem *it;
        char label[256];

        if (ms->type != AVMEDIA_TYPE_VIDEO)
            continue;
        g_snprintf(label, sizeof(label), "#%d %s %d×%d → %s", ms->index, ms->codec_name, ms->width, ms->height,
                   si < 0 ? "dropped" : job->streams[si].action == JOB_COPY ? "copy"
                   : job->streams[si].encoder ? job->streams[si].encoder : "?");
        it = conv_choice_item_new(label, GINT_TO_POINTER(ms->index + 1));
        conv_choice_item_set_state(it, si >= 0, si < 0 ? "This stream is dropped" : NULL);
        g_list_store_append(pw->streams, it);
        g_object_unref(it);
        if (ms->index == pw->stream_input || (pw->stream_input < 0 && sel == GTK_INVALID_LIST_POSITION))
            sel = n;
        n++;
    }
    if (sel != GTK_INVALID_LIST_POSITION) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(pw->stream_dd), sel);
        pw->stream_input = GPOINTER_TO_INT(conv_choice_item_get_data(
            gtk_drop_down_get_selected_item(GTK_DROP_DOWN(pw->stream_dd)))) - 1;
    } else {
        pw->stream_input = -1;
    }
    pw->updating = 0;
    job_free(&job);
}

static void on_stream_changed(GObject *o, GParamSpec *p, gpointer data)
{
    PW *pw = data;
    ConvChoiceItem *it;

    if (pw->updating || !(it = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(pw->stream_dd))))
        return;
    pw->stream_input = GPOINTER_TO_INT(conv_choice_item_get_data(it)) - 1;
    schedule_render(pw);
}

static void on_time_changed(GtkRange *range, gpointer data)
{
    PW *pw = data;
    char t[32];

    format_time(gtk_range_get_value(range), t, sizeof(t));
    gtk_label_set_text(GTK_LABEL(pw->time_label), t);
    schedule_render(pw);
}

static void on_zoom_toggled(GtkToggleButton *b, gpointer data)
{
    PW *pw = data;
    gboolean actual = gtk_toggle_button_get_active(b);

    /* fit: shrink into the view; actual pixels: natural size, scrollable */
    gtk_picture_set_can_shrink(GTK_PICTURE(pw->pic_before), !actual);
    gtk_picture_set_can_shrink(GTK_PICTURE(pw->pic_after), !actual);
}

static void on_job_changed(ConvWindow *w, void *user)
{
    PW *pw = user;

    fill_streams(pw);        /* the encoder in the labels may have changed */
    schedule_render(pw);
}

static const ConvWindowListener owner_listener = { .job_changed = on_job_changed };

/* ------------------------------------------------------------------------- */

static void pw_free(PW *pw)
{
    g_object_unref(pw->streams);
    g_free(pw);
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    PW *pw = data;

    if (pw->delay_id)
        g_source_remove(pw->delay_id);
    conv_window_remove_listener(pw->owner, pw->owner_listener);
    pw->destroyed = 1;
    if (pw->running)            /* freed by render_done() */
        atomic_store(&pw->running->cancel, 1);
    else
        pw_free(pw);
}

static GtkWidget *image_column(GtkWidget **caption, GtkWidget **picture, GtkWidget *share)
{
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6), *frame, *scroll;

    *caption = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(*caption), 0);
    gtk_label_set_ellipsize(GTK_LABEL(*caption), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(col), *caption);

    *picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(*picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(*picture), TRUE);
    gtk_widget_set_hexpand(*picture, TRUE);
    gtk_widget_set_vexpand(*picture, TRUE);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), *picture);
    if (share) {   /* scroll both views together at 100 % */
        GtkScrolledWindow *other = GTK_SCROLLED_WINDOW(gtk_widget_get_ancestor(share, GTK_TYPE_SCROLLED_WINDOW));
        gtk_scrolled_window_set_hadjustment(GTK_SCROLLED_WINDOW(scroll), gtk_scrolled_window_get_hadjustment(other));
        gtk_scrolled_window_set_vadjustment(GTK_SCROLLED_WINDOW(scroll), gtk_scrolled_window_get_vadjustment(other));
    }
    frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), scroll);
    gtk_box_append(GTK_BOX(col), frame);
    return col;
}

GtkWindow *preview_window_new(ConvWindow *owner)
{
    PW *pw = g_new0(PW, 1);
    const MediaInfo *mi = conv_window_media(owner);
    double duration = mi && mi->duration_us > 0 ? mi->duration_us / 1e6 : 0;
    GtkWidget *box, *controls, *label, *status_row, *images;

    pw->owner        = owner;
    pw->stream_input = -1;
    pw->streams      = g_list_store_new(CONV_TYPE_CHOICE_ITEM);

    pw->win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(pw->win, "Preview");
    gtk_window_set_transient_for(pw->win, conv_window_get(owner));
    gtk_window_set_destroy_with_parent(pw->win, TRUE);
    gtk_window_set_default_size(pw->win, 1200, 620);
    g_object_set_data(G_OBJECT(pw->win), "conv-preview", pw);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_window_set_child(pw->win, box);

    /* stream and time */
    controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    label = gtk_label_new("Stream");
    gtk_box_append(GTK_BOX(controls), label);
    pw->stream_dd = conv_choice_dropdown_new(pw->streams, FALSE);
    gtk_widget_set_size_request(pw->stream_dd, 280, -1);
    gtk_box_append(GTK_BOX(controls), pw->stream_dd);
    label = gtk_label_new("Time");
    gtk_widget_set_margin_start(label, 12);
    gtk_box_append(GTK_BOX(controls), label);
    pw->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, duration > 0 ? duration : 1, 0.04);
    gtk_scale_set_draw_value(GTK_SCALE(pw->scale), FALSE);
    gtk_widget_set_hexpand(pw->scale, TRUE);
    gtk_box_append(GTK_BOX(controls), pw->scale);
    pw->time_label = gtk_label_new("00:00.00");
    gtk_label_set_width_chars(GTK_LABEL(pw->time_label), 9);
    gtk_box_append(GTK_BOX(controls), pw->time_label);
    pw->zoom_btn = gtk_toggle_button_new_with_label("Actual pixels");
    gtk_widget_set_tooltip_text(pw->zoom_btn, "Show both frames at 100 %, scrolling together, "
                                "to compare details and compression");
    gtk_widget_set_margin_start(pw->zoom_btn, 12);
    gtk_box_append(GTK_BOX(controls), pw->zoom_btn);
    gtk_box_append(GTK_BOX(box), controls);

    status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    pw->spinner = gtk_spinner_new();
    gtk_box_append(GTK_BOX(status_row), pw->spinner);
    pw->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(pw->status), 0);
    gtk_label_set_wrap(GTK_LABEL(pw->status), TRUE);
    gtk_widget_set_hexpand(pw->status, TRUE);
    gtk_box_append(GTK_BOX(status_row), pw->status);
    gtk_box_append(GTK_BOX(box), status_row);

    images = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(images), TRUE);
    gtk_box_append(GTK_BOX(images), image_column(&pw->cap_before, &pw->pic_before, NULL));
    gtk_box_append(GTK_BOX(images), image_column(&pw->cap_after, &pw->pic_after, pw->pic_before));
    gtk_box_append(GTK_BOX(box), images);

    fill_streams(pw);
    /* start a little into the file: the first frames are often black */
    gtk_range_set_value(GTK_RANGE(pw->scale), duration > 0 ? MIN(duration * 0.1, 30.0) : 0);
    on_time_changed(GTK_RANGE(pw->scale), pw);   /* label + first render */

    g_signal_connect(pw->stream_dd, "notify::selected", G_CALLBACK(on_stream_changed), pw);
    g_signal_connect(pw->scale, "value-changed", G_CALLBACK(on_time_changed), pw);
    g_signal_connect(pw->zoom_btn, "toggled", G_CALLBACK(on_zoom_toggled), pw);
    g_signal_connect(pw->win, "destroy", G_CALLBACK(on_destroy), pw);
    pw->owner_listener = conv_window_add_listener(owner, &owner_listener, pw);
    return pw->win;
}

void preview_window_set_listener(GtkWindow *win, const PreviewWindowListener *l, void *user)
{
    PW *pw = pw_of(win);

    if (!pw)
        return;
    if (l)
        pw->listener = *l;
    else
        memset(&pw->listener, 0, sizeof(pw->listener));
    pw->listener_user = user;
}

void preview_window_set_time(GtkWindow *win, double seconds)
{
    PW *pw = pw_of(win);

    if (pw)
        gtk_range_set_value(GTK_RANGE(pw->scale), seconds);   /* -> on_time_changed */
}

void preview_window_set_actual_pixels(GtkWindow *win, gboolean actual)
{
    PW *pw = pw_of(win);

    if (pw)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->zoom_btn), actual);   /* -> on_zoom_toggled */
}

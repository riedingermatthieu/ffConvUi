/*
 * preview_window.c - one video frame before and after conversion.
 *
 * The worker renders with preview_render() on a snapshot of the job; the
 * window never waits for it. A new request while a render runs cancels it
 * and runs again with the latest choices; only the latest result is shown.
 *
 * Zoom and pan: both frames are drawn by a small canvas widget in the
 * original frame's geometry (the result is scaled into it, keeping its
 * aspect), inside scrolled windows that share their adjustments, so the two
 * views always show the same region. Ctrl+wheel and pinch zoom around the
 * pointer, dragging pans, double-click switches between Fit and 100 %.
 */
#include "preview_window.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include <libavutil/mem.h>

#include "choice_item.h"
#include "preview.h"
#include "stream_row.h"
#include "window.h"

#define RENDER_DELAY_MS 300
#define ZOOM_MIN        0.1
#define ZOOM_MAX        16.0

typedef struct Render Render;

typedef struct PW {
    ConvWindow      *owner;
    guint            owner_listener;
    GtkWindow       *win;
    GtkWidget       *stream_dd, *scale, *time_label;
    GtkWidget       *spinner, *status;
    GtkWidget       *pic_before, *pic_after, *cap_before, *cap_after;
    GtkWidget       *scroll_before, *scroll_after;
    GtkWidget       *zoom_label, *zoom_in_btn, *zoom_out_btn, *fit_btn, *one_btn;
    GListStore      *streams;          /* ConvChoiceItem, data: GINT_TO_POINTER(input index + 1) */
    int              stream_input;     /* input index of the previewed stream, -1 = none */
    guint            delay_id;
    int              updating;
    /* zoom and pan */
    double           zoom;             /* scale of the original frame, 0 = fit the view */
    int              ref_w, ref_h;     /* the original frame's size: the geometry shown */
    double           center_x, center_y; /* frame point (fractions) kept in the middle of
                                          the view when the frame changes size */
    double           drag_h, drag_v;   /* adjustment values when a drag started */
    double           pinch_zoom;       /* scale when a pinch started */
    double           ptr_x, ptr_y;     /* pointer over a view, -1 = none */
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
/* canvas: one frame, drawn at the window's zoom                             */

#define PV_TYPE_CANVAS (pv_canvas_get_type())
G_DECLARE_FINAL_TYPE(PvCanvas, pv_canvas, PV, CANVAS, GtkWidget)

struct _PvCanvas {
    GtkWidget   parent;
    PW         *pw;
    GdkTexture *tex;
};

G_DEFINE_FINAL_TYPE(PvCanvas, pv_canvas, GTK_TYPE_WIDGET)

/* The scale that fits the original frame in width x height. */
static double fit_scale(const PW *pw, double width, double height)
{
    if (pw->ref_w <= 0 || pw->ref_h <= 0 || width <= 0 || height <= 0)
        return 1;
    return MIN(width / pw->ref_w, height / pw->ref_h);
}

static void pv_canvas_measure(GtkWidget *w, GtkOrientation o, int for_size, int *min, int *nat,
                              int *min_base, int *nat_base)
{
    PvCanvas *c = PV_CANVAS(w);
    int ref = o == GTK_ORIENTATION_HORIZONTAL ? c->pw->ref_w : c->pw->ref_h;

    if (c->pw->zoom > 0 && ref > 0) {
        *min = *nat = (int)ceil(ref * c->pw->zoom);   /* larger than the view: scrolls */
    } else {
        *min = 0;                                     /* fit: takes the view's size */
        *nat = MAX(ref, 0);
    }
}

static void pv_canvas_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
    PvCanvas *c = PV_CANVAS(w);
    const PW *pw = c->pw;
    double width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    double s, tw, th, k;

    if (!c->tex || pw->ref_w <= 0 || pw->ref_h <= 0)
        return;
    s = pw->zoom > 0 ? pw->zoom : fit_scale(pw, width, height);
    /* the texture in the original's box, keeping its own aspect: a result
     * scaled by a filter fills it, a cropped one is centred in it */
    tw = gdk_texture_get_width(c->tex);
    th = gdk_texture_get_height(c->tex);
    k  = MIN(pw->ref_w * s / tw, pw->ref_h * s / th);
    tw *= k;
    th *= k;
    gtk_snapshot_append_scaled_texture(snap, c->tex,
                                       k >= 2 ? GSK_SCALING_FILTER_NEAREST     /* show the pixels */
                                              : GSK_SCALING_FILTER_TRILINEAR,
                                       &GRAPHENE_RECT_INIT((width - tw) / 2, (height - th) / 2, tw, th));
}

static void pv_canvas_dispose(GObject *o)
{
    g_clear_object(&PV_CANVAS(o)->tex);
    G_OBJECT_CLASS(pv_canvas_parent_class)->dispose(o);
}

static void pv_canvas_class_init(PvCanvasClass *k)
{
    G_OBJECT_CLASS(k)->dispose    = pv_canvas_dispose;
    GTK_WIDGET_CLASS(k)->measure  = pv_canvas_measure;
    GTK_WIDGET_CLASS(k)->snapshot = pv_canvas_snapshot;
}

static void pv_canvas_init(PvCanvas *c)
{
    gtk_widget_set_overflow(GTK_WIDGET(c), GTK_OVERFLOW_HIDDEN);
}

static GtkWidget *pv_canvas_new(PW *pw)
{
    PvCanvas *c = g_object_new(PV_TYPE_CANVAS, NULL);

    c->pw = pw;
    return GTK_WIDGET(c);
}

static void pv_canvas_set_texture(GtkWidget *w, GdkTexture *tex)
{
    g_set_object(&PV_CANVAS(w)->tex, tex);
    gtk_widget_queue_draw(w);
}

/* ------------------------------------------------------------------------- */
/* zoom and pan                                                              */

static GtkAdjustment *hadj(PW *pw)
{
    return gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(pw->scroll_before));
}

static GtkAdjustment *vadj(PW *pw)
{
    return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(pw->scroll_before));
}

/* The views' size (both views have the same). */
static void view_size(PW *pw, double *w, double *h)
{
    *w = gtk_widget_get_width(pw->scroll_before);
    *h = gtk_widget_get_height(pw->scroll_before);
}

static double current_scale(PW *pw)
{
    double vw, vh;

    if (pw->zoom > 0)
        return pw->zoom;
    view_size(pw, &vw, &vh);
    return fit_scale(pw, vw, vh);
}

static void update_zoom_controls(PW *pw)
{
    double s = current_scale(pw);
    const char *cursor = pw->zoom > 0 ? "grab" : NULL;
    char text[32];

    if (pw->zoom > 0)
        g_snprintf(text, sizeof(text), "%.0f %%", s * 100);
    else if (pw->ref_w > 0)
        g_snprintf(text, sizeof(text), "Fit (%.0f %%)", s * 100);
    else
        g_strlcpy(text, "Fit", sizeof(text));
    gtk_label_set_text(GTK_LABEL(pw->zoom_label), text);
    gtk_widget_set_sensitive(pw->zoom_in_btn, s < ZOOM_MAX - 1e-6);
    gtk_widget_set_sensitive(pw->zoom_out_btn, pw->zoom > 0);
    gtk_widget_set_sensitive(pw->fit_btn, pw->zoom > 0);
    gtk_widget_set_sensitive(pw->one_btn, fabs(pw->zoom - 1) > 1e-6);
    gtk_widget_set_cursor_from_name(pw->pic_before, cursor);
    gtk_widget_set_cursor_from_name(pw->pic_after, cursor);
}

/* Set one axis of the shared adjustments so that frame coordinate `img`
 * (original pixels) is at `at` in the view, at scale s. The adjustment is
 * configured with the size the content is about to have: the viewport has
 * not been laid out again yet, and would clamp the value to the old size. */
static void place_axis(GtkAdjustment *adj, double ref, double s, double view, double img, double at)
{
    double content = MAX(ref * s, view), off = (content - ref * s) / 2;
    double v = CLAMP(img * s + off - at, 0, MAX(content - view, 0));

    gtk_adjustment_configure(adj, v, 0, content, view * 0.1, view * 0.9, view);
}

/* The frame point under view position (ax, ay), in original pixels. */
static void frame_point(PW *pw, double ax, double ay, double *ix, double *iy)
{
    double vw, vh, s = current_scale(pw), cw, ch;
    double hv = pw->zoom > 0 ? gtk_adjustment_get_value(hadj(pw)) : 0;
    double vv = pw->zoom > 0 ? gtk_adjustment_get_value(vadj(pw)) : 0;

    view_size(pw, &vw, &vh);
    cw  = MAX(pw->ref_w * s, vw);
    ch  = MAX(pw->ref_h * s, vh);
    *ix = (hv + ax - (cw - pw->ref_w * s) / 2) / s;
    *iy = (vv + ay - (ch - pw->ref_h * s) / 2) / s;
}

/* Remember which part of the frame is in the middle of the view. */
static void save_center(PW *pw)
{
    double vw, vh, ix, iy;

    if (pw->ref_w <= 0 || pw->zoom <= 0)
        return;
    view_size(pw, &vw, &vh);
    frame_point(pw, vw / 2, vh / 2, &ix, &iy);
    pw->center_x = ix / pw->ref_w;
    pw->center_y = iy / pw->ref_h;
}

static void center_on(PW *pw, double fx, double fy)
{
    double vw, vh;

    pw->center_x = fx;
    pw->center_y = fy;
    if (pw->ref_w <= 0 || pw->zoom <= 0)
        return;   /* applied when a frame is shown */
    view_size(pw, &vw, &vh);
    place_axis(hadj(pw), pw->ref_w, pw->zoom, vw, fx * pw->ref_w, vw / 2);
    place_axis(vadj(pw), pw->ref_h, pw->zoom, vh, fy * pw->ref_h, vh / 2);
}

/* Zoom to `zoom` (0 = fit), keeping the frame point under (ax, ay) where it
 * is in the view; ax < 0: the view's centre. */
static void set_zoom(PW *pw, double zoom, double ax, double ay)
{
    double vw, vh, ix = 0, iy = 0;

    view_size(pw, &vw, &vh);
    if (zoom > 0) {
        zoom = CLAMP(zoom, ZOOM_MIN, ZOOM_MAX);
        /* zooming out below "fit" means fit (100 % stays reachable) */
        if (pw->ref_w > 0 && zoom < current_scale(pw) && zoom <= fit_scale(pw, vw, vh) + 1e-6 &&
            fabs(zoom - 1) > 1e-6)
            zoom = 0;
    }
    if (ax < 0) {
        ax = vw / 2;
        ay = vh / 2;
    }
    if (pw->ref_w > 0)
        frame_point(pw, ax, ay, &ix, &iy);

    pw->zoom = zoom;
    gtk_widget_queue_resize(pw->pic_before);
    gtk_widget_queue_resize(pw->pic_after);
    if (pw->ref_w > 0) {
        if (zoom > 0) {
            place_axis(hadj(pw), pw->ref_w, zoom, vw, ix, ax);
            place_axis(vadj(pw), pw->ref_h, zoom, vh, iy, ay);
            save_center(pw);
        } else {
            gtk_adjustment_set_value(hadj(pw), 0);
            gtk_adjustment_set_value(vadj(pw), 0);
        }
    }
    update_zoom_controls(pw);
}

static const double zoom_stops[] = {
    0.1, 0.125, 0.25, 0.333, 0.5, 0.667, 1, 1.5, 2, 3, 4, 6, 8, 12, 16,
};

static void zoom_step(PW *pw, int dir, double ax, double ay)
{
    double s = current_scale(pw), target = dir > 0 ? ZOOM_MAX : 0;

    if (dir > 0) {
        for (guint i = 0; i < G_N_ELEMENTS(zoom_stops); i++)
            if (zoom_stops[i] > s * 1.01) {
                target = zoom_stops[i];
                break;
            }
    } else {
        for (guint i = G_N_ELEMENTS(zoom_stops); i-- > 0;)
            if (zoom_stops[i] < s * 0.99) {
                target = zoom_stops[i];
                break;
            }
    }
    set_zoom(pw, target, ax, ay);
}

static void on_zoom_in(GtkButton *b, gpointer data)  { zoom_step(data, +1, -1, -1); }
static void on_zoom_out(GtkButton *b, gpointer data) { zoom_step(data, -1, -1, -1); }
static void on_zoom_fit(GtkButton *b, gpointer data) { set_zoom(data, 0, -1, -1); }
static void on_zoom_one(GtkButton *b, gpointer data) { set_zoom(data, 1, -1, -1); }

static gboolean on_scroll(GtkEventControllerScroll *sc, double dx, double dy, gpointer data)
{
    PW *pw = data;
    GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(sc));

    if (!(mods & GDK_CONTROL_MASK) || pw->ref_w <= 0)
        return FALSE;   /* plain wheel: scroll */
    if (gtk_event_controller_scroll_get_unit(sc) == GDK_SCROLL_UNIT_SURFACE)
        dy /= 40;       /* touchpads report pixels */
    if (dy != 0)
        set_zoom(pw, current_scale(pw) * pow(1.2, -dy), pw->ptr_x, pw->ptr_y);
    return TRUE;
}

static void on_motion(GtkEventControllerMotion *m, double x, double y, gpointer data)
{
    PW *pw = data;

    pw->ptr_x = x;
    pw->ptr_y = y;
}

static void on_leave(GtkEventControllerMotion *m, gpointer data)
{
    PW *pw = data;

    pw->ptr_x = pw->ptr_y = -1;
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer data)
{
    PW *pw = data;

    if (pw->zoom <= 0) {   /* the whole frame is shown: nothing to pan */
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    pw->drag_h = gtk_adjustment_get_value(hadj(pw));
    pw->drag_v = gtk_adjustment_get_value(vadj(pw));
    gtk_widget_set_cursor_from_name(pw->pic_before, "grabbing");
    gtk_widget_set_cursor_from_name(pw->pic_after, "grabbing");
}

static void on_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    PW *pw = data;

    gtk_adjustment_set_value(hadj(pw), pw->drag_h - ox);
    gtk_adjustment_set_value(vadj(pw), pw->drag_v - oy);
}

static void on_drag_end(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    PW *pw = data;

    save_center(pw);
    update_zoom_controls(pw);   /* back to the "grab" cursor */
}

static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    PW *pw = data;

    if (n == 2 && pw->ref_w > 0)
        set_zoom(pw, pw->zoom > 0 ? 0 : 1, x, y);   /* fit <-> 100 % at the pointer */
}

static void on_pinch_begin(GtkGesture *g, GdkEventSequence *seq, gpointer data)
{
    PW *pw = data;

    pw->pinch_zoom = current_scale(pw);
}

static void on_pinch(GtkGestureZoom *g, double scale, gpointer data)
{
    PW *pw = data;
    double x, y;

    if (!gtk_gesture_get_bounding_box_center(GTK_GESTURE(g), &x, &y))
        x = y = -1;
    set_zoom(pw, pw->pinch_zoom * scale, x, y);
}

static gboolean on_key(GtkEventControllerKey *k, guint keyval, guint code, GdkModifierType mods, gpointer data)
{
    PW *pw = data;

    if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK))
        return FALSE;
    switch (keyval) {
    case GDK_KEY_plus: case GDK_KEY_KP_Add: case GDK_KEY_equal:
        zoom_step(pw, +1, -1, -1);
        return TRUE;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
        zoom_step(pw, -1, -1, -1);
        return TRUE;
    case GDK_KEY_0: case GDK_KEY_KP_0:
        set_zoom(pw, 0, -1, -1);
        return TRUE;
    case GDK_KEY_1: case GDK_KEY_KP_1:
        set_zoom(pw, 1, -1, -1);
        return TRUE;
    default:
        return FALSE;
    }
}

/* Zoom, pan and double-click on a view. */
static void add_view_controllers(PW *pw, GtkWidget *scroll)
{
    GtkEventController *ec;
    GtkGesture *g;

    ec = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_event_controller_set_propagation_phase(ec, GTK_PHASE_CAPTURE);   /* before it scrolls */
    g_signal_connect(ec, "scroll", G_CALLBACK(on_scroll), pw);
    gtk_widget_add_controller(scroll, ec);

    ec = gtk_event_controller_motion_new();
    g_signal_connect(ec, "motion", G_CALLBACK(on_motion), pw);
    g_signal_connect(ec, "enter", G_CALLBACK(on_motion), pw);
    g_signal_connect(ec, "leave", G_CALLBACK(on_leave), pw);
    gtk_widget_add_controller(scroll, ec);

    g = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);   /* left or middle button */
    g_signal_connect(g, "drag-begin", G_CALLBACK(on_drag_begin), pw);
    g_signal_connect(g, "drag-update", G_CALLBACK(on_drag_update), pw);
    g_signal_connect(g, "drag-end", G_CALLBACK(on_drag_end), pw);
    gtk_widget_add_controller(scroll, GTK_EVENT_CONTROLLER(g));

    g = gtk_gesture_click_new();
    g_signal_connect(g, "pressed", G_CALLBACK(on_click), pw);
    gtk_widget_add_controller(scroll, GTK_EVENT_CONTROLLER(g));

    g = gtk_gesture_zoom_new();
    g_signal_connect(g, "begin", G_CALLBACK(on_pinch_begin), pw);
    g_signal_connect(g, "scale-changed", G_CALLBACK(on_pinch), pw);
    gtk_widget_add_controller(scroll, GTK_EVENT_CONTROLLER(g));
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

static void show_frames(PW *pw, Render *r)
{
    /* a frame of another size (another stream): keep the same part in view */
    gboolean resized = r->res.before.width != pw->ref_w || r->res.before.height != pw->ref_h;
    GdkTexture *tex;

    pw->ref_w = r->res.before.width;
    pw->ref_h = r->res.before.height;
    tex = texture_of(&r->res.before);
    pv_canvas_set_texture(pw->pic_before, tex);
    g_clear_object(&tex);
    tex = texture_of(&r->res.after);
    pv_canvas_set_texture(pw->pic_after, tex);
    g_clear_object(&tex);
    if (resized) {
        gtk_widget_queue_resize(pw->pic_before);
        gtk_widget_queue_resize(pw->pic_after);
        center_on(pw, pw->center_x, pw->center_y);
    }
    update_zoom_controls(pw);
}

static void show_result(PW *pw, Render *r)
{
    char t[32], text[512];

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

    show_frames(pw, r);

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
        pv_canvas_set_texture(pw->pic_before, NULL);
        pv_canvas_set_texture(pw->pic_after, NULL);
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

/* A caption and a scrollable view; `share`: the view to pan together with. */
static GtkWidget *image_column(PW *pw, GtkWidget **caption, GtkWidget **picture, GtkWidget **view,
                               GtkWidget *share)
{
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6), *frame, *scroll;

    *caption = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(*caption), 0);
    gtk_label_set_ellipsize(GTK_LABEL(*caption), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(col), *caption);

    *picture = pv_canvas_new(pw);
    gtk_widget_set_hexpand(*picture, TRUE);
    gtk_widget_set_vexpand(*picture, TRUE);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), *picture);
    gtk_widget_set_vexpand(scroll, TRUE);
    if (share) {
        GtkScrolledWindow *other = GTK_SCROLLED_WINDOW(share);
        gtk_scrolled_window_set_hadjustment(GTK_SCROLLED_WINDOW(scroll), gtk_scrolled_window_get_hadjustment(other));
        gtk_scrolled_window_set_vadjustment(GTK_SCROLLED_WINDOW(scroll), gtk_scrolled_window_get_vadjustment(other));
    }
    add_view_controllers(pw, scroll);
    *view = scroll;
    frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), scroll);
    gtk_box_append(GTK_BOX(col), frame);
    return col;
}

/* − level + | Fit | 1:1 */
static GtkWidget *zoom_controls(PW *pw)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6), *linked;

    gtk_widget_set_margin_start(box, 12);
    linked = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(linked, "linked");
    pw->zoom_out_btn = gtk_button_new_from_icon_name("zoom-out-symbolic");
    gtk_widget_set_tooltip_text(pw->zoom_out_btn, "Zoom out (−, Ctrl+wheel)");
    pw->zoom_label = gtk_label_new("Fit");
    gtk_label_set_width_chars(GTK_LABEL(pw->zoom_label), 11);
    gtk_widget_set_margin_start(pw->zoom_label, 6);
    gtk_widget_set_margin_end(pw->zoom_label, 6);
    gtk_widget_set_tooltip_text(pw->zoom_label, "Ctrl+wheel or pinch on a frame to zoom around the pointer, "
                                "drag to pan, double-click for Fit / 100 %");
    pw->zoom_in_btn = gtk_button_new_from_icon_name("zoom-in-symbolic");
    gtk_widget_set_tooltip_text(pw->zoom_in_btn, "Zoom in (+, Ctrl+wheel)");
    gtk_box_append(GTK_BOX(linked), pw->zoom_out_btn);
    gtk_box_append(GTK_BOX(linked), pw->zoom_label);
    gtk_box_append(GTK_BOX(linked), pw->zoom_in_btn);
    gtk_box_append(GTK_BOX(box), linked);

    pw->fit_btn = gtk_button_new_with_label("Fit");
    gtk_widget_set_tooltip_text(pw->fit_btn, "Show the whole frame (0)");
    pw->one_btn = gtk_button_new_with_label("1:1");
    gtk_widget_set_tooltip_text(pw->one_btn, "Actual pixels: one frame pixel per screen pixel (1)");
    gtk_box_append(GTK_BOX(box), pw->fit_btn);
    gtk_box_append(GTK_BOX(box), pw->one_btn);

    g_signal_connect(pw->zoom_in_btn, "clicked", G_CALLBACK(on_zoom_in), pw);
    g_signal_connect(pw->zoom_out_btn, "clicked", G_CALLBACK(on_zoom_out), pw);
    g_signal_connect(pw->fit_btn, "clicked", G_CALLBACK(on_zoom_fit), pw);
    g_signal_connect(pw->one_btn, "clicked", G_CALLBACK(on_zoom_one), pw);
    return box;
}

GtkWindow *preview_window_new(ConvWindow *owner)
{
    PW *pw = g_new0(PW, 1);
    const MediaInfo *mi = conv_window_media(owner);
    double duration = mi && mi->duration_us > 0 ? mi->duration_us / 1e6 : 0;
    GtkWidget *box, *controls, *label, *status_row, *images;
    GtkEventController *keys;

    pw->owner        = owner;
    pw->stream_input = -1;
    pw->streams      = g_list_store_new(CONV_TYPE_CHOICE_ITEM);
    pw->center_x     = pw->center_y = 0.5;
    pw->ptr_x        = pw->ptr_y = -1;

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

    /* stream, time, zoom */
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
    gtk_box_append(GTK_BOX(controls), zoom_controls(pw));
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
    gtk_box_append(GTK_BOX(images), image_column(pw, &pw->cap_before, &pw->pic_before, &pw->scroll_before, NULL));
    gtk_box_append(GTK_BOX(images), image_column(pw, &pw->cap_after, &pw->pic_after, &pw->scroll_after,
                                                 pw->scroll_before));
    gtk_box_append(GTK_BOX(box), images);
    update_zoom_controls(pw);

    /* + − 0 1, whatever has the focus */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), pw);
    gtk_widget_add_controller(GTK_WIDGET(pw->win), keys);

    fill_streams(pw);
    /* start a little into the file: the first frames are often black */
    gtk_range_set_value(GTK_RANGE(pw->scale), duration > 0 ? MIN(duration * 0.1, 30.0) : 0);
    on_time_changed(GTK_RANGE(pw->scale), pw);   /* label + first render */

    g_signal_connect(pw->stream_dd, "notify::selected", G_CALLBACK(on_stream_changed), pw);
    g_signal_connect(pw->scale, "value-changed", G_CALLBACK(on_time_changed), pw);
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

void preview_window_set_zoom(GtkWindow *win, double zoom)
{
    PW *pw = pw_of(win);

    if (pw)
        set_zoom(pw, zoom, -1, -1);
}

double preview_window_get_zoom(GtkWindow *win)
{
    PW *pw = pw_of(win);

    return pw ? pw->zoom : 0;
}

void preview_window_center_on(GtkWindow *win, double fx, double fy)
{
    PW *pw = pw_of(win);

    if (pw)
        center_on(pw, fx, fy);
}

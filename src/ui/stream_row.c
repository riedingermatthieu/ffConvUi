/*
 * stream_row.c - stream table row.
 */
#include "stream_row.h"

#include <string.h>

#include <libavutil/avutil.h>

struct StreamRow {
    const MediaStream  *ms;
    GtkWidget          *box;
    GtkWidget          *action_dd;
    GtkWidget          *encoder_dd;
    JobAction           actions[3];      /* dropdown position -> action */
    int                 nb_actions;
    const CapsEncoder **encoders;        /* dropdown position -> encoder */
    int                 nb_encoders;
    StreamRowChanged    changed;
    void               *user;
};

static const char *type_icon(enum AVMediaType t)
{
    switch (t) {
    case AVMEDIA_TYPE_VIDEO:    return "video-x-generic-symbolic";
    case AVMEDIA_TYPE_AUDIO:    return "audio-x-generic-symbolic";
    case AVMEDIA_TYPE_SUBTITLE: return "media-view-subtitles-symbolic";
    default:                    return "text-x-generic-symbolic";
    }
}

char *stream_describe(const MediaStream *ms)
{
    GString *s = g_string_new(ms->codec_name[0] ? ms->codec_name : "unknown codec");

    if (ms->profile[0])
        g_string_append_printf(s, " (%s)", ms->profile);
    switch (ms->type) {
    case AVMEDIA_TYPE_VIDEO:
        if (ms->width > 0)
            g_string_append_printf(s, " · %d×%d", ms->width, ms->height);
        if (ms->is_attached_pic)
            g_string_append(s, " · cover art");
        else if (ms->guessed_frame_rate.num > 0 && ms->guessed_frame_rate.den > 0) {
            char fps[32], *p;
            g_snprintf(fps, sizeof(fps), "%.3f", av_q2d(ms->guessed_frame_rate));
            for (p = fps + strlen(fps) - 1; *p == '0'; p--)   /* 29.970 -> 29.97, 25.000 -> 25 */
                *p = 0;
            if (*p == '.')
                *p = 0;
            g_string_append_printf(s, " · %s fps", fps);
        }
        if (ms->pix_fmt_name[0])
            g_string_append_printf(s, " · %s", ms->pix_fmt_name);
        break;
    case AVMEDIA_TYPE_AUDIO:
        if (ms->sample_rate > 0)
            g_string_append_printf(s, " · %g kHz", ms->sample_rate / 1000.0);
        if (ms->ch_layout[0])
            g_string_append_printf(s, " · %s", ms->ch_layout);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        g_string_append(s, ms->is_text_sub ? " · text" : ms->is_bitmap_sub ? " · bitmap" : "");
        break;
    case AVMEDIA_TYPE_ATTACHMENT:
        if (ms->filename[0])
            g_string_append_printf(s, " · %s", ms->filename);
        break;
    default:
        break;
    }
    if (ms->language[0] && strcmp(ms->language, "und"))
        g_string_append_printf(s, " · %s", ms->language);
    if (ms->title[0])
        g_string_append_printf(s, " · “%s”", ms->title);
    if (ms->disposition & AV_DISPOSITION_DEFAULT)
        g_string_append(s, " · default");
    return g_string_free(s, FALSE);
}

static const char *action_label(JobAction a)
{
    switch (a) {
    case JOB_COPY:      return "Copy";
    case JOB_TRANSCODE: return "Convert";
    default:            return "Drop";
    }
}

static void on_action_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
    StreamRow *row = data;

    gtk_widget_set_visible(row->encoder_dd, stream_row_action(row) == JOB_TRANSCODE);
    if (row->changed)
        row->changed(row->user);
}

static void on_encoder_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
    StreamRow *row = data;

    if (row->changed)
        row->changed(row->user);
}

StreamRow *stream_row_new(const Caps *caps, const CapsMuxer *mux, const MediaStream *ms,
                          int prev_action, const char *prev_encoder,
                          StreamRowChanged changed, void *user)
{
    StreamRow *row = g_new0(StreamRow, 1);
    GtkStringList *actions = gtk_string_list_new(NULL);
    GtkStringList *encoders = gtk_string_list_new(NULL);
    GtkWidget *icon, *index, *details;
    CapsStreamActions act = { 0 };
    const CapsEncoder *def_enc = NULL;
    CapsDefault def = CAPS_DEFAULT_DROP;
    guint sel_action = 0, sel_encoder = 0;
    char *desc, *markup;

    row->ms = ms;

    if (caps_stream_actions(caps, mux, ms, 0, &act) >= 0)
        def = caps_default_action(&act, &def_enc);

    /* actions the container allows */
    if (act.copy != CAPS_NO)
        row->actions[row->nb_actions++] = JOB_COPY;
    if (act.can_transcode && act.nb_encoders)
        row->actions[row->nb_actions++] = JOB_TRANSCODE;
    row->actions[row->nb_actions++] = JOB_DROP;
    for (int i = 0; i < row->nb_actions; i++) {
        JobAction a = row->actions[i];
        char label[64];
        g_snprintf(label, sizeof(label), "%s%s", action_label(a),
                   a == JOB_COPY && act.copy == CAPS_MAYBE ? " (?)" : "");
        gtk_string_list_append(actions, label);
        if ((prev_action >= 0 && (int)a == prev_action) ||
            (prev_action < 0 && ((def == CAPS_DEFAULT_COPY && a == JOB_COPY) ||
                                 (def == CAPS_DEFAULT_TRANSCODE && a == JOB_TRANSCODE) ||
                                 (def == CAPS_DEFAULT_DROP && a == JOB_DROP))))
            sel_action = i;
    }

    /* encoders, best first */
    row->encoders = g_new0(const CapsEncoder *, act.nb_encoders ? act.nb_encoders : 1);
    for (int i = 0; i < act.nb_encoders; i++) {
        const CapsEncChoice *c = &act.encoders[i];
        char label[128];
        g_snprintf(label, sizeof(label), "%s%s%s", c->enc->name,
                   c->enc->is_hardware ? " (hardware)" : "", c->compat == CAPS_MAYBE ? " (?)" : "");
        gtk_string_list_append(encoders, label);
        row->encoders[row->nb_encoders] = c->enc;
        if ((prev_encoder && !strcmp(prev_encoder, c->enc->name)) ||
            (!prev_encoder && c->enc == def_enc))
            sel_encoder = row->nb_encoders;
        row->nb_encoders++;
    }

    /* widgets */
    row->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(row->box, 6);
    gtk_widget_set_margin_bottom(row->box, 6);
    gtk_widget_set_margin_start(row->box, 8);
    gtk_widget_set_margin_end(row->box, 8);

    icon = gtk_image_new_from_icon_name(type_icon(ms->type));
    gtk_box_append(GTK_BOX(row->box), icon);

    index = gtk_label_new(NULL);
    markup = g_markup_printf_escaped("<b>#%d</b>", ms->index);
    gtk_label_set_markup(GTK_LABEL(index), markup);
    g_free(markup);
    gtk_label_set_width_chars(GTK_LABEL(index), 3);
    gtk_box_append(GTK_BOX(row->box), index);

    desc = stream_describe(ms);
    details = gtk_label_new(desc);
    gtk_widget_set_tooltip_text(details, desc);
    g_free(desc);
    gtk_label_set_xalign(GTK_LABEL(details), 0);
    gtk_label_set_ellipsize(GTK_LABEL(details), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(details, TRUE);
    gtk_box_append(GTK_BOX(row->box), details);

    row->action_dd = gtk_drop_down_new(G_LIST_MODEL(actions), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(row->action_dd), sel_action);
    if (act.note)
        gtk_widget_set_tooltip_text(row->action_dd, act.note);
    gtk_box_append(GTK_BOX(row->box), row->action_dd);

    row->encoder_dd = gtk_drop_down_new(G_LIST_MODEL(encoders),
        gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string"));
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(row->encoder_dd), TRUE);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(row->encoder_dd), sel_encoder);
    gtk_widget_set_size_request(row->encoder_dd, 190, -1);
    gtk_widget_set_tooltip_text(row->encoder_dd, "Encoder (type to search)");
    gtk_box_append(GTK_BOX(row->box), row->encoder_dd);
    gtk_widget_set_visible(row->encoder_dd, stream_row_action(row) == JOB_TRANSCODE);

    caps_stream_actions_free(&act);

    /* connect last: building the row must not fire change notifications */
    row->changed = changed;
    row->user    = user;
    g_signal_connect(row->action_dd, "notify::selected", G_CALLBACK(on_action_changed), row);
    g_signal_connect(row->encoder_dd, "notify::selected", G_CALLBACK(on_encoder_changed), row);
    g_object_ref_sink(row->box);
    return row;
}

void stream_row_free(StreamRow *row)
{
    if (!row)
        return;
    g_signal_handlers_disconnect_by_data(row->action_dd, row);
    g_signal_handlers_disconnect_by_data(row->encoder_dd, row);
    g_object_unref(row->box);
    g_free(row->encoders);
    g_free(row);
}

GtkWidget *stream_row_widget(const StreamRow *row)
{
    return row->box;
}

int stream_row_input_index(const StreamRow *row)
{
    return row->ms->index;
}

JobAction stream_row_action(const StreamRow *row)
{
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(row->action_dd));
    return i < (guint)row->nb_actions ? row->actions[i] : JOB_DROP;
}

gboolean stream_row_select(StreamRow *row, JobAction action, const char *encoder)
{
    int a = -1, e = -1;

    for (int i = 0; i < row->nb_actions; i++)
        if (row->actions[i] == action)
            a = i;
    if (a < 0)
        return FALSE;
    if (action == JOB_TRANSCODE) {
        for (int i = 0; encoder && i < row->nb_encoders; i++)
            if (!strcmp(row->encoders[i]->name, encoder))
                e = i;
        if (e < 0)
            return FALSE;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(row->encoder_dd), e);
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(row->action_dd), a);
    return TRUE;
}

const char *stream_row_encoder(const StreamRow *row)
{
    guint i;

    if (stream_row_action(row) != JOB_TRANSCODE)
        return NULL;
    i = gtk_drop_down_get_selected(GTK_DROP_DOWN(row->encoder_dd));
    return i < (guint)row->nb_encoders ? row->encoders[i]->name : NULL;
}

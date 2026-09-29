/*
 * stream_row.c - stream table row.
 *
 * Every action and every encoder of the stream's type is listed. Those the
 * current container cannot take are greyed out, with the reason, but stay
 * selectable: nothing is changed behind the user's back, validation reports
 * the problem and keeps Convert disabled.
 */
#include "stream_row.h"

#include <string.h>

#include <libavutil/avutil.h>

#include "choice_item.h"

struct StreamRow {
    const MediaStream  *ms;
    GtkWidget          *box;
    GtkWidget          *action_dd;
    GtkWidget          *encoder_dd;
    GtkWidget          *options_btn;
    GListStore         *actions;         /* ConvChoiceItem, data: GINT_TO_POINTER(action + 1) */
    GListStore         *encoders;        /* ConvChoiceItem, data: const CapsEncoder * */
    GHashTable         *options;         /* encoder name -> OptBox (only what the user set) */
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

/* ------------------------------------------------------------------------- */
/* current choice                                                            */

static ConvChoiceItem *selected_item(GtkWidget *dd)
{
    return gtk_drop_down_get_selected_item(GTK_DROP_DOWN(dd));
}

JobAction stream_row_action(const StreamRow *row)
{
    ConvChoiceItem *it = selected_item(row->action_dd);
    return it ? (JobAction)(GPOINTER_TO_INT(conv_choice_item_get_data(it)) - 1) : JOB_DROP;
}

static const CapsEncoder *current_encoder(const StreamRow *row)
{
    ConvChoiceItem *it;

    if (stream_row_action(row) != JOB_TRANSCODE || !(it = selected_item(row->encoder_dd)))
        return NULL;
    return conv_choice_item_get_data(it);
}

const char *stream_row_encoder(const StreamRow *row)
{
    const CapsEncoder *enc = current_encoder(row);
    return enc ? enc->name : NULL;
}

int stream_row_input_index(const StreamRow *row)
{
    return row->ms->index;
}

GtkWidget *stream_row_widget(const StreamRow *row)
{
    return row->box;
}

gboolean stream_row_fits(const StreamRow *row, const CapsMuxer *m, char *why, size_t size)
{
    const CapsEncoder *enc;
    JobAction a = stream_row_action(row);

    if (a == JOB_DROP)
        return TRUE;
    if (a == JOB_COPY) {
        if (caps_copy_compat(m, row->ms) != CAPS_NO)
            return TRUE;
        g_snprintf(why, size, "cannot store stream #%d as is (%s)", row->ms->index, row->ms->codec_name);
        return FALSE;
    }
    if (!(enc = current_encoder(row)) || caps_mux_codec(m, enc->id) != CAPS_NO)
        return TRUE;   /* no encoder chosen: not the container's fault */
    g_snprintf(why, size, "cannot store stream #%d as %s (%s)", row->ms->index,
               avcodec_get_name(enc->id), enc->name);
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* encoder options, kept per encoder so switching back restores them         */

typedef struct OptBox {
    AVDictionary *dict;
} OptBox;

static void opt_box_free(gpointer data)
{
    OptBox *b = data;

    av_dict_free(&b->dict);
    g_free(b);
}

static OptBox *current_box(StreamRow *row, gboolean create)
{
    const CapsEncoder *enc = current_encoder(row);
    OptBox *b;

    if (!enc)
        return NULL;
    if (!(b = g_hash_table_lookup(row->options, enc->name)) && create) {
        b = g_new0(OptBox, 1);
        g_hash_table_insert(row->options, g_strdup(enc->name), b);
    }
    return b;
}

static void update_options_label(StreamRow *row)
{
    OptBox *b = current_box(row, FALSE);
    int n = b ? av_dict_count(b->dict) : 0;
    char label[32];

    if (n)
        g_snprintf(label, sizeof(label), "Options (%d)", n);
    else
        g_strlcpy(label, "Options", sizeof(label));
    gtk_button_set_label(GTK_BUTTON(row->options_btn), label);
    gtk_widget_set_sensitive(row->options_btn, current_encoder(row) != NULL);
}

static void on_options_edited(void *data)
{
    StreamRow *row = data;

    update_options_label(row);
    if (row->changed)
        row->changed(row->user);
}

GtkWindow *stream_row_edit_options(StreamRow *row, OptionEditor **editor)
{
    const CapsEncoder *enc = current_encoder(row);
    OptBox *b = current_box(row, TRUE);
    OptionTarget t = { 0 };
    GtkRoot *root;

    if (!enc || !b)
        return NULL;
    root = gtk_widget_get_root(row->box);
    t.codec = enc->codec;
    return option_dialog_show(GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, t, &b->dict,
                              on_options_edited, row, editor);
}

static void on_options_clicked(GtkButton *b, gpointer data)
{
    stream_row_edit_options(data, NULL);
}

const AVDictionary *stream_row_options(const StreamRow *row)
{
    OptBox *b = current_box((StreamRow *)row, FALSE);
    return b ? b->dict : NULL;
}

GHashTable *stream_row_take_options(StreamRow *row)
{
    GHashTable *h = row->options;

    row->options = NULL;
    return h;
}

/* ------------------------------------------------------------------------- */

static void on_action_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
    StreamRow *row = data;
    gboolean transcode = stream_row_action(row) == JOB_TRANSCODE;

    gtk_widget_set_visible(row->encoder_dd, transcode);
    gtk_widget_set_visible(row->options_btn, transcode);
    update_options_label(row);
    if (row->changed)
        row->changed(row->user);
}

static void on_encoder_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
    StreamRow *row = data;

    update_options_label(row);
    if (row->changed)
        row->changed(row->user);
}

/* Actions: always the three; greyed when the container cannot take them. */
static guint fill_actions(StreamRow *row, const CapsMuxer *mux, const CapsStreamActions *act,
                          int prev_action, CapsDefault def)
{
    static const JobAction order[] = { JOB_COPY, JOB_TRANSCODE, JOB_DROP };
    guint sel = 0;
    int nb_ok = 0;

    for (int i = 0; i < act->nb_encoders; i++)
        nb_ok += act->encoders[i].compat != CAPS_NO;

    for (guint i = 0; i < G_N_ELEMENTS(order); i++) {
        JobAction a = order[i];
        ConvChoiceItem *it;
        char reason[256] = "";
        const char *label = a == JOB_COPY ? (act->copy == CAPS_MAYBE ? "Copy (?)" : "Copy")
                          : a == JOB_TRANSCODE ? "Convert" : "Drop";
        gboolean ok = TRUE;

        if (a == JOB_COPY && act->copy == CAPS_NO) {
            ok = FALSE;
            if (row->ms->type == AVMEDIA_TYPE_ATTACHMENT)
                g_strlcpy(reason, "Only Matroska stores attachments", sizeof(reason));
            else
                g_snprintf(reason, sizeof(reason), "%s cannot store %s as is: convert it or choose "
                           "another container", mux->key, row->ms->codec_name[0] ? row->ms->codec_name : "this codec");
        } else if (a == JOB_TRANSCODE && (!act->can_transcode || !nb_ok)) {
            ok = FALSE;
            g_snprintf(reason, sizeof(reason), "%s", act->note ? act->note : "cannot be converted");
            reason[0] = g_ascii_toupper(reason[0]);
        }

        it = conv_choice_item_new(label, GINT_TO_POINTER(a + 1));
        conv_choice_item_set_state(it, ok, ok ? NULL : reason);
        g_list_store_append(row->actions, it);
        g_object_unref(it);

        if ((prev_action >= 0 && (int)a == prev_action) ||
            (prev_action < 0 && ((def == CAPS_DEFAULT_COPY && a == JOB_COPY) ||
                                 (def == CAPS_DEFAULT_TRANSCODE && a == JOB_TRANSCODE) ||
                                 (def == CAPS_DEFAULT_DROP && a == JOB_DROP))))
            sel = i;
    }
    return sel;
}

/* Encoders: every one of the stream's type, possible ones first. */
static guint fill_encoders(StreamRow *row, const CapsMuxer *mux, const CapsStreamActions *act,
                           const char *prev_encoder, const CapsEncoder *def_enc)
{
    guint sel = GTK_INVALID_LIST_POSITION, def_pos = 0;

    for (int i = 0; i < act->nb_encoders; i++) {
        const CapsEncChoice *c = &act->encoders[i];
        ConvChoiceItem *it;
        char label[128], reason[256] = "";

        g_snprintf(label, sizeof(label), "%s%s%s", c->enc->name, c->enc->is_hardware ? " (hardware)" : "",
                   c->compat == CAPS_MAYBE ? " (?)" : "");
        if (c->reason == CAPS_REASON_SUBTITLE_KIND)
            g_snprintf(reason, sizeof(reason), "%s subtitles cannot become %s subtitles",
                       row->ms->is_text_sub ? "Text" : "Bitmap", row->ms->is_text_sub ? "bitmap" : "text");
        else if (c->compat == CAPS_NO)
            g_snprintf(reason, sizeof(reason), "%s cannot store %s", mux->key, avcodec_get_name(c->enc->id));

        it = conv_choice_item_new(label, c->enc);
        conv_choice_item_set_state(it, c->compat != CAPS_NO, reason);
        g_list_store_append(row->encoders, it);
        g_object_unref(it);

        if (prev_encoder && !strcmp(prev_encoder, c->enc->name))
            sel = i;
        if (c->enc == def_enc)
            def_pos = i;
    }
    return sel != GTK_INVALID_LIST_POSITION ? sel : def_pos;
}

StreamRow *stream_row_new(const Caps *caps, const CapsMuxer *mux, const MediaStream *ms,
                          GHashTable *options,
                          int prev_action, const char *prev_encoder,
                          StreamRowChanged changed, void *user)
{
    StreamRow *row = g_new0(StreamRow, 1);
    CapsStreamActions act = { 0 };
    const CapsEncoder *def_enc = NULL;
    CapsDefault def = CAPS_DEFAULT_DROP;
    GtkWidget *icon, *index, *details;
    guint sel_action, sel_encoder;
    char *desc, *markup;

    row->ms       = ms;
    row->options  = options ? options
                            : g_hash_table_new_full(g_str_hash, g_str_equal, g_free, opt_box_free);
    row->actions  = g_list_store_new(CONV_TYPE_CHOICE_ITEM);
    row->encoders = g_list_store_new(CONV_TYPE_CHOICE_ITEM);

    if (caps_stream_actions(caps, mux, ms, CAPS_ACTIONS_INCOMPATIBLE, &act) >= 0)
        def = caps_default_action(&act, &def_enc);
    sel_action  = fill_actions(row, mux, &act, prev_action, def);
    sel_encoder = fill_encoders(row, mux, &act, prev_encoder, def_enc);
    caps_stream_actions_free(&act);

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

    row->action_dd = conv_choice_dropdown_new(row->actions, FALSE);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(row->action_dd), sel_action);
    gtk_widget_set_size_request(row->action_dd, 120, -1);
    gtk_box_append(GTK_BOX(row->box), row->action_dd);

    row->encoder_dd = conv_choice_dropdown_new(row->encoders, TRUE);
    if (g_list_model_get_n_items(G_LIST_MODEL(row->encoders)))
        gtk_drop_down_set_selected(GTK_DROP_DOWN(row->encoder_dd), sel_encoder);
    gtk_widget_set_size_request(row->encoder_dd, 210, -1);
    gtk_widget_set_tooltip_text(row->encoder_dd, "Encoder (type to search)");
    gtk_box_append(GTK_BOX(row->box), row->encoder_dd);

    row->options_btn = gtk_button_new_with_label("Options");
    gtk_widget_set_tooltip_text(row->options_btn, "Encoder options");
    g_signal_connect(row->options_btn, "clicked", G_CALLBACK(on_options_clicked), row);
    gtk_box_append(GTK_BOX(row->box), row->options_btn);

    gtk_widget_set_visible(row->encoder_dd, stream_row_action(row) == JOB_TRANSCODE);
    gtk_widget_set_visible(row->options_btn, stream_row_action(row) == JOB_TRANSCODE);
    update_options_label(row);

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
    g_signal_handlers_disconnect_by_data(row->options_btn, row);
    if (row->options)
        g_hash_table_unref(row->options);
    g_object_unref(row->box);
    g_object_unref(row->actions);
    g_object_unref(row->encoders);
    g_free(row);
}

gboolean stream_row_select(StreamRow *row, JobAction action, const char *encoder)
{
    guint a = conv_choice_find(G_LIST_MODEL(row->actions), GINT_TO_POINTER(action + 1));

    if (a == GTK_INVALID_LIST_POSITION)
        return FALSE;
    if (action == JOB_TRANSCODE) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(row->encoders)), e = GTK_INVALID_LIST_POSITION;
        for (guint i = 0; encoder && i < n; i++) {
            ConvChoiceItem *it = g_list_model_get_item(G_LIST_MODEL(row->encoders), i);
            const CapsEncoder *enc = conv_choice_item_get_data(it);
            if (!strcmp(enc->name, encoder))
                e = i;
            g_object_unref(it);
        }
        if (e == GTK_INVALID_LIST_POSITION)
            return FALSE;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(row->encoder_dd), e);
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(row->action_dd), a);
    return TRUE;
}

GtkWidget *stream_row_action_dropdown(const StreamRow *row)
{
    return row->action_dd;
}

GtkWidget *stream_row_encoder_dropdown(const StreamRow *row)
{
    return row->encoder_dd;
}

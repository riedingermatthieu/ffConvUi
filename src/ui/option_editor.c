/*
 * option_editor.c - AVOption schema -> GTK widgets.
 *
 * One row per option: name, help, a widget chosen from the option's type
 * (spin button, dropdown, flag checkboxes or text entry) and a reset button.
 * Values are written to the caller's dictionary only when changed, and
 * checked on a scratch encoder/muxer/filter context so bad values are
 * flagged at once with FFmpeg's reason.
 */
#include "option_editor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavfilter/avfilter.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>

#include "avopt_schema.h"

enum { SEC_COMMON, SEC_PRIVATE, SEC_GENERAL, NB_SECTIONS };

typedef enum TargetKind { T_CODEC, T_MUXER, T_FILTER } TargetKind;

/* generic options worth showing first */
static const char *const common_video[] = {
    "b", "maxrate", "minrate", "bufsize", "g", "bf", "profile", "level", "aspect", "threads", NULL
};
static const char *const common_audio[] = {
    "b", "ar", "ch_layout", "cutoff", "threads", NULL
};

typedef struct OptRow {
    OptionEditor    *ed;
    const OptSchema *s;          /* NULL for the Quality row */
    int              section;
    GtkWidget       *box;
    GtkWidget       *value;      /* spin button, dropdown, entry or flow box */
    GtkWidget       *reset;
    GtkWidget      **checks;     /* flags */
    int              nb_checks;
    char            *default_flags;
    GtkWidget       *enable;     /* Quality: on/off */
    /* the encoder's/muxer's actual default, read from the scratch context
     * (libx264 overrides FFmpeg's generic defaults: b, g, bf...) */
    int64_t          def_i64;
    double           def_dbl;
    char             def_str[128];
} OptRow;

struct OptionEditor {
    OptSchemaList   lists[2];    /* [0] private (with child classes), [1] generic */
    AVDictionary  **values;
    void           *check_obj;   /* AVCodecContext*, AVFormatContext* or AVFilterContext* */
    AVFilterGraph  *check_graph; /* owns the scratch AVFilterContext */
    TargetKind      kind;
    int             is_codec;
    GtkWindow      *win;
    GtkWidget      *root, *search;
    GtkWidget      *section_title[NB_SECTIONS], *section_frame[NB_SECTIONS], *section_list[NB_SECTIONS];
    GPtrArray      *rows;
    OptRow         *quality;
    char           *filter;      /* casefolded search text */
    OptionsChanged  changed;
    void           *user;
    int             suppress;    /* programmatic widget changes: ignore callbacks */
    int             loading;     /* applying initial values: no change notifications */
};

/* ------------------------------------------------------------------------- */
/* values                                                                    */

static void notify(OptionEditor *ed)
{
    if (!ed->loading && ed->changed)
        ed->changed(ed->user);
}

/* The widget actually used: integers with an effectively unlimited range
 * (bitrates, buffer sizes, GOP...) get a text entry, which accepts FFmpeg's
 * suffixes ("96k", "2M") and is checked like any other value. */
static OptWidget row_widget(const OptRow *r)
{
    if (r->s->widget == OPT_W_INT && r->s->max - r->s->min > 1e7)
        return OPT_W_STRING;
    return r->s->widget;
}

/* The object that receives option `name`, as FFmpeg applies a dictionary:
 * avcodec_open2() and filters set private options first,
 * avformat_write_header() generic ones first. *children is set when child
 * classes must be searched. */
static void *target_obj(OptionEditor *ed, const char *name, int *children)
{
    void *obj = ed->check_obj, *priv;
    int in_priv, in_gen;

    *children = 0;
    if (!obj)
        return NULL;
    priv    = ed->kind == T_CODEC  ? ((AVCodecContext *)obj)->priv_data
            : ed->kind == T_FILTER ? ((AVFilterContext *)obj)->priv
                                   : ((AVFormatContext *)obj)->priv_data;
    in_priv = priv && av_opt_find(priv, name, NULL, 0, AV_OPT_SEARCH_CHILDREN);
    in_gen  = av_opt_find(obj, name, NULL, 0, 0) != NULL;
    if (in_priv && (ed->kind != T_MUXER || !in_gen)) {
        *children = 1;
        return priv;
    }
    return in_gen ? obj : NULL;
}

/* Check a value on the scratch context; flag the widget if FFmpeg refuses it. */
static void check_value(OptRow *r, const char *value)
{
    int children, ret = 0;
    void *obj = r->s ? target_obj(r->ed, r->s->name, &children) : NULL;

    if (value && obj)
        ret = av_opt_set(obj, r->s->name, value, children ? AV_OPT_SEARCH_CHILDREN : 0);
    if (ret < 0) {
        char msg[256];
        g_snprintf(msg, sizeof(msg), "FFmpeg refuses “%s”: %s", value, av_err2str(ret));
        gtk_widget_add_css_class(r->value, "error");
        gtk_widget_set_tooltip_text(r->value, msg);
    } else {
        gtk_widget_remove_css_class(r->value, "error");
        gtk_widget_set_tooltip_text(r->value, NULL);
    }
}

static void store(OptRow *r, const char *value)
{
    av_dict_set(r->ed->values, r->s->name, value, 0);
    gtk_widget_set_visible(r->reset, value != NULL);
    check_value(r, value);
    notify(r->ed);
}

static double clamp_range(double v)
{
    return v < -1e12 ? -1e12 : v > 1e12 ? 1e12 : v;
}

/* Read the row's default from the scratch context (falls back to the
 * AVOption's own default). Must run before any value is checked on it. */
static void read_default(OptRow *r)
{
    const OptSchema *s = r->s;
    int children;
    void *obj = target_obj(r->ed, s->name, &children);
    int flags = children ? AV_OPT_SEARCH_CHILDREN : 0;
    uint8_t *str = NULL;

    r->def_i64 = s->opt->default_val.i64;
    r->def_dbl = s->opt->default_val.dbl;
    av_strlcpy(r->def_str, s->default_str, sizeof(r->def_str));
    if (!obj || s->is_array)
        return;

    switch (s->type) {
    case AV_OPT_TYPE_INT:
    case AV_OPT_TYPE_INT64:
    case AV_OPT_TYPE_UINT:
    case AV_OPT_TYPE_UINT64:
    case AV_OPT_TYPE_FLAGS:
    case AV_OPT_TYPE_BOOL:
        if (av_opt_get_int(obj, s->name, flags, &r->def_i64) < 0)
            return;
        if (s->type == AV_OPT_TYPE_BOOL) {
            av_strlcpy(r->def_str, r->def_i64 < 0 ? "auto" : r->def_i64 ? "on" : "off", sizeof(r->def_str));
        } else if (s->type != AV_OPT_TYPE_FLAGS) {
            g_snprintf(r->def_str, sizeof(r->def_str), "%" G_GINT64_FORMAT, r->def_i64);
            for (int i = 0; i < s->nb_choices; i++)
                if (s->choices[i].value == r->def_i64) {
                    av_strlcpy(r->def_str, s->choices[i].name, sizeof(r->def_str));
                    break;
                }
        }
        break;
    case AV_OPT_TYPE_FLOAT:
    case AV_OPT_TYPE_DOUBLE:
        if (av_opt_get_double(obj, s->name, flags, &r->def_dbl) >= 0)
            g_ascii_formatd(r->def_str, sizeof(r->def_str), "%g", r->def_dbl);
        break;
    default:
        if (av_opt_get(obj, s->name, flags, &str) >= 0) {
            av_strlcpy(r->def_str, str ? (const char *)str : "", sizeof(r->def_str));
            av_free(str);
        }
        if (s->type == AV_OPT_TYPE_CHLAYOUT && !strcmp(r->def_str, "0 channels"))
            r->def_str[0] = '\0';   /* an empty layout: "not set" */
        break;
    }
}

static double numeric_default(const OptRow *r)
{
    const OptSchema *s = r->s;
    double d = s->type == AV_OPT_TYPE_FLOAT || s->type == AV_OPT_TYPE_DOUBLE ? r->def_dbl : (double)r->def_i64;
    return FFMIN(FFMAX(d, clamp_range(s->min)), clamp_range(s->max));
}

static char *flags_string(OptRow *r)
{
    GString *g = g_string_new(NULL);

    for (int i = 0; i < r->nb_checks; i++) {
        if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(r->checks[i])))
            continue;
        if (g->len)
            g_string_append_c(g, '+');
        g_string_append(g, r->s->choices[i].name);
    }
    if (!g->len)
        g_string_append_c(g, '0');
    return g_string_free(g, FALSE);
}

/* What the widget currently says, as FFmpeg option text; NULL = default. */
static char *widget_value(OptRow *r)
{
    const OptSchema *s = r->s;
    char buf[64];
    guint sel;

    switch (row_widget(r)) {
    case OPT_W_INT:
        g_ascii_formatd(buf, sizeof(buf), "%.0f", gtk_spin_button_get_value(GTK_SPIN_BUTTON(r->value)));
        return g_strdup(buf);
    case OPT_W_FLOAT:
        g_ascii_formatd(buf, sizeof(buf), "%.6g", gtk_spin_button_get_value(GTK_SPIN_BUTTON(r->value)));
        return g_strdup(buf);
    case OPT_W_BOOL:
        sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(r->value));
        return sel == 1 ? g_strdup("1") : sel == 2 ? g_strdup("0") : NULL;
    case OPT_W_ENUM:
        sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(r->value));
        return sel >= 1 && sel <= (guint)s->nb_choices ? g_strdup(s->choices[sel - 1].name) : NULL;
    case OPT_W_FLAGS: {
        char *v = flags_string(r);
        if (!strcmp(v, r->default_flags)) {
            g_free(v);
            return NULL;
        }
        return v;
    }
    default: {
        const char *t = gtk_editable_get_text(GTK_EDITABLE(r->value));
        return *t ? g_strdup(t) : NULL;
    }
    }
}

static void widget_changed(OptRow *r)
{
    char *v;

    if (r->ed->suppress)
        return;
    v = widget_value(r);
    store(r, v);
    g_free(v);
}

static void on_spin(GtkSpinButton *b, gpointer data)              { widget_changed(data); }
static void on_dropdown(GObject *o, GParamSpec *p, gpointer data) { widget_changed(data); }
static void on_entry(GtkEditable *e, gpointer data)               { widget_changed(data); }
static void on_check(GtkCheckButton *c, gpointer data)            { widget_changed(data); }

/* ------------------------------------------------------------------------- */
/* Quality (the ffmpeg CLI's -q): global_quality + the qscale flag           */

static OptRow *find_row(OptionEditor *ed, const char *name)
{
    for (guint i = 0; i < ed->rows->len; i++) {
        OptRow *r = g_ptr_array_index(ed->rows, i);
        if (r->s && !strcmp(r->s->name, name))
            return r;
    }
    return NULL;
}

static void set_qscale_flag(OptionEditor *ed, gboolean on)
{
    OptRow *flags = find_row(ed, "flags");

    if (flags) {
        for (int i = 0; i < flags->nb_checks; i++) {
            if (!strcmp(flags->s->choices[i].name, "qscale")) {
                ed->suppress = 1;
                gtk_check_button_set_active(GTK_CHECK_BUTTON(flags->checks[i]), on);
                ed->suppress = 0;
                widget_changed(flags);
                return;
            }
        }
    }
    av_dict_set(ed->values, "flags", on ? "+qscale" : NULL, 0);   /* no flags row */
}

static void quality_changed(OptRow *r)
{
    OptionEditor *ed = r->ed;
    gboolean on = gtk_check_button_get_active(GTK_CHECK_BUTTON(r->enable));
    char buf[32];

    if (ed->suppress)
        return;
    gtk_widget_set_sensitive(r->value, on);
    if (on) {
        g_snprintf(buf, sizeof(buf), "%ld",
                   lround(gtk_spin_button_get_value(GTK_SPIN_BUTTON(r->value)) * FF_QP2LAMBDA));
        av_dict_set(ed->values, "global_quality", buf, 0);
    } else {
        av_dict_set(ed->values, "global_quality", NULL, 0);
    }
    gtk_widget_set_visible(r->reset, on);
    set_qscale_flag(ed, on);
    notify(ed);
}

static void on_quality_spin(GtkSpinButton *b, gpointer data)   { quality_changed(data); }
static void on_quality_check(GtkCheckButton *c, gpointer data) { quality_changed(data); }

/* ------------------------------------------------------------------------- */
/* rows                                                                      */

static void reset_row(OptRow *r)
{
    OptionEditor *ed = r->ed;

    ed->suppress = 1;
    if (!r->s) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(r->enable), FALSE);
        ed->suppress = 0;
        quality_changed(r);
        return;
    }
    switch (row_widget(r)) {
    case OPT_W_INT:
    case OPT_W_FLOAT:
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->value), numeric_default(r));
        break;
    case OPT_W_BOOL:
    case OPT_W_ENUM:
        gtk_drop_down_set_selected(GTK_DROP_DOWN(r->value), 0);
        break;
    case OPT_W_FLAGS:
        for (int i = 0; i < r->nb_checks; i++) {
            int64_t bit = r->s->choices[i].value;
            gtk_check_button_set_active(GTK_CHECK_BUTTON(r->checks[i]),
                                        bit && (r->def_i64 & bit) == bit);
        }
        break;
    default:
        gtk_editable_set_text(GTK_EDITABLE(r->value), "");
        break;
    }
    ed->suppress = 0;
    store(r, NULL);
}

static void on_reset(GtkButton *b, gpointer data)
{
    reset_row(data);
}

static GtkWidget *make_value_widget(OptRow *r)
{
    const OptSchema *s = r->s;
    OptionEditor *ed = r->ed;
    GtkStringList *model;
    GtkWidget *w;
    char label[160];

    switch (row_widget(r)) {
    case OPT_W_INT:
    case OPT_W_FLOAT: {
        double lo = clamp_range(s->min), hi = clamp_range(s->max);
        int is_float = s->widget == OPT_W_FLOAT;
        if (lo > hi) {
            double t = lo; lo = hi; hi = t;
        }
        w = gtk_spin_button_new_with_range(lo, hi, is_float ? 0.1 : 1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(w), is_float ? 3 : 0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), numeric_default(r));
        gtk_editable_set_width_chars(GTK_EDITABLE(w), 10);
        g_signal_connect(w, "value-changed", G_CALLBACK(on_spin), r);
        return w;
    }
    case OPT_W_BOOL:
        g_snprintf(label, sizeof(label), "Default (%s)", r->def_str);
        model = gtk_string_list_new((const char *[]){ label, "On", "Off", NULL });
        w = gtk_drop_down_new(G_LIST_MODEL(model), NULL);
        g_signal_connect(w, "notify::selected", G_CALLBACK(on_dropdown), r);
        return w;
    case OPT_W_ENUM:
        /* the engine sets threads=auto when the job does not */
        g_snprintf(label, sizeof(label), "Default (%s)",
                   ed->is_codec && !strcmp(s->name, "threads") ? "auto"
                   : r->def_str[0] ? r->def_str : "none");
        model = gtk_string_list_new(NULL);
        gtk_string_list_append(model, label);
        for (int i = 0; i < s->nb_choices; i++)
            gtk_string_list_append(model, s->choices[i].name);
        w = gtk_drop_down_new(G_LIST_MODEL(model),
                              gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string"));
        if (s->nb_choices > 12)
            gtk_drop_down_set_enable_search(GTK_DROP_DOWN(w), TRUE);
        g_signal_connect(w, "notify::selected", G_CALLBACK(on_dropdown), r);
        return w;
    case OPT_W_FLAGS:
        w = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(w), GTK_SELECTION_NONE);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(w), 4);
        gtk_widget_set_size_request(w, 360, -1);
        r->checks = g_new0(GtkWidget *, s->nb_choices ? s->nb_choices : 1);
        for (int i = 0; i < s->nb_choices; i++) {
            int64_t bit = s->choices[i].value;
            GtkWidget *c = gtk_check_button_new_with_label(s->choices[i].name);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(c), bit && (r->def_i64 & bit) == bit);
            if (s->choices[i].help)
                gtk_widget_set_tooltip_text(c, s->choices[i].help);
            g_signal_connect(c, "toggled", G_CALLBACK(on_check), r);
            gtk_flow_box_append(GTK_FLOW_BOX(w), c);
            r->checks[r->nb_checks++] = c;
        }
        r->value = w;
        r->default_flags = flags_string(r);
        return w;
    default:
        w = gtk_entry_new();
        g_snprintf(label, sizeof(label), "%s", r->def_str[0] ? r->def_str : "not set");
        gtk_entry_set_placeholder_text(GTK_ENTRY(w), label);
        gtk_editable_set_width_chars(GTK_EDITABLE(w), 18);
        g_signal_connect(w, "changed", G_CALLBACK(on_entry), r);
        return w;
    }
}

static GtkWidget *row_text(const char *name, const char *detail, const char *help)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *l;
    char *markup = detail ? g_markup_printf_escaped("<b>%s</b>  <small>%s</small>", name, detail)
                          : g_markup_printf_escaped("<b>%s</b>", name);

    l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(v), l);
    g_free(markup);
    if (help && *help) {
        l = gtk_label_new(help);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
        gtk_widget_add_css_class(l, "dim-label");
        gtk_box_append(GTK_BOX(v), l);
    }
    gtk_widget_set_hexpand(v, TRUE);
    return v;
}

static void append_row(OptionEditor *ed, OptRow *r, GtkWidget *text)
{
    GtkWidget *lbrow;

    r->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(r->box, 6);
    gtk_widget_set_margin_bottom(r->box, 6);
    gtk_widget_set_margin_start(r->box, 10);
    gtk_widget_set_margin_end(r->box, 10);
    gtk_box_append(GTK_BOX(r->box), text);
    gtk_widget_set_valign(r->value, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(r->box), r->enable ? r->enable : r->value);
    if (r->enable)
        gtk_box_append(GTK_BOX(r->box), r->value);

    r->reset = gtk_button_new_from_icon_name("edit-undo-symbolic");
    gtk_widget_set_tooltip_text(r->reset, "Back to the default");
    gtk_widget_set_valign(r->reset, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(r->reset, "flat");
    gtk_widget_set_visible(r->reset, FALSE);
    g_signal_connect(r->reset, "clicked", G_CALLBACK(on_reset), r);
    gtk_box_append(GTK_BOX(r->box), r->reset);

    gtk_list_box_append(GTK_LIST_BOX(ed->section_list[r->section]), r->box);
    lbrow = gtk_widget_get_parent(r->box);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(lbrow), FALSE);
    g_object_set_data(G_OBJECT(lbrow), "optrow", r);
    g_ptr_array_add(ed->rows, r);
}

static void add_option_row(OptionEditor *ed, const OptSchema *s, int section)
{
    OptRow *r = g_new0(OptRow, 1);
    char detail[64];

    r->ed      = ed;
    r->s       = s;
    r->section = section;
    read_default(r);
    if (!r->value)
        r->value = make_value_widget(r);
    g_snprintf(detail, sizeof(detail), "%s%s", s->is_array ? "list of " : "", optschema_type_name(s->type));
    append_row(ed, r, row_text(s->name, detail, s->help));
}

static void add_quality_row(OptionEditor *ed)
{
    OptRow *r = g_new0(OptRow, 1);

    r->ed      = ed;
    r->section = SEC_COMMON;
    r->enable  = gtk_check_button_new();
    r->value   = gtk_spin_button_new_with_range(0, 69, 0.5);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(r->value), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->value), 4);
    gtk_widget_set_sensitive(r->value, FALSE);
    gtk_widget_set_valign(r->enable, GTK_ALIGN_CENTER);
    g_signal_connect(r->enable, "toggled", G_CALLBACK(on_quality_check), r);
    g_signal_connect(r->value, "value-changed", G_CALLBACK(on_quality_spin), r);
    append_row(ed, r, row_text("Quality", "the ffmpeg CLI's -q",
        "Constant quality for encoders that use qscale (sets global_quality and the qscale flag). "
        "The scale depends on the encoder: libvorbis 0–10 and aac 0.1–2 (higher is better), "
        "libmp3lame 0–9 and mpeg4/mjpeg 1–31 (lower is better). Encoders with their own quality "
        "option, such as x264's crf, ignore it."));
    ed->quality = r;
}

/* ------------------------------------------------------------------------- */
/* search                                                                    */

static gboolean row_matches(OptionEditor *ed, const OptRow *r)
{
    char *hay;
    gboolean ok;

    if (!ed->filter || !*ed->filter)
        return TRUE;
    hay = g_utf8_casefold(r->s ? r->s->name : "quality q", -1);
    ok = strstr(hay, ed->filter) != NULL;
    g_free(hay);
    if (!ok && r->s && r->s->help[0]) {
        hay = g_utf8_casefold(r->s->help, -1);
        ok = strstr(hay, ed->filter) != NULL;
        g_free(hay);
    }
    return ok;
}

static gboolean filter_func(GtkListBoxRow *row, gpointer data)
{
    OptRow *r = g_object_get_data(G_OBJECT(row), "optrow");
    return !r || row_matches(data, r);
}

void option_editor_search(OptionEditor *ed, const char *text)
{
    int visible[NB_SECTIONS] = { 0 };

    g_free(ed->filter);
    ed->filter = text && *text ? g_utf8_casefold(text, -1) : NULL;
    for (guint i = 0; i < ed->rows->len; i++) {
        OptRow *r = g_ptr_array_index(ed->rows, i);
        visible[r->section] += row_matches(ed, r);
    }
    for (int s = 0; s < NB_SECTIONS; s++) {
        gtk_list_box_invalidate_filter(GTK_LIST_BOX(ed->section_list[s]));
        gtk_widget_set_visible(ed->section_title[s], visible[s] > 0);
        gtk_widget_set_visible(ed->section_frame[s], visible[s] > 0);
    }
}

static void on_search(GtkSearchEntry *e, gpointer data)
{
    option_editor_search(data, gtk_editable_get_text(GTK_EDITABLE(e)));
}

/* ------------------------------------------------------------------------- */
/* setting values from text (initial values, test hooks)                     */

gboolean option_editor_set_text(OptionEditor *ed, const char *name, const char *text)
{
    OptRow *r;
    char *end;
    double d;

    if (!strcmp(name, "q") && ed->quality) {
        d = g_ascii_strtod(text, &end);
        if (end == text)
            return FALSE;
        r = ed->quality;
        ed->suppress = 1;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->value), d);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(r->enable), TRUE);
        ed->suppress = 0;
        quality_changed(r);
        return TRUE;
    }
    if (!(r = find_row(ed, name))) {
        /* an alias ("w" for crop's out_w) is shown as the option it writes */
        for (int l = 0; l < 2 && !r; l++)
            for (int i = 0; i < ed->lists[l].nb_opts && !r; i++)
                if (ed->lists[l].opts[i].alias_of && !strcmp(ed->lists[l].opts[i].name, name))
                    r = find_row(ed, ed->lists[l].opts[i].alias_of);
        if (!r)
            return FALSE;
        if (ed->values && strcmp(r->s->name, name))
            av_dict_set(ed->values, name, NULL, 0);   /* one key for the field */
    }

    ed->suppress = 1;
    switch (row_widget(r)) {
    case OPT_W_INT:
    case OPT_W_FLOAT:
        d = g_ascii_strtod(text, &end);
        if (end == text || *end) {
            ed->suppress = 0;
            return FALSE;
        }
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->value), d);
        break;
    case OPT_W_BOOL: {
        int on = !g_ascii_strcasecmp(text, "1") || !g_ascii_strcasecmp(text, "true") ||
                 !g_ascii_strcasecmp(text, "on") || !g_ascii_strcasecmp(text, "yes");
        gtk_drop_down_set_selected(GTK_DROP_DOWN(r->value), on ? 1 : 2);
        break;
    }
    case OPT_W_ENUM: {
        int found = -1;
        for (int i = 0; i < r->s->nb_choices && found < 0; i++)
            if (!strcmp(r->s->choices[i].name, text))
                found = i;
        if (found < 0) {
            ed->suppress = 0;
            return FALSE;
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(r->value), found + 1);
        break;
    }
    case OPT_W_FLAGS: {
        /* "a+b": exactly these; "+a" / "-b": relative to the current state */
        char **tok = g_strsplit_set(text, "+-", -1);
        int relative = text[0] == '+' || text[0] == '-';
        const char *p = text;
        if (!relative)
            for (int i = 0; i < r->nb_checks; i++)
                gtk_check_button_set_active(GTK_CHECK_BUTTON(r->checks[i]), FALSE);
        for (char **t = tok; *t; t++) {
            int on;
            if (!**t)
                continue;
            p = strstr(p, *t);
            on = !(p > text && p[-1] == '-');
            for (int i = 0; i < r->nb_checks; i++)
                if (!strcmp(r->s->choices[i].name, *t))
                    gtk_check_button_set_active(GTK_CHECK_BUTTON(r->checks[i]), on);
        }
        g_strfreev(tok);
        break;
    }
    default:
        gtk_editable_set_text(GTK_EDITABLE(r->value), text);
        break;
    }
    ed->suppress = 0;
    widget_changed(r);   /* store what the widget now says */
    return TRUE;
}

/* ------------------------------------------------------------------------- */

static void add_section(OptionEditor *ed, GtkWidget *box, int s, const char *title)
{
    ed->section_title[s] = gtk_label_new(NULL);
    {
        char *markup = g_markup_printf_escaped("<b>%s</b>", title);
        gtk_label_set_markup(GTK_LABEL(ed->section_title[s]), markup);
        g_free(markup);
    }
    gtk_label_set_xalign(GTK_LABEL(ed->section_title[s]), 0);
    gtk_widget_set_margin_top(ed->section_title[s], s ? 14 : 0);
    gtk_box_append(GTK_BOX(box), ed->section_title[s]);

    ed->section_list[s] = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ed->section_list[s]), GTK_SELECTION_NONE);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(ed->section_list[s]), filter_func, ed, NULL);
    ed->section_frame[s] = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(ed->section_frame[s]), ed->section_list[s]);
    gtk_box_append(GTK_BOX(box), ed->section_frame[s]);
}

static int schema_has(const OptSchemaList *l, const char *name)
{
    for (int i = 0; i < l->nb_opts; i++)
        if (!strcmp(l->opts[i].name, name))
            return 1;
    return 0;
}

static int in_list(const char *const *list, const char *name)
{
    for (; list && *list; list++)
        if (!strcmp(*list, name))
            return 1;
    return 0;
}

static OptionEditor *editor_new(OptionTarget t, AVDictionary **values, OptionsChanged changed, void *user)
{
    OptionEditor *ed = g_new0(OptionEditor, 1);
    const char *const *common = NULL;
    GtkWidget *scroll, *box;
    const AVDictionaryEntry *e = NULL;
    char title[160];
    int media_flag = 0;

    ed->values   = values;
    ed->changed  = changed;
    ed->user     = user;
    ed->rows     = g_ptr_array_new();
    ed->is_codec = t.codec != NULL;
    ed->kind     = t.codec ? T_CODEC : t.filter ? T_FILTER : T_MUXER;

    if (t.filter) {
        /* the filter's own options, and the timeline (`enable`) when it has one */
        optschema_from_class(t.filter->priv_class, 0, 1, &ed->lists[0]);
        if (t.filter->flags & AVFILTER_FLAG_SUPPORT_TIMELINE)
            optschema_from_class(avfilter_get_class(), AV_OPT_FLAG_FILTERING_PARAM, 0, &ed->lists[1]);
        if ((ed->check_graph = avfilter_graph_alloc()))
            ed->check_obj = avfilter_graph_alloc_filter(ed->check_graph, t.filter, "check");
        g_snprintf(title, sizeof(title), "%s options", t.filter->name);
    } else if (t.codec) {
        media_flag = t.codec->type == AVMEDIA_TYPE_VIDEO    ? AV_OPT_FLAG_VIDEO_PARAM
                   : t.codec->type == AVMEDIA_TYPE_AUDIO    ? AV_OPT_FLAG_AUDIO_PARAM
                   : t.codec->type == AVMEDIA_TYPE_SUBTITLE ? AV_OPT_FLAG_SUBTITLE_PARAM : 0;
        common = t.codec->type == AVMEDIA_TYPE_VIDEO ? common_video
               : t.codec->type == AVMEDIA_TYPE_AUDIO ? common_audio : NULL;
        optschema_from_class(t.codec->priv_class, 0, 1, &ed->lists[0]);
        optschema_from_class(avcodec_get_class(), AV_OPT_FLAG_ENCODING_PARAM | media_flag, 0, &ed->lists[1]);
        ed->check_obj = avcodec_alloc_context3(t.codec);
        g_snprintf(title, sizeof(title), "%s options", t.codec->name);
    } else {
        AVFormatContext *fc = NULL;
        optschema_from_class(t.muxer->priv_class, 0, 1, &ed->lists[0]);
        optschema_from_class(avformat_get_class(), AV_OPT_FLAG_ENCODING_PARAM, 0, &ed->lists[1]);
        if (avformat_alloc_output_context2(&fc, t.muxer, NULL, NULL) >= 0)
            ed->check_obj = fc;
        g_snprintf(title, sizeof(title), "%s muxer options", t.muxer_key ? t.muxer_key : t.muxer->name);
    }

    ed->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    ed->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(ed->search), "Search options");
    g_signal_connect(ed->search, "search-changed", G_CALLBACK(on_search), ed);
    gtk_box_append(GTK_BOX(ed->root), ed->search);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_end(box, 12);   /* room for the scrollbar */
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_box_append(GTK_BOX(ed->root), scroll);

    add_section(ed, box, SEC_COMMON, "Common");
    add_section(ed, box, SEC_PRIVATE, title);
    add_section(ed, box, SEC_GENERAL, t.codec ? "Other general encoding options"
                                    : t.filter ? "Timeline" : "General muxing options");

    /* A name in both lists (x264's "profile" and the generic "profile") is one
     * dictionary key; show only the option FFmpeg gives it to: the private
     * one for encoders, the generic one for muxers (see target_obj). */
#define SHADOWED_GENERIC(name) (ed->kind != T_MUXER && schema_has(&ed->lists[0], name))
#define SHADOWED_PRIVATE(name) (ed->kind == T_MUXER && schema_has(&ed->lists[1], name))

    /* Common: curated generic options, in the list's order, then Quality */
    for (int c = 0; common && common[c]; c++)
        for (int i = 0; i < ed->lists[1].nb_opts; i++)
            if (!ed->lists[1].opts[i].alias_of && !strcmp(ed->lists[1].opts[i].name, common[c]) &&
                !SHADOWED_GENERIC(common[c]))
                add_option_row(ed, &ed->lists[1].opts[i], SEC_COMMON);
    if (t.codec && (t.codec->type == AVMEDIA_TYPE_VIDEO || t.codec->type == AVMEDIA_TYPE_AUDIO))
        add_quality_row(ed);

    for (int i = 0; i < ed->lists[0].nb_opts; i++)
        if (!ed->lists[0].opts[i].alias_of && !SHADOWED_PRIVATE(ed->lists[0].opts[i].name))
            add_option_row(ed, &ed->lists[0].opts[i], SEC_PRIVATE);
    for (int i = 0; i < ed->lists[1].nb_opts; i++) {
        const OptSchema *s = &ed->lists[1].opts[i];
        if (s->alias_of || in_list(common, s->name) || SHADOWED_GENERIC(s->name) ||
            (t.codec && !strcmp(s->name, "global_quality")) ||
            (t.filter && strcmp(s->name, "enable")))
            continue;   /* global_quality belongs to the Quality control; of the
                           generic filter options only `enable` is the user's */
        add_option_row(ed, s, SEC_GENERAL);
    }
#undef SHADOWED_GENERIC
#undef SHADOWED_PRIVATE
    option_editor_search(ed, NULL);   /* hides empty sections */

    /* show the values already set, without reporting them as changes */
    ed->loading = 1;
    {
        AVDictionary *initial = NULL;
        const AVDictionaryEntry *gq = av_dict_get(*values, "global_quality", NULL, 0);
        av_dict_copy(&initial, *values, 0);
        while ((e = av_dict_iterate(initial, e))) {
            if (!strcmp(e->key, "global_quality"))
                continue;
            option_editor_set_text(ed, e->key, e->value);
        }
        if (gq && ed->quality) {
            char q[32];
            g_ascii_formatd(q, sizeof(q), "%.1f", atof(gq->value) / FF_QP2LAMBDA);
            option_editor_set_text(ed, "q", q);
        }
        av_dict_free(&initial);
    }
    ed->loading = 0;
    return ed;
}

static void editor_free(OptionEditor *ed)
{
    for (guint i = 0; i < ed->rows->len; i++) {
        OptRow *r = g_ptr_array_index(ed->rows, i);
        g_free(r->checks);
        g_free(r->default_flags);
        g_free(r);
    }
    g_ptr_array_free(ed->rows, TRUE);
    optschema_free(&ed->lists[0]);
    optschema_free(&ed->lists[1]);
    if (ed->kind == T_CODEC) {
        AVCodecContext *c = ed->check_obj;
        avcodec_free_context(&c);
    } else if (ed->kind == T_FILTER) {
        avfilter_graph_free(&ed->check_graph);   /* frees the filter too */
    } else {
        avformat_free_context(ed->check_obj);
    }
    g_free(ed->filter);
    g_free(ed);
}

/* ------------------------------------------------------------------------- */
/* dialog                                                                    */

static void on_reset_all(GtkButton *b, gpointer data)
{
    OptionEditor *ed = data;

    for (guint i = 0; i < ed->rows->len; i++) {
        OptRow *r = g_ptr_array_index(ed->rows, i);
        if (gtk_widget_get_visible(r->reset))
            reset_row(r);
    }
}

static void on_close_clicked(GtkButton *b, gpointer data)
{
    gtk_window_close(data);
}

static void on_dialog_destroy(GtkWidget *w, gpointer data)
{
    editor_free(data);
}

GtkWindow *option_dialog_show(GtkWindow *parent, OptionTarget target, AVDictionary **values,
                              OptionsChanged changed, void *user, OptionEditor **editor)
{
    OptionEditor *ed = editor_new(target, values, changed, user);
    GtkWindow *win = GTK_WINDOW(gtk_window_new());
    GtkWidget *box, *buttons, *reset, *close;
    char title[160];

    if (target.codec)
        g_snprintf(title, sizeof(title), "%s — %s", target.codec->name,
                   target.codec->long_name ? target.codec->long_name : "");
    else if (target.filter)
        g_snprintf(title, sizeof(title), "%s — %s", target.filter->name,
                   target.filter->description ? target.filter->description : "");
    else
        g_snprintf(title, sizeof(title), "%s muxer", target.muxer_key ? target.muxer_key : target.muxer->name);
    gtk_window_set_title(win, title);
    gtk_window_set_transient_for(win, parent);
    gtk_window_set_modal(win, TRUE);
    gtk_window_set_default_size(win, 760, 680);
    ed->win = win;

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_box_append(GTK_BOX(box), ed->root);

    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    reset = gtk_button_new_with_mnemonic("_Reset all");
    gtk_widget_set_tooltip_text(reset, "Back to the defaults for every option");
    close = gtk_button_new_with_mnemonic("_Done");
    gtk_widget_add_css_class(close, "suggested-action");
    gtk_widget_set_hexpand(reset, FALSE);
    gtk_box_append(GTK_BOX(buttons), reset);
    {
        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(buttons), spacer);
    }
    gtk_box_append(GTK_BOX(buttons), close);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_child(win, box);

    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_all), ed);
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), win);
    g_signal_connect(win, "destroy", G_CALLBACK(on_dialog_destroy), ed);
    gtk_window_set_default_widget(win, close);

    gtk_window_present(win);
    if (editor)
        *editor = ed;
    return win;
}

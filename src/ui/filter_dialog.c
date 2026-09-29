/*
 * filter_dialog.c - the filter chain editor.
 *
 * Filters that cannot go in a stream's chain (several inputs, hardware
 * frames, media type changes...) are listed greyed out with the reason but
 * can still be added: nothing is refused behind the user's back, the chain
 * check then says what is wrong and validation keeps Convert disabled.
 */
#include "filter_dialog.h"

#include <string.h>

#include <libavutil/opt.h>

#include "choice_item.h"
#include "validate.h"

typedef struct FD {
    const Caps        *caps;
    const MediaStream *ms;
    FilterChain       *chain;
    GtkWindow         *win;
    GListStore        *choices;      /* ConvChoiceItem, data: const CapsFilter * */
    GtkWidget         *add_dd, *add_btn, *add_desc;
    GtkWidget         *list, *empty;
    GtkWidget         *text, *back_btn;
    GtkWidget         *status_icon, *status;
    GtkWindow         *options_win;
    int                options_entry; /* entry the option window edits */
    GPtrArray         *summaries;    /* per entry: summary label */
    GPtrArray         *errors;       /* per entry: error label */
    GPtrArray         *messages;     /* per entry: error text or NULL (for describe) */
    char               chain_error[768];
    int                suppress;
} FD;

static FD *get_fd(GtkWindow *w)
{
    return w ? g_object_get_data(G_OBJECT(w), "filter-dialog") : NULL;
}

/* Why `cf` cannot be in a chain for a stream of `type` (FALSE if it can). */
static gboolean class_problem(const CapsFilter *cf, enum AVMediaType type, char *buf, size_t size)
{
    switch (cf->cls) {
    case CAPS_FILTER_SIMPLE:
        if (cf->in_type == type)
            return FALSE;
        g_snprintf(buf, size, "%s is a%s %s filter but the stream is %s", cf->name,
                   cf->in_type == AVMEDIA_TYPE_AUDIO ? "n" : "", av_get_media_type_string(cf->in_type),
                   av_get_media_type_string(type));
        return TRUE;
    case CAPS_FILTER_HW:
        g_snprintf(buf, size, "%s works on hardware frames, which are not supported yet", cf->name);
        return TRUE;
    case CAPS_FILTER_CONVERT:
        g_snprintf(buf, size, "%s turns %s into %s, which a stream's chain cannot do", cf->name,
                   av_get_media_type_string(cf->in_type), av_get_media_type_string(cf->out_type));
        return TRUE;
    default:
        g_snprintf(buf, size, "%s is a %s filter; a stream's chain only takes one-input, "
                   "one-output filters", cf->name, caps_filter_class_name(cf->cls));
        return TRUE;
    }
}

static gboolean has_options(const AVFilter *f)
{
    return (f->priv_class && av_opt_next(&f->priv_class, NULL)) ||
           (f->flags & AVFILTER_FLAG_SUPPORT_TIMELINE);
}

/* ------------------------------------------------------------------------- */
/* checking                                                                  */

static void set_entry_error(FD *fd, int i, const char *msg)
{
    GtkWidget *l;

    if (i < 0 || i >= (int)fd->errors->len)
        return;
    l = g_ptr_array_index(fd->errors, i);
    gtk_label_set_text(GTK_LABEL(l), msg ? msg : "");
    gtk_widget_set_visible(l, msg != NULL);
    g_free(fd->messages->pdata[i]);
    fd->messages->pdata[i] = g_strdup(msg);
}

static void set_status(FD *fd, gboolean ok, const char *text)
{
    gtk_image_set_from_icon_name(GTK_IMAGE(fd->status_icon), ok ? "object-select-symbolic" : "dialog-error-symbolic");
    gtk_widget_remove_css_class(fd->status_icon, ok ? "error" : "success");
    gtk_widget_add_css_class(fd->status_icon, ok ? "success" : "error");
    gtk_label_set_text(GTK_LABEL(fd->status), text);
    g_strlcpy(fd->chain_error, ok ? "" : text, sizeof(fd->chain_error));
}

/* "cannot set up the filters: Invalid argument (Parsed_crop_2: Invalid too
 * big...)" -> "Invalid too big..." when the filter is shown anyway. */
static void tidy_reason(char *msg, gboolean attributed)
{
    char *p = strstr(msg, "(Parsed_"), *colon, *end;

    if (!attributed || !p || !(colon = strstr(p, ": ")) || !(end = strrchr(colon, ')')))
        return;
    *end = '\0';
    memmove(msg, colon + 2, strlen(colon + 2) + 1);
}

static void check(FD *fd)
{
    gboolean text_mode = filter_chain_text(fd->chain) != NULL, static_fail = FALSE;
    char msg[768], *str;
    int idx = -1;

    for (int i = 0; i < (int)fd->errors->len; i++)
        set_entry_error(fd, i, NULL);

    if (!text_mode) {
        for (int i = 0; i < filter_chain_length(fd->chain); i++) {
            const FilterEntry *e = filter_chain_get(fd->chain, i);
            const CapsFilter *cf = caps_find_filter(fd->caps, e->filter->name);
            if (e->enabled && cf && class_problem(cf, fd->ms->type, msg, sizeof(msg))) {
                set_entry_error(fd, i, msg);
                static_fail = TRUE;
            }
        }
        if (static_fail) {
            set_status(fd, FALSE, "Some filters cannot be used in this chain (see above).");
            return;
        }
    }

    str = filter_chain_string(fd->chain);
    if (!*str) {
        set_status(fd, TRUE, "No filters: the stream is encoded as decoded.");
    } else if (validate_filter_chain(fd->ms, str, &idx, msg, sizeof(msg)) < 0) {
        int ei = text_mode ? -1 : filter_chain_entry_index(fd->chain, idx);
        char full[800];
        tidy_reason(msg, ei >= 0);
        if (ei >= 0) {
            set_entry_error(fd, ei, msg);
            g_snprintf(full, sizeof(full), "%s: %s", filter_chain_get(fd->chain, ei)->filter->name, msg);
        } else {
            g_strlcpy(full, msg, sizeof(full));
        }
        set_status(fd, FALSE, full);
    } else {
        set_status(fd, TRUE, "The chain works with this stream.");
    }
    g_free(str);
}

static void refresh_text(FD *fd)
{
    char *s;

    if (filter_chain_text(fd->chain))
        return;   /* the user's own text stays as typed */
    s = filter_chain_string(fd->chain);
    fd->suppress++;
    gtk_editable_set_text(GTK_EDITABLE(fd->text), s);
    fd->suppress--;
    g_free(s);
}

static void rebuild(FD *fd);

/* After a change: show it, check it, tell the stream row. */
static void changed(FD *fd, gboolean structure)
{
    if (structure) {
        rebuild(fd);
    } else {
        refresh_text(fd);
        check(fd);
    }
    filter_chain_changed(fd->chain);
}

/* ------------------------------------------------------------------------- */
/* chain rows                                                                */

static int button_index(GtkWidget *b)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "entry"));
}

static void on_up(GtkButton *b, gpointer data)
{
    int i = button_index(GTK_WIDGET(b));
    filter_chain_move(((FD *)data)->chain, i, i - 1);
    changed(data, TRUE);
}

static void on_down(GtkButton *b, gpointer data)
{
    int i = button_index(GTK_WIDGET(b));
    filter_chain_move(((FD *)data)->chain, i, i + 1);
    changed(data, TRUE);
}

static void on_remove(GtkButton *b, gpointer data)
{
    filter_chain_remove(((FD *)data)->chain, button_index(GTK_WIDGET(b)));
    changed(data, TRUE);
}

static void on_switch(GObject *obj, GParamSpec *pspec, gpointer data)
{
    FD *fd = data;
    int i = button_index(GTK_WIDGET(obj));
    FilterEntry *e = filter_chain_get(fd->chain, i);
    GtkWidget *row_box = g_object_get_data(obj, "row-box");

    if (fd->suppress || !e)
        return;
    e->enabled = gtk_switch_get_active(GTK_SWITCH(obj));
    if (e->enabled)
        gtk_widget_remove_css_class(row_box, "dim-label");
    else
        gtk_widget_add_css_class(row_box, "dim-label");
    changed(fd, FALSE);
}

static void set_summary(FD *fd, int i)
{
    FilterEntry *e = filter_chain_get(fd->chain, i);
    char *s;

    if (!e || i >= (int)fd->summaries->len)
        return;
    s = filter_entry_summary(e);
    gtk_label_set_text(GTK_LABEL(g_ptr_array_index(fd->summaries, i)), *s ? s : "default options");
    g_free(s);
}

static void on_options_changed(void *user)
{
    FD *fd = user;

    set_summary(fd, fd->options_entry);
    changed(fd, FALSE);
}

static void on_options_destroyed(GtkWidget *w, gpointer data)
{
    ((FD *)data)->options_win = NULL;
}

GtkWindow *filter_dialog_edit_options(GtkWindow *dialog, int i, OptionEditor **editor)
{
    FD *fd = get_fd(dialog);
    FilterEntry *e = fd ? filter_chain_get(fd->chain, i) : NULL;
    OptionTarget t = { 0 };

    if (!e || filter_chain_text(fd->chain) || !has_options(e->filter))
        return NULL;
    t.filter = e->filter;
    fd->options_entry = i;
    fd->options_win = option_dialog_show(fd->win, t, &e->options, on_options_changed, fd, editor);
    g_signal_connect(fd->options_win, "destroy", G_CALLBACK(on_options_destroyed), fd);
    return fd->options_win;
}

static void on_options(GtkButton *b, gpointer data)
{
    filter_dialog_edit_options(((FD *)data)->win, button_index(GTK_WIDGET(b)), NULL);
}

static GtkWidget *icon_button(const char *icon, const char *tip, int entry, gboolean sensitive,
                              GCallback cb, FD *fd)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);

    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(b, sensitive);
    g_object_set_data(G_OBJECT(b), "entry", GINT_TO_POINTER(entry));
    g_signal_connect(b, "clicked", cb, fd);
    return b;
}

static void add_row(FD *fd, int i, int n)
{
    FilterEntry *e = filter_chain_get(fd->chain, i);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10), *text, *l, *sw, *opt, *lbrow;
    char *markup;

    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 6);

    sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), e->enabled);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(sw, "Use this filter (switch off to try the chain without it)");
    g_object_set_data(G_OBJECT(sw), "entry", GINT_TO_POINTER(i));
    gtk_box_append(GTK_BOX(box), sw);

    text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text, TRUE);
    l = gtk_label_new(NULL);
    markup = g_markup_printf_escaped("<b>%s</b>  <small>%s</small>", e->filter->name,
                                     e->filter->description ? e->filter->description : "");
    gtk_label_set_markup(GTK_LABEL(l), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(text), l);

    l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_widget_add_css_class(l, "monospace");
    gtk_box_append(GTK_BOX(text), l);
    g_ptr_array_add(fd->summaries, l);

    l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "error");
    gtk_widget_set_visible(l, FALSE);
    gtk_box_append(GTK_BOX(text), l);
    g_ptr_array_add(fd->errors, l);
    g_ptr_array_add(fd->messages, NULL);
    gtk_box_append(GTK_BOX(box), text);
    if (!e->enabled)
        gtk_widget_add_css_class(text, "dim-label");
    g_object_set_data(G_OBJECT(sw), "row-box", text);
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_switch), fd);

    gtk_box_append(GTK_BOX(box), icon_button("go-up-symbolic", "Earlier in the chain", i, i > 0,
                                             G_CALLBACK(on_up), fd));
    gtk_box_append(GTK_BOX(box), icon_button("go-down-symbolic", "Later in the chain", i, i < n - 1,
                                             G_CALLBACK(on_down), fd));
    opt = gtk_button_new_with_label("Options…");
    gtk_widget_set_valign(opt, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(opt, has_options(e->filter));
    gtk_widget_set_tooltip_text(opt, has_options(e->filter) ? "Filter options" : "This filter has no options");
    g_object_set_data(G_OBJECT(opt), "entry", GINT_TO_POINTER(i));
    g_signal_connect(opt, "clicked", G_CALLBACK(on_options), fd);
    gtk_box_append(GTK_BOX(box), opt);
    gtk_box_append(GTK_BOX(box), icon_button("window-close-symbolic", "Remove from the chain", i, TRUE,
                                             G_CALLBACK(on_remove), fd));

    gtk_list_box_append(GTK_LIST_BOX(fd->list), box);
    lbrow = gtk_widget_get_parent(box);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(lbrow), FALSE);
    set_summary(fd, i);
}

static void rebuild(FD *fd)
{
    int n = filter_chain_length(fd->chain);
    gboolean text_mode = filter_chain_text(fd->chain) != NULL;

    gtk_list_box_remove_all(GTK_LIST_BOX(fd->list));
    g_ptr_array_set_size(fd->summaries, 0);
    g_ptr_array_set_size(fd->errors, 0);
    g_ptr_array_set_size(fd->messages, 0);
    for (int i = 0; i < n; i++)
        add_row(fd, i, n);
    gtk_widget_set_visible(fd->list, n > 0);
    gtk_widget_set_visible(fd->empty, n == 0);
    gtk_widget_set_sensitive(fd->list, !text_mode);
    gtk_widget_set_sensitive(fd->add_btn, !text_mode);
    gtk_widget_set_sensitive(fd->add_dd, !text_mode);
    gtk_widget_set_visible(fd->back_btn, text_mode);
    refresh_text(fd);
    check(fd);
}

/* ------------------------------------------------------------------------- */
/* adding, text                                                              */

static void on_add_selected(GObject *obj, GParamSpec *pspec, gpointer data)
{
    FD *fd = data;
    ConvChoiceItem *it = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(fd->add_dd));
    const CapsFilter *cf = it ? conv_choice_item_get_data(it) : NULL;
    const char *reason = it ? conv_choice_item_get_reason(it) : NULL;
    char *s;

    if (!cf) {
        gtk_label_set_text(GTK_LABEL(fd->add_desc), "");
        return;
    }
    s = reason && *reason ? g_strdup_printf("%s — %s", cf->description ? cf->description : "", reason)
                          : g_strdup(cf->description ? cf->description : "");
    gtk_label_set_text(GTK_LABEL(fd->add_desc), s);
    g_free(s);
}

static void on_add(GtkButton *b, gpointer data)
{
    FD *fd = data;
    ConvChoiceItem *it = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(fd->add_dd));
    const CapsFilter *cf = it ? conv_choice_item_get_data(it) : NULL;

    if (!cf || filter_chain_text(fd->chain))
        return;
    filter_chain_append(fd->chain, cf->filter);
    changed(fd, TRUE);
}

gboolean filter_dialog_add(GtkWindow *dialog, const char *name)
{
    FD *fd = get_fd(dialog);
    guint n;

    if (!fd)
        return FALSE;
    n = g_list_model_get_n_items(G_LIST_MODEL(fd->choices));
    for (guint i = 0; i < n; i++) {
        ConvChoiceItem *it = g_list_model_get_item(G_LIST_MODEL(fd->choices), i);
        const CapsFilter *cf = conv_choice_item_get_data(it);
        gboolean match = !strcmp(cf->name, name);
        g_object_unref(it);
        if (match) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(fd->add_dd), i);
            on_add(NULL, fd);
            return TRUE;
        }
    }
    return FALSE;
}

static void on_text_changed(GtkEditable *e, gpointer data)
{
    FD *fd = data;
    gboolean was_text = filter_chain_text(fd->chain) != NULL;

    if (fd->suppress)
        return;
    filter_chain_set_text(fd->chain, gtk_editable_get_text(e));
    changed(fd, !was_text);   /* entering text mode greys the list */
}

void filter_dialog_set_text(GtkWindow *dialog, const char *text)
{
    FD *fd = get_fd(dialog);

    if (fd)
        gtk_editable_set_text(GTK_EDITABLE(fd->text), text);
}

static void on_back(GtkButton *b, gpointer data)
{
    FD *fd = data;

    filter_chain_set_text(fd->chain, NULL);
    changed(fd, TRUE);
}

static void on_clear(GtkButton *b, gpointer data)
{
    FD *fd = data;

    filter_chain_set_text(fd->chain, NULL);
    while (filter_chain_length(fd->chain))
        filter_chain_remove(fd->chain, 0);
    changed(fd, TRUE);
}

char *filter_dialog_describe(GtkWindow *dialog)
{
    FD *fd = get_fd(dialog);
    GString *g = g_string_new(NULL);
    char *s;

    if (!fd)
        return g_string_free(g, FALSE);
    for (int i = 0; i < filter_chain_length(fd->chain); i++) {
        const FilterEntry *e = filter_chain_get(fd->chain, i);
        const char *m = i < (int)fd->messages->len ? fd->messages->pdata[i] : NULL;
        g_string_append_printf(g, "#%d %s %s %s%s\n", i, e->filter->name, e->enabled ? "on" : "off",
                               m ? "error: " : "ok", m ? m : "");
    }
    s = filter_chain_string(fd->chain);
    g_string_append_printf(g, "text %s%s\n", filter_chain_text(fd->chain) ? "(custom) " : "", s);
    g_free(s);
    g_string_append_printf(g, "chain %s%s\n", fd->chain_error[0] ? "error: " : "ok", fd->chain_error);
    return g_string_free(g, FALSE);
}

/* ------------------------------------------------------------------------- */
/* window                                                                    */

static void fill_choices(FD *fd)
{
    const CapsFilter **arr = NULL;
    int nb = 0;

    if (caps_filters_for(fd->caps, fd->ms->type, CAPS_FILTER_ALL, &arr, &nb) < 0)
        return;
    /* usable ones first, each group by name */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nb; i++) {
            char reason[256];
            gboolean bad = class_problem(arr[i], fd->ms->type, reason, sizeof(reason));
            ConvChoiceItem *it;
            if (bad != (pass == 1))
                continue;
            it = conv_choice_item_new(arr[i]->name, arr[i]);
            conv_choice_item_set_state(it, !bad, bad ? reason : NULL);
            g_list_store_append(fd->choices, it);
            g_object_unref(it);
        }
    }
    av_free(arr);
}

static void on_done(GtkButton *b, gpointer data)
{
    gtk_window_close(data);
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    FD *fd = data;

    if (fd->options_win) {
        g_signal_handlers_disconnect_by_data(fd->options_win, fd);
        gtk_window_destroy(fd->options_win);
    }
    filter_chain_set_dialog(fd->chain, NULL);
    filter_chain_unref(fd->chain);
    g_object_unref(fd->choices);
    g_ptr_array_free(fd->summaries, TRUE);
    g_ptr_array_free(fd->errors, TRUE);
    g_ptr_array_free(fd->messages, TRUE);
    g_free(fd);
}

static GtkWidget *heading(const char *text, const char *detail)
{
    GtkWidget *l = gtk_label_new(NULL);
    char *markup = detail ? g_markup_printf_escaped("<b>%s</b>  <small>%s</small>", text, detail)
                          : g_markup_printf_escaped("<b>%s</b>", text);

    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    g_free(markup);
    return l;
}

GtkWindow *filter_dialog_show(GtkWindow *parent, const Caps *caps, const MediaStream *ms, FilterChain *chain)
{
    GtkWindow *existing = filter_chain_get_dialog(chain);
    FD *fd;
    GtkWidget *box, *line, *scroll, *listbox, *buttons, *clear, *done, *spacer;
    char title[128];

    if (existing) {
        gtk_window_present(existing);
        return existing;
    }
    fd = g_new0(FD, 1);
    fd->caps      = caps;
    fd->ms        = ms;
    fd->chain     = filter_chain_ref(chain);
    fd->choices   = g_list_store_new(CONV_TYPE_CHOICE_ITEM);
    fd->summaries = g_ptr_array_new();
    fd->errors    = g_ptr_array_new();
    fd->messages  = g_ptr_array_new_with_free_func(g_free);
    fill_choices(fd);

    fd->win = GTK_WINDOW(gtk_window_new());
    g_snprintf(title, sizeof(title), "Filters — stream #%d (%s)", ms->index, av_get_media_type_string(ms->type));
    gtk_window_set_title(fd->win, title);
    gtk_window_set_transient_for(fd->win, parent);
    gtk_window_set_destroy_with_parent(fd->win, TRUE);
    gtk_window_set_default_size(fd->win, 760, 560);
    g_object_set_data(G_OBJECT(fd->win), "filter-dialog", fd);
    filter_chain_set_dialog(chain, fd->win);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    /* add */
    line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(line), heading("Add", NULL));
    fd->add_dd = conv_choice_dropdown_new(fd->choices, TRUE);
    gtk_widget_set_hexpand(fd->add_dd, TRUE);
    gtk_widget_set_tooltip_text(fd->add_dd, "Filters for this stream (type to search)");
    gtk_box_append(GTK_BOX(line), fd->add_dd);
    fd->add_btn = gtk_button_new_with_mnemonic("_Add");
    gtk_box_append(GTK_BOX(line), fd->add_btn);
    gtk_box_append(GTK_BOX(box), line);
    fd->add_desc = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(fd->add_desc), 0);
    gtk_label_set_wrap(GTK_LABEL(fd->add_desc), TRUE);
    gtk_widget_add_css_class(fd->add_desc, "dim-label");
    gtk_box_append(GTK_BOX(box), fd->add_desc);

    /* chain */
    gtk_box_append(GTK_BOX(box), heading("Chain", "applied in this order, after decoding"));
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    listbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    fd->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(fd->list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(fd->list, "boxed-list");
    fd->empty = gtk_label_new("No filters yet: pick one above and press Add.");
    gtk_widget_add_css_class(fd->empty, "dim-label");
    gtk_widget_set_margin_top(fd->empty, 24);
    gtk_widget_set_margin_bottom(fd->empty, 24);
    gtk_box_append(GTK_BOX(listbox), fd->list);
    gtk_box_append(GTK_BOX(listbox), fd->empty);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), listbox);
    {
        GtkWidget *frame = gtk_frame_new(NULL);
        gtk_frame_set_child(GTK_FRAME(frame), scroll);
        gtk_widget_set_vexpand(frame, TRUE);
        gtk_box_append(GTK_BOX(box), frame);
    }

    /* text */
    gtk_box_append(GTK_BOX(box), heading("Filtergraph", "what the chain gives; edit it to write the chain by hand"));
    line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    fd->text = gtk_entry_new();
    gtk_widget_set_hexpand(fd->text, TRUE);
    gtk_widget_add_css_class(fd->text, "monospace");
    gtk_entry_set_placeholder_text(GTK_ENTRY(fd->text), "e.g. scale=1280:-2,hflip");
    gtk_box_append(GTK_BOX(line), fd->text);
    fd->back_btn = gtk_button_new_with_mnemonic("_Back to the list");
    gtk_widget_set_tooltip_text(fd->back_btn, "Drop the text and use the list again");
    gtk_box_append(GTK_BOX(line), fd->back_btn);
    gtk_box_append(GTK_BOX(box), line);

    line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    fd->status_icon = gtk_image_new();
    gtk_widget_set_valign(fd->status_icon, GTK_ALIGN_START);
    fd->status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(fd->status), 0);
    gtk_label_set_wrap(GTK_LABEL(fd->status), TRUE);
    gtk_label_set_selectable(GTK_LABEL(fd->status), TRUE);
    gtk_widget_set_hexpand(fd->status, TRUE);
    gtk_box_append(GTK_BOX(line), fd->status_icon);
    gtk_box_append(GTK_BOX(line), fd->status);
    gtk_box_append(GTK_BOX(box), line);

    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    clear = gtk_button_new_with_mnemonic("_Remove all");
    done = gtk_button_new_with_mnemonic("_Done");
    gtk_widget_add_css_class(done, "suggested-action");
    spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(buttons), clear);
    gtk_box_append(GTK_BOX(buttons), spacer);
    gtk_box_append(GTK_BOX(buttons), done);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_child(fd->win, box);

    if (filter_chain_text(chain)) {
        fd->suppress++;
        gtk_editable_set_text(GTK_EDITABLE(fd->text), filter_chain_text(chain));
        fd->suppress--;
    }
    rebuild(fd);
    on_add_selected(NULL, NULL, fd);

    g_signal_connect(fd->add_dd, "notify::selected", G_CALLBACK(on_add_selected), fd);
    g_signal_connect(fd->add_btn, "clicked", G_CALLBACK(on_add), fd);
    g_signal_connect(fd->text, "changed", G_CALLBACK(on_text_changed), fd);
    g_signal_connect(fd->back_btn, "clicked", G_CALLBACK(on_back), fd);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear), fd);
    g_signal_connect(done, "clicked", G_CALLBACK(on_done), fd->win);
    g_signal_connect(fd->win, "destroy", G_CALLBACK(on_destroy), fd);

    gtk_window_present(fd->win);
    return fd->win;
}

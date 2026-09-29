/*
 * choice_item.c - dropdown entries that can be shown as incompatible.
 */
#include "choice_item.h"

struct _ConvChoiceItem {
    GObject       parent;
    char         *label;
    char         *reason;
    gboolean      enabled;
    gconstpointer data;
};

enum { PROP_0, PROP_LABEL, PROP_ENABLED, PROP_REASON, N_PROPS };
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(ConvChoiceItem, conv_choice_item, G_TYPE_OBJECT)

static void item_get_property(GObject *obj, guint id, GValue *value, GParamSpec *pspec)
{
    ConvChoiceItem *self = CONV_CHOICE_ITEM(obj);

    switch (id) {
    case PROP_LABEL:   g_value_set_string(value, self->label);    break;
    case PROP_ENABLED: g_value_set_boolean(value, self->enabled); break;
    case PROP_REASON:  g_value_set_string(value, self->reason);   break;
    default:           G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void item_finalize(GObject *obj)
{
    ConvChoiceItem *self = CONV_CHOICE_ITEM(obj);

    g_free(self->label);
    g_free(self->reason);
    G_OBJECT_CLASS(conv_choice_item_parent_class)->finalize(obj);
}

static void conv_choice_item_class_init(ConvChoiceItemClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);

    oc->get_property = item_get_property;
    oc->finalize     = item_finalize;
    props[PROP_LABEL]   = g_param_spec_string("label", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    props[PROP_ENABLED] = g_param_spec_boolean("enabled", NULL, NULL, TRUE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    props[PROP_REASON]  = g_param_spec_string("reason", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(oc, N_PROPS, props);
}

static void conv_choice_item_init(ConvChoiceItem *self)
{
    self->enabled = TRUE;
}

ConvChoiceItem *conv_choice_item_new(const char *label, gconstpointer data)
{
    ConvChoiceItem *self = g_object_new(CONV_TYPE_CHOICE_ITEM, NULL);

    self->label = g_strdup(label);
    self->data  = data;
    return self;
}

const char *conv_choice_item_get_label(ConvChoiceItem *item)    { return item->label; }
gconstpointer conv_choice_item_get_data(ConvChoiceItem *item)   { return item->data; }
gboolean conv_choice_item_get_enabled(ConvChoiceItem *item)     { return item->enabled; }
const char *conv_choice_item_get_reason(ConvChoiceItem *item)   { return item->reason; }

void conv_choice_item_set_state(ConvChoiceItem *item, gboolean enabled, const char *reason)
{
    if (!enabled && !reason)
        reason = "Not possible with the current choices";
    if (item->enabled == enabled && !g_strcmp0(item->reason, enabled ? NULL : reason))
        return;
    item->enabled = enabled;
    g_free(item->reason);
    item->reason = enabled ? NULL : g_strdup(reason);
    g_object_notify_by_pspec(G_OBJECT(item), props[PROP_ENABLED]);
}

/* ------------------------------------------------------------------------- */
/* rendering: label + warning icon; greyed out when not enabled              */

static void refresh(GtkListItem *li)
{
    ConvChoiceItem *item = gtk_list_item_get_item(li);
    GtkWidget *box = gtk_list_item_get_child(li);
    GtkWidget *label = gtk_widget_get_first_child(box);
    GtkWidget *icon = gtk_widget_get_next_sibling(label);

    if (!item)
        return;
    gtk_label_set_text(GTK_LABEL(label), item->label);
    gtk_widget_set_visible(icon, !item->enabled);
    if (item->enabled) {
        gtk_widget_remove_css_class(box, "dim-label");
        gtk_widget_set_tooltip_text(box, NULL);
    } else {
        gtk_widget_add_css_class(box, "dim-label");
        gtk_widget_set_tooltip_text(box, item->reason);
    }
}

static void on_item_notify(GObject *obj, GParamSpec *pspec, gpointer li)
{
    refresh(li);
}

static void factory_setup(GtkSignalListItemFactory *f, GObject *obj, gpointer data)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new(NULL);
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");

    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_add_css_class(icon, "warning");
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_list_item_set_child(GTK_LIST_ITEM(obj), box);
}

static void factory_bind(GtkSignalListItemFactory *f, GObject *obj, gpointer data)
{
    GtkListItem *li = GTK_LIST_ITEM(obj);
    ConvChoiceItem *item = gtk_list_item_get_item(li);

    refresh(li);
    if (item)
        g_signal_connect(item, "notify::enabled", G_CALLBACK(on_item_notify), li);
}

static void factory_unbind(GtkSignalListItemFactory *f, GObject *obj, gpointer data)
{
    ConvChoiceItem *item = gtk_list_item_get_item(GTK_LIST_ITEM(obj));

    if (item)
        g_signal_handlers_disconnect_by_data(item, obj);
}

GtkWidget *conv_choice_dropdown_new(GListStore *store, gboolean search)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    GtkWidget *dd;

    g_signal_connect(factory, "setup", G_CALLBACK(factory_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(factory_bind), NULL);
    g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind), NULL);

    dd = gtk_drop_down_new(G_LIST_MODEL(g_object_ref(store)),
                           gtk_property_expression_new(CONV_TYPE_CHOICE_ITEM, NULL, "label"));
    gtk_drop_down_set_factory(GTK_DROP_DOWN(dd), factory);   /* button and popup */
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(dd), search);
    g_object_unref(factory);
    return dd;
}

guint conv_choice_find(GListModel *model, gconstpointer data)
{
    guint n = g_list_model_get_n_items(model);

    for (guint i = 0; i < n; i++) {
        ConvChoiceItem *item = g_list_model_get_item(model, i);
        gboolean match = item->data == data;
        g_object_unref(item);
        if (match)
            return i;
    }
    return GTK_INVALID_LIST_POSITION;
}

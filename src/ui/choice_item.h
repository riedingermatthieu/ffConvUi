/*
 * choice_item.h - dropdown entries that can be shown as incompatible.
 *
 * A ConvChoiceItem has a label, an "enabled" state and the reason it is not.
 * Incompatible entries stay selectable (the user decides; validation then
 * explains and keeps Convert disabled): they are only drawn greyed out with
 * a warning icon, the reason as tooltip. Changing an item's state redraws
 * it wherever it is shown, including in the dropdown's button.
 */
#ifndef UI_CHOICE_ITEM_H
#define UI_CHOICE_ITEM_H

#include <gtk/gtk.h>

#define CONV_TYPE_CHOICE_ITEM (conv_choice_item_get_type())
G_DECLARE_FINAL_TYPE(ConvChoiceItem, conv_choice_item, CONV, CHOICE_ITEM, GObject)

ConvChoiceItem *conv_choice_item_new(const char *label, gconstpointer data);

const char     *conv_choice_item_get_label(ConvChoiceItem *item);
gconstpointer   conv_choice_item_get_data(ConvChoiceItem *item);
gboolean        conv_choice_item_get_enabled(ConvChoiceItem *item);
const char     *conv_choice_item_get_reason(ConvChoiceItem *item);
/* reason: why it is not possible (NULL when enabled) */
void            conv_choice_item_set_state(ConvChoiceItem *item, gboolean enabled, const char *reason);

/* A GtkDropDown over a GListStore of ConvChoiceItem, drawing incompatible
 * items greyed out, with type-to-search on the labels. */
GtkWidget *conv_choice_dropdown_new(GListStore *store, gboolean search);

/* Position of the item whose data is `data`, or GTK_INVALID_LIST_POSITION. */
guint conv_choice_find(GListModel *model, gconstpointer data);

#endif /* UI_CHOICE_ITEM_H */

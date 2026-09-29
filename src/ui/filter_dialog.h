/*
 * filter_dialog.h - edit a stream's filter chain.
 *
 * A window (not modal: the main window and the preview stay usable) with
 * the chain as a list (reorder, switch off, edit options, remove), a
 * searchable list of the filters for the stream's media type to add from,
 * and the resulting filtergraph text, which can also be edited directly.
 * Every change is checked at once against the stream's real parameters and
 * errors are shown on the filter at fault.
 */
#ifndef UI_FILTER_DIALOG_H
#define UI_FILTER_DIALOG_H

#include <gtk/gtk.h>

#include "caps.h"
#include "filter_chain.h"
#include "option_editor.h"
#include "probe.h"

/* Open (or bring up) the dialog for `chain`, which it keeps a reference to.
 * `ms` must stay valid while the window is open. */
GtkWindow *filter_dialog_show(GtkWindow *parent, const Caps *caps, const MediaStream *ms,
                              FilterChain *chain);

/* As a user would (test hooks): add a filter by name, open an entry's
 * options, type filtergraph text. */
gboolean   filter_dialog_add(GtkWindow *dialog, const char *name);
GtkWindow *filter_dialog_edit_options(GtkWindow *dialog, int entry, OptionEditor **editor);
void       filter_dialog_set_text(GtkWindow *dialog, const char *text);

/* What the dialog shows, one line per entry ("#0 scale on ok" /
 * "#1 crop on error: ...") then "chain ok" or "chain error: ..." (g_free it). */
char      *filter_dialog_describe(GtkWindow *dialog);

#endif /* UI_FILTER_DIALOG_H */

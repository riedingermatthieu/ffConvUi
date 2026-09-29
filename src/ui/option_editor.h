/*
 * option_editor.h - option editor generated from FFmpeg's AVOption metadata.
 *
 * Works for any encoder or muxer: no option is hard-coded except the short
 * "Common" list and the Quality control (the ffmpeg CLI's -q, which is not
 * an option: it sets global_quality and the qscale flag).
 *
 * Only the options the user changes are written to the caller's dictionary,
 * as the strings FFmpeg expects ("veryfast", "30", "a+b" for flags...).
 */
#ifndef UI_OPTION_EDITOR_H
#define UI_OPTION_EDITOR_H

#include <gtk/gtk.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>

typedef struct OptionEditor OptionEditor;

typedef void (*OptionsChanged)(void *user);

/* Exactly one of codec / muxer is set. */
typedef struct OptionTarget {
    const AVCodec        *codec;
    const AVOutputFormat *muxer;
    const char           *muxer_key;   /* display name for a muxer ("matroska:mka") */
} OptionTarget;

/* Open the editor in a modal window. `values` is edited in place (live) and
 * must stay valid while the window is open; `changed` is called after every
 * change. Returns the window; *editor (optional) is valid until it closes. */
GtkWindow *option_dialog_show(GtkWindow *parent, OptionTarget target, AVDictionary **values,
                              OptionsChanged changed, void *user, OptionEditor **editor);

/* Set an option through its widget, as a user would ("crf", "30").
 * "q" drives the Quality control. Returns FALSE if the option is not shown
 * or the value does not fit the widget. (Used by the test hooks.) */
gboolean option_editor_set_text(OptionEditor *ed, const char *name, const char *value);

/* Filter the rows as the search entry does. */
void option_editor_search(OptionEditor *ed, const char *text);

#endif /* UI_OPTION_EDITOR_H */

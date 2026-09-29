/*
 * testhooks.h - drive the UI without a user, for the GUI tests.
 *
 * Built into ffconv when CMake's CONV_TEST_HOOKS is ON (testhooks.c);
 * otherwise testhooks_none.c provides a no-op. The application only calls
 * testhooks_install(); everything else goes through the windows' public
 * events and operations (window.h, progress.h, stream_row.h,
 * option_editor.h), as a user's actions would.
 */
#ifndef UI_TESTHOOKS_H
#define UI_TESTHOOKS_H

#include <gtk/gtk.h>

/* Watch `app`'s windows and act on the FFCONV_TEST_* environment variables
 * (nothing happens when none is set). */
void testhooks_install(GtkApplication *app);

#endif /* UI_TESTHOOKS_H */

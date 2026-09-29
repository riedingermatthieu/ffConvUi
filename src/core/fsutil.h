/*
 * fsutil.h - small file-system helpers taking UTF-8 paths (as FFmpeg does,
 * also on Windows).
 */
#ifndef CONV_FSUTIL_H
#define CONV_FSUTIL_H

/* Delete a local file ("file:" URLs too; other protocols are ignored). */
void conv_delete_file(const char *path);

#endif /* CONV_FSUTIL_H */

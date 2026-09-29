/*
 * fsutil.c - file-system helpers with UTF-8 paths.
 */
#include "fsutil.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/mem.h>

void conv_delete_file(const char *path)
{
    const char *proto = avio_find_protocol_name(path);

    if (!proto || strcmp(proto, "file"))
        return;
    av_strstart(path, "file:", &path);
#ifdef _WIN32
    {
        /* the C runtime's remove() takes the ANSI code page, not UTF-8 */
        int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        wchar_t *w = n > 0 ? av_malloc_array(n, sizeof(*w)) : NULL;
        if (w && MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n) > 0)
            DeleteFileW(w);
        av_free(w);
    }
#else
    remove(path);
#endif
}

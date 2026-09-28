/*
 * logcap.h - capture FFmpeg log messages per thread.
 *
 * FFmpeg often explains a failure only in its log ("Error setting preset
 * warp", "Could not find tag for codec subrip") while the API returns a bare
 * AVERROR(EINVAL). logcap installs one global av_log callback that, on
 * threads where a capture is active, records the first and last error messages so they
 * can be attached to our own error messages.
 *
 * Messages logged from a codec's internal worker threads are not captured
 * (no capture is active there); initialisation errors are logged on the
 * calling thread, which is what matters here.
 */
#ifndef CONV_LOGCAP_H
#define CONV_LOGCAP_H

#include <stdarg.h>

typedef struct LogCapture {
    int  level;          /* capture messages at this level or more severe (AV_LOG_ERROR) */
    int  forward;        /* also pass captured-thread messages to the fallback callback */
    char first[512];     /* first complete message since the last clear: usually the root
                            cause ("libx264: Error setting preset..."), or "" */
    char last[512];      /* last complete message, or "" */
    char pending[512];   /* message being assembled across av_log calls */
} LogCapture;

typedef void (*LogCallback)(void *avcl, int level, const char *fmt, va_list vl);

/* Install the capturing callback (idempotent). Messages from threads without
 * an active capture, and forwarded ones, go to `fallback`
 * (NULL = av_log_default_callback). Call before starting worker threads. */
void logcap_install(LogCallback fallback);

/* Install the callback if nobody did yet, keeping any fallback already set.
 * The engine and validator call this themselves. */
void logcap_ensure_installed(void);

/* Start / stop capturing on the calling thread (not nestable). */
void logcap_begin(LogCapture *c);
void logcap_end(void);

/* Forget the messages (call before an operation whose failure you want to explain). */
void logcap_clear(LogCapture *c);

#endif /* CONV_LOGCAP_H */

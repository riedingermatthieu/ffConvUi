/*
 * preview.h - what one video frame will look like after conversion.
 *
 * "Before" is the input frame at the chosen time. "After" is the same frame
 * after the real pipeline: a short clip around that time is converted by the
 * engine with the stream's encoder, options and filters (so rate control,
 * scaling and filtering are the real ones), then decoded again.
 */
#ifndef CONV_PREVIEW_H
#define CONV_PREVIEW_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "job.h"

typedef struct PreviewImage {
    int      width, height;   /* pixels, display aspect applied (non-square pixels widened) */
    int      stride;          /* bytes per row */
    uint8_t *rgba;            /* 8-bit RGBA */
    double   time;            /* seconds from the input's start (after: from the clip's) */
    int      coded_width, coded_height;
    char     pix_fmt[32];     /* of the decoded frame */
} PreviewImage;

typedef struct PreviewResult {
    PreviewImage before;
    PreviewImage after;           /* same as before for a copied stream */
    int          copied;          /* the stream is copied: nothing changes */
    char         encoder[64];
    int64_t      clip_bytes;      /* size of the encoded clip's video */
    double       clip_seconds;
    double       kbps;            /* the clip's video bitrate */
    double       encode_seconds;  /* wall time of the clip conversion */
} PreviewResult;

typedef struct PreviewRequest {
    const ConvJob *job;
    int            stream;        /* index in job->streams: a video stream, copied or converted */
    double         time;          /* seconds from the input's start */
    double         lead;          /* clip starts this much earlier (default 0.5 s) */
    double         tail;          /* and ends this much later (default 1.0 s): rate control */
                                  /* and look-ahead need frames around the one shown */
    const char    *scratch;       /* where to write the clip (deleted afterwards) */
} PreviewRequest;

/* Returns 0, AVERROR_EXIT if cancelled, or a negative AVERROR with a message
 * in err. Free the result with preview_result_free(). */
int  preview_render(const PreviewRequest *req, atomic_int *cancel, PreviewResult *out,
                    char *err, size_t errlen);
void preview_result_free(PreviewResult *r);

/* Write an image as PNG (UTF-8 path). */
int  preview_save_png(const PreviewImage *img, const char *path);

#endif /* CONV_PREVIEW_H */

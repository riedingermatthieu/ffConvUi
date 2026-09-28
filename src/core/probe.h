/*
 * probe.h - describe the contents of an input media file.
 *
 * MediaInfo is a plain snapshot: it owns all its memory and holds no FFmpeg
 * context pointers, so it can be kept around after the file is closed and
 * handed to other threads.
 */
#ifndef CONV_PROBE_H
#define CONV_PROBE_H

#include <stddef.h>
#include <stdint.h>

#include <libavcodec/codec_id.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>

typedef struct MiTag {
    char *key;
    char *value;
} MiTag;

typedef struct MiChapter {
    double start;   /* seconds */
    double end;     /* seconds */
    char  *title;   /* may be NULL */
} MiChapter;

typedef struct MediaStream {
    int              index;
    enum AVMediaType type;
    enum AVCodecID   codec_id;
    char             codec_name[64];
    char             codec_long_name[128];
    char             profile[64];
    uint32_t         codec_tag;
    int64_t          bit_rate;             /* 0 if unknown */
    int              bits_per_raw_sample;
    int              has_decoder;          /* a decoder exists in this build */

    AVRational       time_base;
    int64_t          start_time;           /* in time_base, AV_NOPTS_VALUE if unknown */
    int64_t          duration;             /* in time_base, AV_NOPTS_VALUE if unknown */
    int64_t          nb_frames;            /* 0 if unknown */

    int              disposition;          /* AV_DISPOSITION_* */
    char             language[16];
    char             title[128];
    MiTag           *tags;
    int              nb_tags;

    /* video */
    int              width, height;
    int              pix_fmt;              /* enum AVPixelFormat */
    char             pix_fmt_name[32];
    AVRational       sar, dar;
    AVRational       avg_frame_rate, r_frame_rate, guessed_frame_rate;
    int              color_range, color_space, color_primaries, color_trc;
    int              field_order;          /* enum AVFieldOrder */
    int              has_rotation;
    double           rotation;             /* degrees, as reported by the display matrix */
    int              has_mastering_display, has_content_light, has_dovi;
    int              is_attached_pic;

    /* audio */
    int              sample_rate;
    int              sample_fmt;           /* enum AVSampleFormat */
    char             sample_fmt_name[32];
    int              channels;
    char             ch_layout[64];
    int              frame_size;

    /* subtitle */
    int              is_text_sub, is_bitmap_sub;

    /* attachment */
    char             filename[128];
    char             mimetype[64];
} MediaStream;

typedef struct MediaInfo {
    char        *path;
    char         format_name[64];
    char         format_long_name[128];
    int64_t      duration_us;    /* AV_NOPTS_VALUE if unknown */
    int64_t      start_time_us;  /* AV_NOPTS_VALUE if unknown */
    int64_t      bit_rate;       /* 0 if unknown */
    int64_t      file_size;      /* -1 if unknown */

    MediaStream *streams;
    int          nb_streams;
    MiChapter   *chapters;
    int          nb_chapters;
    MiTag       *tags;
    int          nb_tags;
} MediaInfo;

/* Open and analyse `path`. On success *out receives a new MediaInfo (free with
 * mi_free) and 0 is returned. On failure a negative AVERROR is returned and a
 * message is written to err (if err is not NULL). */
int  mi_probe(const char *path, MediaInfo **out, char *err, size_t errlen);
void mi_free(MediaInfo **mi);

/* Number of streams of a given type. */
int  mi_count_type(const MediaInfo *mi, enum AVMediaType type);

#endif /* CONV_PROBE_H */

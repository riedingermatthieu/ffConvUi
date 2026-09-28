/*
 * caps.h - capability catalog of the linked FFmpeg build.
 *
 * Caps is built once (caps_build) by enumerating every muxer, encoder and
 * filter. It is read-only afterwards and may be shared between threads.
 * The narrowing queries (caps_muxers_for, caps_stream_actions,
 * caps_filters_for) answer "what is possible for this file / stream".
 *
 * The pointers stored here (AVOutputFormat, AVCodec, AVFilter, the supported
 * config arrays) point to static FFmpeg data and never need freeing.
 */
#ifndef CONV_CAPS_H
#define CONV_CAPS_H

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>

#include "probe.h"

typedef enum CapsCompat {
    CAPS_NO    = 0,   /* known not to work */
    CAPS_MAYBE = 1,   /* FFmpeg has no information; needs a dry run to be sure */
    CAPS_YES   = 2,   /* known to work */
} CapsCompat;

typedef struct CapsMuxer {
    const AVOutputFormat *fmt;
    char                  key[64];      /* unique id: name, or "name:ext" for duplicates */
    int                   is_canonical; /* the muxer av_guess_format(name) returns */
    const char           *name;         /* FFmpeg name, not unique */
    const char           *long_name;
    const char           *extensions;   /* may be NULL */
    const char           *mime_type;    /* may be NULL */
    int                   flags;        /* AVFMT_* */
    int                   is_common;
} CapsMuxer;

typedef struct CapsEncoder {
    const AVCodec           *codec;
    const char              *name;
    const char              *long_name;
    const char              *wrapper_name;  /* external library/API name, may be NULL */
    enum AVMediaType         type;
    enum AVCodecID           id;
    int                      capabilities;  /* AV_CODEC_CAP_* */
    int                      props;         /* AV_CODEC_PROP_* of the codec id */
    int                      is_common;     /* 0, or preference rank (1 = most preferred) */
    int                      is_experimental;
    int                      is_hardware;

    /* Supported configurations. ptr == NULL means "not restricted / unknown". */
    const enum AVPixelFormat  *pix_fmts;     int nb_pix_fmts;
    const enum AVSampleFormat *sample_fmts;  int nb_sample_fmts;
    const int                 *sample_rates; int nb_sample_rates;
    const AVChannelLayout     *ch_layouts;   int nb_ch_layouts;
    const AVRational          *frame_rates;  int nb_frame_rates;
    const enum AVColorRange   *color_ranges; int nb_color_ranges;
    const enum AVColorSpace   *color_spaces; int nb_color_spaces;
} CapsEncoder;

typedef enum CapsFilterClass {
    CAPS_FILTER_SIMPLE    = 1 << 0,  /* 1 input -> 1 output, same media type, software */
    CAPS_FILTER_HW        = 1 << 1,  /* 1 -> 1, needs hardware frames or a device */
    CAPS_FILTER_CONVERT   = 1 << 2,  /* 1 -> 1, media type changes (e.g. showwaves) */
    CAPS_FILTER_MULTI_IN  = 1 << 3,  /* several or dynamic inputs (overlay, amix...) */
    CAPS_FILTER_MULTI_OUT = 1 << 4,  /* several outputs (split...) */
    CAPS_FILTER_SOURCE    = 1 << 5,  /* no input (testsrc, anullsrc...) */
    CAPS_FILTER_SINK      = 1 << 6,  /* no output (nullsink...) */
    CAPS_FILTER_ALL       = (1 << 7) - 1,
} CapsFilterClass;

typedef struct CapsFilter {
    const AVFilter  *filter;
    const char      *name;
    const char      *description;
    int              flags;             /* AVFILTER_FLAG_* */
    int              nb_inputs;         /* pads with default options (see below) */
    int              nb_outputs;
    int              dynamic_inputs;
    int              dynamic_outputs;
    int              default_pads_probed; /* dynamic filter created once to count its
                                             default pads; if 0, counts are static pads */
    enum AVMediaType in_type;           /* first pad, AVMEDIA_TYPE_UNKNOWN if none */
    enum AVMediaType out_type;
    CapsFilterClass  cls;
    int              is_hw;
    int              supports_timeline; /* accepts the generic `enable` option */
    int              metadata_only;     /* never modifies frame data */
} CapsFilter;

typedef struct Caps {
    CapsMuxer   *muxers;   int nb_muxers;     /* sorted by key */
    CapsEncoder *encoders; int nb_encoders;   /* sorted by name */
    CapsFilter  *filters;  int nb_filters;    /* sorted by name */
} Caps;

int  caps_build(Caps **out);
void caps_free(Caps **caps);

const CapsMuxer   *caps_find_muxer(const Caps *c, const char *key);
const CapsEncoder *caps_find_encoder(const Caps *c, const char *name);
const CapsFilter  *caps_find_filter(const Caps *c, const char *name);

/* Resolve a muxer key ("mp4", "matroska:mka") without building a catalog. */
const AVOutputFormat *caps_lookup_muxer(const char *key);

/* Can muxer `m` store a stream with codec `id`? */
CapsCompat caps_mux_codec(const CapsMuxer *m, enum AVCodecID id);

/* Muxers able to keep at least one video/audio/subtitle stream of `mi`. */
typedef struct CapsMuxerFit {
    const CapsMuxer *mux;
    int nb_considered;               /* video + audio + subtitle streams in the input */
    int nb_kept;                     /* of those, how many this muxer can keep */
    int kept_video, kept_audio, kept_subtitle;
} CapsMuxerFit;

/* *out is sorted: common first, then most streams kept, then name. av_free() it. */
int caps_muxers_for(const Caps *c, const MediaInfo *mi, CapsMuxerFit **out, int *nb);

/* What can be done with one input stream when writing to muxer `m`. */
typedef struct CapsEncChoice {
    const CapsEncoder *enc;
    CapsCompat         compat;
} CapsEncChoice;

typedef struct CapsStreamActions {
    CapsCompat     copy;           /* stream copy without re-encoding */
    int            can_transcode;  /* a decoder exists and the type is transcodable */
    CapsEncChoice *encoders;       /* sorted: YES first, common first, then name */
    int            nb_encoders;
    const char    *note;           /* human-readable reason when limited, may be NULL */
} CapsStreamActions;

int  caps_stream_actions(const Caps *c, const CapsMuxer *m, const MediaStream *s,
                         int include_experimental, CapsStreamActions *out);
void caps_stream_actions_free(CapsStreamActions *a);

/* Filters of classes in `class_mask` that apply to `type`
 * (AVMEDIA_TYPE_UNKNOWN = any type). *out is an array of pointers; av_free() it. */
int caps_filters_for(const Caps *c, enum AVMediaType type, unsigned class_mask,
                     const CapsFilter ***out, int *nb);

/* Hardware device types compiled in, and whether one can be opened here. */
typedef struct CapsHwDevice {
    enum AVHWDeviceType type;
    const char         *name;
    int                 available;
    char                error[128];
} CapsHwDevice;

int caps_probe_hw(CapsHwDevice **out, int *nb);

const char *caps_compat_name(CapsCompat c);
const char *caps_filter_class_name(CapsFilterClass cls);

#endif /* CONV_CAPS_H */

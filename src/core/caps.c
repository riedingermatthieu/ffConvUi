/*
 * caps.c - enumerate what the linked FFmpeg build can do, and narrow it down
 * to what is possible for a given input.
 */
#include "caps.h"

#include <stdlib.h>
#include <string.h>

#include <libavutil/avstring.h>

/* Shown first in the UI; everything else stays one click away. */
static const char *const common_muxers[] = {
    "mp4", "matroska", "webm", "mov", "mpegts", "avi", "mp3", "ipod",
    "adts", "ogg", "opus", "flac", "wav", "gif", NULL
};

/* In order of preference: the first one that fits is the default choice. */
static const char *const common_encoders[] = {
    /* video */
    "libx264", "libx265", "libsvtav1", "libvpx-vp9", "libaom-av1", "librav1e",
    "libvpx", "prores_ks", "dnxhd", "mpeg4", "mjpeg", "ffv1", "utvideo", "gif",
    "png", "h264_nvenc", "hevc_nvenc", "av1_nvenc", "h264_qsv", "hevc_qsv",
    "av1_qsv", "h264_amf", "hevc_amf", "av1_amf", "h264_videotoolbox",
    "hevc_videotoolbox", "h264_vaapi", "hevc_vaapi",
    /* audio */
    "aac", "libopus", "libfdk_aac", "libmp3lame", "libvorbis", "flac", "alac",
    "ac3", "eac3", "pcm_s16le", "pcm_s24le",
    /* subtitle */
    "mov_text", "srt", "subrip", "ass", "webvtt", "dvdsub",
    NULL
};

/* 1-based position of name in list, 0 if absent */
static int in_list(const char *const *list, const char *name)
{
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], name))
            return i + 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* catalog building                                                          */

static int cmp_muxer(const void *a, const void *b)
{
    return strcmp(((const CapsMuxer *)a)->key, ((const CapsMuxer *)b)->key);
}

static int cmp_muxer_name(const void *a, const void *b)
{
    const CapsMuxer *x = a, *y = b;
    int r = strcmp(x->name, y->name);

    /* the muxer av_guess_format() picks for this name comes first */
    return r ? r : y->is_canonical - x->is_canonical;
}

/* Muxer names are not unique ("matroska" is both .mkv and .mka). The one
 * av_guess_format() returns keeps its name as key; the others get
 * "name:first_extension" (or "name:N"). */
static void assign_muxer_keys(Caps *c)
{
    for (int i = 0; i < c->nb_muxers; i++) {
        CapsMuxer *m = &c->muxers[i];
        m->is_canonical = av_guess_format(m->name, NULL, NULL) == m->fmt;
        av_strlcpy(m->key, m->name, sizeof(m->key));
    }
    qsort(c->muxers, c->nb_muxers, sizeof(*c->muxers), cmp_muxer_name);

    for (int i = 1; i < c->nb_muxers; i++) {
        CapsMuxer *m = &c->muxers[i];
        const char *ext = m->extensions;

        if (strcmp(m->name, c->muxers[i - 1].name))
            continue;
        if (ext && *ext)
            snprintf(m->key, sizeof(m->key), "%s:%.*s", m->name, (int)strcspn(ext, ","), ext);
        else
            snprintf(m->key, sizeof(m->key), "%s:%d", m->name, i);
    }
    qsort(c->muxers, c->nb_muxers, sizeof(*c->muxers), cmp_muxer);
}

static int cmp_encoder(const void *a, const void *b)
{
    return strcmp(((const CapsEncoder *)a)->name, ((const CapsEncoder *)b)->name);
}

static int cmp_filter(const void *a, const void *b)
{
    return strcmp(((const CapsFilter *)a)->name, ((const CapsFilter *)b)->name);
}

static int build_muxers(Caps *c)
{
    const AVOutputFormat *of;
    void *it = NULL;
    int n = 0;

    while (av_muxer_iterate(&it))
        n++;
    if (!(c->muxers = av_calloc(n ? n : 1, sizeof(*c->muxers))))
        return AVERROR(ENOMEM);

    it = NULL;
    while ((of = av_muxer_iterate(&it)) && c->nb_muxers < n) {
        CapsMuxer *m = &c->muxers[c->nb_muxers++];

        m->fmt        = of;
        m->name       = of->name;
        m->long_name  = of->long_name ? of->long_name : "";
        m->extensions = of->extensions;
        m->mime_type  = of->mime_type;
        m->flags      = of->flags;
    }
    assign_muxer_keys(c);
    for (int i = 0; i < c->nb_muxers; i++)
        c->muxers[i].is_common = c->muxers[i].is_canonical &&
                                 in_list(common_muxers, c->muxers[i].name) > 0;
    return 0;
}

/* avcodec_get_supported_config() with a NULL context returns the codec's
 * static lists, which stay valid for the lifetime of the program. */
static void get_config(const AVCodec *codec, enum AVCodecConfig cfg,
                       const void **ptr, int *nb)
{
    *ptr = NULL;
    *nb  = 0;
    if (avcodec_get_supported_config(NULL, codec, cfg, 0, ptr, nb) < 0 || !*ptr) {
        *ptr = NULL;
        *nb  = 0;
    }
}

static void fill_encoder(CapsEncoder *e, const AVCodec *codec)
{
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codec->id);
    const void *p;

    e->codec           = codec;
    e->name            = codec->name;
    e->long_name       = codec->long_name ? codec->long_name : "";
    e->wrapper_name    = codec->wrapper_name;
    e->type            = codec->type;
    e->id              = codec->id;
    e->capabilities    = codec->capabilities;
    e->props           = desc ? desc->props : 0;
    e->is_common       = in_list(common_encoders, codec->name);
    e->is_experimental = !!(codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL);
    e->is_hardware     = !!(codec->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID)) ||
                         avcodec_get_hw_config(codec, 0) != NULL;

    if (codec->type == AVMEDIA_TYPE_VIDEO) {
        get_config(codec, AV_CODEC_CONFIG_PIX_FORMAT, &p, &e->nb_pix_fmts);
        e->pix_fmts = p;
        get_config(codec, AV_CODEC_CONFIG_FRAME_RATE, &p, &e->nb_frame_rates);
        e->frame_rates = p;
        get_config(codec, AV_CODEC_CONFIG_COLOR_RANGE, &p, &e->nb_color_ranges);
        e->color_ranges = p;
        get_config(codec, AV_CODEC_CONFIG_COLOR_SPACE, &p, &e->nb_color_spaces);
        e->color_spaces = p;
    } else if (codec->type == AVMEDIA_TYPE_AUDIO) {
        get_config(codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, &p, &e->nb_sample_fmts);
        e->sample_fmts = p;
        get_config(codec, AV_CODEC_CONFIG_SAMPLE_RATE, &p, &e->nb_sample_rates);
        e->sample_rates = p;
        get_config(codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT, &p, &e->nb_ch_layouts);
        e->ch_layouts = p;
    }
}

static int build_encoders(Caps *c)
{
    const AVCodec *codec;
    void *it = NULL;
    int n = 0;

    while ((codec = av_codec_iterate(&it)))
        n += av_codec_is_encoder(codec);
    if (!(c->encoders = av_calloc(n ? n : 1, sizeof(*c->encoders))))
        return AVERROR(ENOMEM);

    it = NULL;
    while ((codec = av_codec_iterate(&it)) && c->nb_encoders < n)
        if (av_codec_is_encoder(codec))
            fill_encoder(&c->encoders[c->nb_encoders++], codec);
    qsort(c->encoders, c->nb_encoders, sizeof(*c->encoders), cmp_encoder);
    return 0;
}

static int has_suffix(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls > lx && !strcmp(s + ls - lx, suffix);
}

/* There is no public API telling which pixel formats a filter accepts, so
 * hardware filters are recognised by flag and by naming convention. */
static int is_hw_filter(const AVFilter *f)
{
    static const char *const suffixes[] = {
        "_cuda", "_npp", "_vaapi", "_qsv", "_vulkan", "_opencl", "_d3d11",
        "_d3d12", "_videotoolbox", "_vt", "_amf", "_mediacodec", NULL
    };

    if (f->flags & AVFILTER_FLAG_HWDEVICE)
        return 1;
    if (!strncmp(f->name, "hwupload", 8) || !strcmp(f->name, "hwdownload") ||
        !strcmp(f->name, "hwmap"))
        return 1;
    for (const char *const *s = suffixes; *s; s++)
        if (has_suffix(f->name, *s))
            return 1;
    return 0;
}

/* Pads of filters with dynamic inputs/outputs only exist once the filter is
 * initialised. Create it with default options in a scratch graph and read
 * what it got (decimate: 1 input, split: 2 outputs, select: 1 output...).
 * Fails for filters that need options to initialise (movie, streamselect...). */
static int probe_default_pads(AVFilterGraph *graph, const AVFilter *f, CapsFilter *cf)
{
    AVFilterContext *ctx = avfilter_graph_alloc_filter(graph, f, NULL);
    int ret;

    if (!ctx)
        return AVERROR(ENOMEM);
    if ((ret = avfilter_init_str(ctx, NULL)) >= 0) {
        cf->nb_inputs  = ctx->nb_inputs;
        cf->nb_outputs = ctx->nb_outputs;
        cf->in_type    = ctx->nb_inputs ? avfilter_pad_get_type(ctx->input_pads, 0) : AVMEDIA_TYPE_UNKNOWN;
        cf->out_type   = ctx->nb_outputs ? avfilter_pad_get_type(ctx->output_pads, 0) : AVMEDIA_TYPE_UNKNOWN;
        cf->default_pads_probed = 1;
    }
    avfilter_free(ctx);
    return ret;
}

static void fill_filter(CapsFilter *cf, const AVFilter *f, AVFilterGraph *graph)
{
    cf->filter            = f;
    cf->name              = f->name;
    cf->description       = f->description ? f->description : "";
    cf->flags             = f->flags;
    cf->nb_inputs         = avfilter_filter_pad_count(f, 0);
    cf->nb_outputs        = avfilter_filter_pad_count(f, 1);
    cf->dynamic_inputs    = !!(f->flags & AVFILTER_FLAG_DYNAMIC_INPUTS);
    cf->dynamic_outputs   = !!(f->flags & AVFILTER_FLAG_DYNAMIC_OUTPUTS);
    cf->in_type           = cf->nb_inputs ? avfilter_pad_get_type(f->inputs, 0) : AVMEDIA_TYPE_UNKNOWN;
    cf->out_type          = cf->nb_outputs ? avfilter_pad_get_type(f->outputs, 0) : AVMEDIA_TYPE_UNKNOWN;
    cf->is_hw             = is_hw_filter(f);
    cf->supports_timeline = !!(f->flags & AVFILTER_FLAG_SUPPORT_TIMELINE);
    cf->metadata_only     = !!(f->flags & AVFILTER_FLAG_METADATA_ONLY);

    /* Not for hardware filters: initialising them opens a device
     * (libplacebo creates a Vulkan instance, ~1 s). */
    if ((cf->dynamic_inputs || cf->dynamic_outputs) && graph && !cf->is_hw)
        probe_default_pads(graph, f, cf);

    if (cf->default_pads_probed) {
        /* exact default pad counts: classify on those alone */
        if (cf->nb_inputs == 0)
            cf->cls = CAPS_FILTER_SOURCE;
        else if (cf->nb_outputs == 0)
            cf->cls = CAPS_FILTER_SINK;
        else if (cf->nb_inputs > 1)
            cf->cls = CAPS_FILTER_MULTI_IN;
        else if (cf->nb_outputs > 1)
            cf->cls = CAPS_FILTER_MULTI_OUT;
        else if (cf->is_hw)
            cf->cls = CAPS_FILTER_HW;
        else if (cf->in_type != cf->out_type)
            cf->cls = CAPS_FILTER_CONVERT;
        else
            cf->cls = CAPS_FILTER_SIMPLE;
        return;
    }

    /* static pads only, or a dynamic filter that needs options: be conservative */
    if (cf->nb_inputs == 0 && !cf->dynamic_inputs)
        cf->cls = CAPS_FILTER_SOURCE;
    else if (cf->nb_outputs == 0 && !cf->dynamic_outputs)
        cf->cls = CAPS_FILTER_SINK;
    else if (cf->is_hw && cf->nb_inputs <= 1 && cf->nb_outputs <= 1)
        cf->cls = CAPS_FILTER_HW;   /* unprobed dynamic hw filter, e.g. libplacebo */
    else if (cf->nb_inputs > 1 || cf->dynamic_inputs)
        cf->cls = CAPS_FILTER_MULTI_IN;
    else if (cf->nb_outputs > 1 || cf->dynamic_outputs)
        cf->cls = CAPS_FILTER_MULTI_OUT;
    else if (cf->is_hw)
        cf->cls = CAPS_FILTER_HW;
    else if (cf->in_type != cf->out_type)
        cf->cls = CAPS_FILTER_CONVERT;
    else
        cf->cls = CAPS_FILTER_SIMPLE;
}

static int build_filters(Caps *c)
{
    AVFilterGraph *graph = avfilter_graph_alloc();
    int log_level = av_log_get_level();
    const AVFilter *f;
    void *it = NULL;
    int n = 0;

    while (av_filter_iterate(&it))
        n++;
    if (!(c->filters = av_calloc(n ? n : 1, sizeof(*c->filters)))) {
        avfilter_graph_free(&graph);
        return AVERROR(ENOMEM);
    }

    /* filters that cannot initialise without options complain loudly */
    av_log_set_level(AV_LOG_QUIET);
    it = NULL;
    while ((f = av_filter_iterate(&it)) && c->nb_filters < n)
        fill_filter(&c->filters[c->nb_filters++], f, graph);
    av_log_set_level(log_level);

    avfilter_graph_free(&graph);
    qsort(c->filters, c->nb_filters, sizeof(*c->filters), cmp_filter);
    return 0;
}

int caps_build(Caps **out)
{
    Caps *c = av_mallocz(sizeof(*c));
    int ret;

    *out = NULL;
    if (!c)
        return AVERROR(ENOMEM);
    if ((ret = build_muxers(c)) < 0 ||
        (ret = build_encoders(c)) < 0 ||
        (ret = build_filters(c)) < 0) {
        caps_free(&c);
        return ret;
    }
    *out = c;
    return 0;
}

void caps_free(Caps **pc)
{
    Caps *c = pc ? *pc : NULL;

    if (!c)
        return;
    av_free(c->muxers);
    av_free(c->encoders);
    av_free(c->filters);
    av_freep(pc);
}

/* ------------------------------------------------------------------------- */
/* lookups                                                                   */

const CapsMuxer *caps_find_muxer(const Caps *c, const char *name)
{
    CapsMuxer key;

    av_strlcpy(key.key, name, sizeof(key.key));
    return bsearch(&key, c->muxers, c->nb_muxers, sizeof(*c->muxers), cmp_muxer);
}

const AVOutputFormat *caps_lookup_muxer(const char *key)
{
    const char *colon = strchr(key, ':');
    const AVOutputFormat *of;
    char name[64];
    void *it = NULL;

    if (!colon)
        return av_guess_format(key, NULL, NULL);

    /* "name:ext": the muxer called `name` whose first extension is `ext`
     * (same rule as assign_muxer_keys) */
    av_strlcpy(name, key, FFMIN(sizeof(name), (size_t)(colon - key) + 1));
    while ((of = av_muxer_iterate(&it))) {
        const char *ext = of->extensions;
        size_t len;

        if (strcmp(of->name, name) || !ext)
            continue;
        len = strcspn(ext, ",");
        if (strlen(colon + 1) == len && !strncmp(ext, colon + 1, len))
            return of;
    }
    return NULL;
}

const CapsEncoder *caps_find_encoder(const Caps *c, const char *name)
{
    CapsEncoder key = { .name = name };
    return bsearch(&key, c->encoders, c->nb_encoders, sizeof(*c->encoders), cmp_encoder);
}

const CapsFilter *caps_find_filter(const Caps *c, const char *name)
{
    CapsFilter key = { .name = name };
    return bsearch(&key, c->filters, c->nb_filters, sizeof(*c->filters), cmp_filter);
}

/* ------------------------------------------------------------------------- */
/* compatibility                                                             */

static enum AVCodecID default_codec(const AVOutputFormat *of, enum AVMediaType type)
{
    switch (type) {
    case AVMEDIA_TYPE_VIDEO:    return of->video_codec;
    case AVMEDIA_TYPE_AUDIO:    return of->audio_codec;
    case AVMEDIA_TYPE_SUBTITLE: return of->subtitle_codec;
    default:                    return AV_CODEC_ID_NONE;
    }
}

CapsCompat caps_mux_codec(const CapsMuxer *m, enum AVCodecID id)
{
    enum AVMediaType type;
    int r;

    if (id == AV_CODEC_ID_NONE)
        return CAPS_NO;

    r = avformat_query_codec(m->fmt, id, FF_COMPLIANCE_NORMAL);
    if (r == 1)
        return CAPS_YES;

    /* A muxer without a default codec for this media type almost never
     * accepts it (image2 even answers "yes" to anything when compliance is
     * relaxed, so this check must come before the experimental retry). */
    type = avcodec_get_type(id);
    if (default_codec(m->fmt, type) == AV_CODEC_ID_NONE)
        return CAPS_NO;

    if (r == 0)
        /* Some pairs are only allowed with -strict experimental. */
        return avformat_query_codec(m->fmt, id, FF_COMPLIANCE_EXPERIMENTAL) == 1
               ? CAPS_MAYBE : CAPS_NO;

    /* The muxer has no codec table: it only knows its default codecs. */
    return CAPS_MAYBE;
}

static int is_transcodable_type(enum AVMediaType t)
{
    return t == AVMEDIA_TYPE_VIDEO || t == AVMEDIA_TYPE_AUDIO || t == AVMEDIA_TYPE_SUBTITLE;
}

/* Subtitle encoders only accept the same kind (text or bitmap) as the input:
 * FFmpeg cannot render text to bitmaps in the encoder, and bitmap -> text
 * would need OCR. */
static int subtitle_kind_ok(const MediaStream *s, const CapsEncoder *e)
{
    if (s->type != AVMEDIA_TYPE_SUBTITLE)
        return 1;
    if (s->is_text_sub)
        return !!(e->props & AV_CODEC_PROP_TEXT_SUB);
    if (s->is_bitmap_sub)
        return !!(e->props & AV_CODEC_PROP_BITMAP_SUB);
    return 0;
}

static CapsCompat copy_compat(const CapsMuxer *m, const MediaStream *s)
{
    if (s->codec_id == AV_CODEC_ID_NONE)
        return CAPS_NO;
    if (s->type == AVMEDIA_TYPE_ATTACHMENT)
        /* Only Matroska stores attachments (fonts, cover files...). */
        return !strcmp(m->name, "matroska") ? CAPS_YES : CAPS_NO;
    return caps_mux_codec(m, s->codec_id);
}

static int stream_fits(const Caps *c, const CapsMuxer *m, const MediaStream *s)
{
    if (copy_compat(m, s) != CAPS_NO)
        return 1;
    if (!s->has_decoder)
        return 0;
    for (int i = 0; i < c->nb_encoders; i++) {
        const CapsEncoder *e = &c->encoders[i];
        if (e->type == s->type && !e->is_experimental && subtitle_kind_ok(s, e) &&
            caps_mux_codec(m, e->id) != CAPS_NO)
            return 1;
    }
    return 0;
}

static int cmp_fit(const void *a, const void *b)
{
    const CapsMuxerFit *x = a, *y = b;

    if (x->mux->is_common != y->mux->is_common)
        return y->mux->is_common - x->mux->is_common;
    if (x->nb_kept != y->nb_kept)
        return y->nb_kept - x->nb_kept;
    return strcmp(x->mux->key, y->mux->key);
}

int caps_muxers_for(const Caps *c, const MediaInfo *mi, CapsMuxerFit **out, int *nb)
{
    CapsMuxerFit *fits;
    int n = 0;

    *out = NULL;
    *nb  = 0;
    if (!(fits = av_calloc(c->nb_muxers ? c->nb_muxers : 1, sizeof(*fits))))
        return AVERROR(ENOMEM);

    for (int i = 0; i < c->nb_muxers; i++) {
        const CapsMuxer *m = &c->muxers[i];
        CapsMuxerFit fit = { .mux = m };

        for (int j = 0; j < mi->nb_streams; j++) {
            const MediaStream *s = &mi->streams[j];

            if (!is_transcodable_type(s->type))
                continue;
            fit.nb_considered++;
            if (!stream_fits(c, m, s))
                continue;
            fit.nb_kept++;
            fit.kept_video    += s->type == AVMEDIA_TYPE_VIDEO;
            fit.kept_audio    += s->type == AVMEDIA_TYPE_AUDIO;
            fit.kept_subtitle += s->type == AVMEDIA_TYPE_SUBTITLE;
        }
        if (fit.nb_kept > 0)
            fits[n++] = fit;
    }
    qsort(fits, n, sizeof(*fits), cmp_fit);
    *out = fits;
    *nb  = n;
    return 0;
}

static int cmp_choice(const void *a, const void *b)
{
    const CapsEncChoice *x = a, *y = b;

    if (x->compat != y->compat)
        return (int)y->compat - (int)x->compat;
    /* common encoders first, in preference order */
    if (x->enc->is_common != y->enc->is_common) {
        if (!x->enc->is_common || !y->enc->is_common)
            return x->enc->is_common ? -1 : 1;
        return x->enc->is_common - y->enc->is_common;
    }
    /* software first: hardware encoders may not work on this machine */
    if (x->enc->is_hardware != y->enc->is_hardware)
        return x->enc->is_hardware - y->enc->is_hardware;
    return strcmp(x->enc->name, y->enc->name);
}

int caps_stream_actions(const Caps *c, const CapsMuxer *m, const MediaStream *s,
                        int include_experimental, CapsStreamActions *out)
{
    int n = 0;

    memset(out, 0, sizeof(*out));
    out->copy = copy_compat(m, s);

    if (!is_transcodable_type(s->type)) {
        out->note = "only stream copy is possible for this stream type";
        return 0;
    }
    if (!s->has_decoder) {
        out->note = "no decoder for this codec in the FFmpeg build: copy or drop only";
        return 0;
    }
    if (s->type == AVMEDIA_TYPE_SUBTITLE && !s->is_text_sub && !s->is_bitmap_sub) {
        out->note = "unknown subtitle kind: copy or drop only";
        return 0;
    }
    out->can_transcode = 1;

    if (!(out->encoders = av_calloc(c->nb_encoders ? c->nb_encoders : 1, sizeof(*out->encoders))))
        return AVERROR(ENOMEM);

    for (int i = 0; i < c->nb_encoders; i++) {
        const CapsEncoder *e = &c->encoders[i];
        CapsCompat compat;

        if (e->type != s->type || !subtitle_kind_ok(s, e))
            continue;
        if (e->is_experimental && !include_experimental)
            continue;
        compat = caps_mux_codec(m, e->id);
        if (compat == CAPS_NO)
            continue;
        out->encoders[n].enc    = e;
        out->encoders[n].compat = compat;
        n++;
    }
    qsort(out->encoders, n, sizeof(*out->encoders), cmp_choice);
    out->nb_encoders = n;

    if (!n)
        out->note = s->type == AVMEDIA_TYPE_SUBTITLE
            ? "no subtitle encoder of the same kind (text/bitmap) fits this container"
            : "no encoder for this stream type fits this container";
    return 0;
}

void caps_stream_actions_free(CapsStreamActions *a)
{
    if (!a)
        return;
    av_freep(&a->encoders);
    a->nb_encoders = 0;
}

/* ------------------------------------------------------------------------- */
/* filters                                                                   */

static int filter_matches_type(const CapsFilter *f, enum AVMediaType type)
{
    if (type == AVMEDIA_TYPE_UNKNOWN)
        return 1;
    if (f->cls == CAPS_FILTER_SOURCE)
        return f->out_type == type;
    /* unprobed dynamic-input filters have no known input pad: use the output */
    return f->in_type == type || (f->in_type == AVMEDIA_TYPE_UNKNOWN && f->out_type == type);
}

int caps_filters_for(const Caps *c, enum AVMediaType type, unsigned class_mask,
                     const CapsFilter ***out, int *nb)
{
    const CapsFilter **list;
    int n = 0;

    *out = NULL;
    *nb  = 0;
    if (!(list = av_calloc(c->nb_filters ? c->nb_filters : 1, sizeof(*list))))
        return AVERROR(ENOMEM);

    for (int i = 0; i < c->nb_filters; i++) {
        const CapsFilter *f = &c->filters[i];
        if ((f->cls & class_mask) && filter_matches_type(f, type))
            list[n++] = f;
    }
    *out = list;
    *nb  = n;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* hardware                                                                  */

int caps_probe_hw(CapsHwDevice **out, int *nb)
{
    enum AVHWDeviceType t = AV_HWDEVICE_TYPE_NONE;
    CapsHwDevice *devs;
    int n = 0, i = 0;

    *out = NULL;
    *nb  = 0;
    while ((t = av_hwdevice_iterate_types(t)) != AV_HWDEVICE_TYPE_NONE)
        n++;
    if (!(devs = av_calloc(n ? n : 1, sizeof(*devs))))
        return AVERROR(ENOMEM);

    while (i < n && (t = av_hwdevice_iterate_types(t)) != AV_HWDEVICE_TYPE_NONE) {
        AVBufferRef *ref = NULL;
        int ret = av_hwdevice_ctx_create(&ref, t, NULL, NULL, 0);

        devs[i].type      = t;
        devs[i].name      = av_hwdevice_get_type_name(t);
        devs[i].available = ret >= 0;
        if (ret < 0)
            av_strerror(ret, devs[i].error, sizeof(devs[i].error));
        av_buffer_unref(&ref);
        i++;
    }
    *out = devs;
    *nb  = i;
    return 0;
}

/* ------------------------------------------------------------------------- */

const char *caps_compat_name(CapsCompat c)
{
    switch (c) {
    case CAPS_YES:   return "yes";
    case CAPS_MAYBE: return "maybe";
    default:         return "no";
    }
}

const char *caps_filter_class_name(CapsFilterClass cls)
{
    switch (cls) {
    case CAPS_FILTER_SIMPLE:    return "simple";
    case CAPS_FILTER_HW:        return "hw";
    case CAPS_FILTER_CONVERT:   return "convert";
    case CAPS_FILTER_MULTI_IN:  return "multi-in";
    case CAPS_FILTER_MULTI_OUT: return "multi-out";
    case CAPS_FILTER_SOURCE:    return "source";
    case CAPS_FILTER_SINK:      return "sink";
    default:                    return "?";
    }
}

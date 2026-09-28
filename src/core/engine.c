/*
 * engine.c - run a ConvJob (software pipeline).
 *
 * Per output stream:
 *   copy:      demux -> (rescale timestamps) -> mux
 *   transcode: demux -> decode -> buffer -> user chain -> format tail -> buffersink
 *              -> encode -> mux                          (audio / video)
 *              demux -> decode -> encode -> mux           (subtitles)
 *
 * Filter graphs are configured before encoders are opened, and encoders take
 * their parameters from the buffersink, so size/format/rate changes made by
 * the user's filters reach the encoder. The "format tail" appended to each
 * chain lists the encoder's supported formats, so lavfi negotiates a format
 * the encoder accepts and inserts conversions automatically. If the decoded
 * frames later change size/format, the graph is rebuilt with an exact tail
 * matching the already-open encoder.
 */
#include "engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#include "caps.h"

#define SUB_BUF_SIZE         (1 << 20)
#define PROGRESS_INTERVAL_US 250000

typedef struct OutStream {
    const JobStream *js;
    int              job_index;      /* position in job->streams, for messages */
    AVStream        *in_st;
    AVStream        *out_st;
    enum AVMediaType type;

    /* transcoding */
    AVCodecContext  *dec;
    AVCodecContext  *enc;
    const AVCodec   *encoder;
    char            *chain;          /* user filter chain, "" if none */
    AVFilterGraph   *graph;
    AVFilterContext *src;
    AVFilterContext *sink;
    AVRational       src_tb;         /* time base of frames fed to the graph */
    int              cfg_w, cfg_h, cfg_fmt, cfg_rate;  /* current graph input */
    AVChannelLayout  cfg_layout;
    int64_t          next_pts;       /* sink time base, for frames without pts */
    int64_t          audio_next_pts; /* src_tb: where the last audio frame ended */
    int64_t          last_enc_pts;   /* video: last pts sent to the encoder */
    uint8_t         *sub_buf;

    AVFrame         *frame;
    AVFrame         *filt_frame;
    AVPacket        *pkt;
} OutStream;

typedef struct Engine {
    const ConvJob   *job;
    EngineCallbacks  cb;
    atomic_int      *cancel;
    EngineStats      stats;
    char            *err;
    size_t           errlen;

    AVFormatContext *ifmt;
    AVFormatContext *ofmt;
    OutStream       *os;
    int              nb_os;
    AVPacket        *pkt;
    int              output_opened;
    int              header_written;

    int64_t          t0;
    int64_t          last_progress;
    double           in_start;       /* seconds */
    double           duration;       /* seconds, 0 if unknown */
    double           out_time;       /* seconds written, relative to in_start */
    int64_t          frames;
} Engine;

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */

/* Records the first error only: later failures are usually consequences. */
static int fail(Engine *e, int ret, const char *fmt, ...)
{
    char msg[512];
    va_list ap;

    if (e->err && e->errlen && !e->err[0]) {
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        if (ret < 0)
            snprintf(e->err, e->errlen, "%s: %s", msg, av_err2str(ret));
        else
            snprintf(e->err, e->errlen, "%s", msg);
    }
    return ret < 0 ? ret : AVERROR(EINVAL);
}

static int cancelled(const Engine *e)
{
    return e->cancel && atomic_load(e->cancel);
}

static int interrupt_cb(void *opaque)
{
    return cancelled(opaque);
}

static void report(Engine *e, int final)
{
    EngineProgress p = { 0 };
    int64_t now = av_gettime_relative();

    if (!e->cb.on_progress || (!final && now - e->last_progress < PROGRESS_INTERVAL_US))
        return;
    e->last_progress = now;

    p.elapsed   = (now - e->t0) / 1e6;
    p.out_time  = FFMAX(e->out_time, 0);
    p.duration  = e->duration;
    p.percent   = e->duration > 0 ? FFMIN(100.0, 100.0 * p.out_time / e->duration) : -1;
    p.frames    = e->frames;
    p.fps       = p.elapsed > 0 ? e->frames / p.elapsed : 0;
    p.speed     = p.elapsed > 0 ? p.out_time / p.elapsed : 0;
    /* avio_size, not avio_tell: +faststart rewrites the file at the end */
    p.out_bytes = e->ofmt && e->ofmt->pb ? FFMAX(avio_size(e->ofmt->pb), avio_tell(e->ofmt->pb)) : 0;
    p.finished  = final;
    e->cb.on_progress(&p, e->cb.opaque);
}

static int same_path(const char *a, const char *b)
{
#ifdef _WIN32
    return !av_strcasecmp(a, b);
#else
    return !strcmp(a, b);
#endif
}

static void delete_file(const char *path)
{
    const char *proto = avio_find_protocol_name(path);

    if (!proto || strcmp(proto, "file"))
        return;
    av_strstart(path, "file:", &path);
#ifdef _WIN32
    {
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

/* Matroska statistics tags describe the old encoding: drop them when the
 * stream is re-encoded (the muxer writes fresh ones). */
static int is_stale_tag(const char *key)
{
    static const char *const prefixes[] = {
        "DURATION", "BPS", "NUMBER_OF_FRAMES", "NUMBER_OF_BYTES",
        "_STATISTICS_", "ENCODER", NULL
    };
    for (const char *const *p = prefixes; *p; p++)
        if (!av_strncasecmp(key, *p, strlen(*p)))
            return 1;
    return 0;
}

static int apply_overrides(AVDictionary **dst, const AVDictionary *overrides)
{
    const AVDictionaryEntry *e = NULL;
    int ret;

    while ((e = av_dict_iterate(overrides, e)))
        if ((ret = av_dict_set(dst, e->key, *e->value ? e->value : NULL, 0)) < 0)
            return ret;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* filter graphs                                                             */

static void append_video_tail(OutStream *os, AVBPrint *bp, int exact)
{
    const enum AVPixelFormat *fmts = NULL;
    int nb = 0, first = 1;

    if (exact) {
        av_bprintf(bp, "scale=%d:%d,format=pix_fmts=%s", os->enc->width, os->enc->height,
                   av_get_pix_fmt_name(os->enc->pix_fmt));
        return;
    }
    avcodec_get_supported_config(NULL, os->encoder, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                 (const void **)&fmts, &nb);
    for (int i = 0; fmts && i < nb; i++) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fmts[i]);
        /* software pipeline: hardware surfaces cannot come out of it */
        if (!d || (d->flags & AV_PIX_FMT_FLAG_HWACCEL))
            continue;
        av_bprintf(bp, "%s%s", first ? "format=pix_fmts=" : "|", d->name);
        first = 0;
    }
}

static int describe_layout(const AVChannelLayout *l, char *buf, size_t size)
{
    /* custom-order layouts describe as "3 channels (FL+LFE+FR)", which the
     * aformat option parser does not accept */
    return av_channel_layout_describe(l, buf, size) >= 0 && !strchr(buf, ' ');
}

static void append_audio_tail(OutStream *os, AVBPrint *bp, int exact)
{
    const enum AVSampleFormat *fmts = NULL;
    const int *rates = NULL;
    const AVChannelLayout *layouts = NULL;
    int nb_fmts = 0, nb_rates = 0, nb_layouts = 0, parts = 0;
    char buf[128];

    if (exact) {
        av_bprintf(bp, "aresample=%d,aformat=sample_fmts=%s", os->enc->sample_rate,
                   av_get_sample_fmt_name(os->enc->sample_fmt));
        if (describe_layout(&os->enc->ch_layout, buf, sizeof(buf)))
            av_bprintf(bp, ":channel_layouts=%s", buf);
        return;
    }

    avcodec_get_supported_config(NULL, os->encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                 (const void **)&fmts, &nb_fmts);
    avcodec_get_supported_config(NULL, os->encoder, AV_CODEC_CONFIG_SAMPLE_RATE, 0,
                                 (const void **)&rates, &nb_rates);
    avcodec_get_supported_config(NULL, os->encoder, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0,
                                 (const void **)&layouts, &nb_layouts);
    if (!fmts && !rates && !layouts)
        return;

    av_bprintf(bp, "aformat=");
    if (fmts && nb_fmts) {
        av_bprintf(bp, "sample_fmts=");
        for (int i = 0; i < nb_fmts; i++)
            av_bprintf(bp, "%s%s", i ? "|" : "", av_get_sample_fmt_name(fmts[i]));
        parts++;
    }
    if (rates && nb_rates) {
        av_bprintf(bp, "%ssample_rates=", parts++ ? ":" : "");
        for (int i = 0; i < nb_rates; i++)
            av_bprintf(bp, "%s%d", i ? "|" : "", rates[i]);
    }
    if (layouts && nb_layouts) {
        int first = 1;
        for (int i = 0; i < nb_layouts; i++) {
            if (!describe_layout(&layouts[i], buf, sizeof(buf)))
                continue;
            av_bprintf(bp, "%s%s", first ? (parts ? ":channel_layouts=" : "channel_layouts=") : "|", buf);
            first = 0;
        }
    }
}

/* Build (or rebuild) the stream's graph. With frame == NULL the input
 * parameters come from the decoder; exact = 1 forces the tail to the open
 * encoder's exact parameters. */
static int build_graph(Engine *e, OutStream *os, const AVFrame *frame, int exact)
{
    const int video = os->type == AVMEDIA_TYPE_VIDEO;
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterContext *src = NULL, *sink = NULL;
    AVFilterInOut *inputs = NULL, *outputs = NULL;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    AVBPrint desc;
    int ret;

    av_bprint_init(&desc, 0, AV_BPRINT_SIZE_UNLIMITED);
    if (!graph || !par) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (video) {
        par->format              = frame ? frame->format : os->dec->pix_fmt;
        par->width               = frame ? frame->width : os->dec->width;
        par->height              = frame ? frame->height : os->dec->height;
        par->sample_aspect_ratio = frame ? frame->sample_aspect_ratio
                                         : av_guess_sample_aspect_ratio(e->ifmt, os->in_st, NULL);
        par->frame_rate          = av_guess_frame_rate(e->ifmt, os->in_st, NULL);
        par->color_space         = frame ? frame->colorspace : os->dec->colorspace;
        par->color_range         = frame ? frame->color_range : os->dec->color_range;
        par->time_base           = os->in_st->time_base;
        if (par->format == AV_PIX_FMT_NONE || par->width <= 0 || par->height <= 0) {
            ret = fail(e, AVERROR(EINVAL), "stream #%d: input video format unknown", os->job_index);
            goto end;
        }
    } else {
        par->format      = frame ? frame->format : os->dec->sample_fmt;
        par->sample_rate = frame ? frame->sample_rate : os->dec->sample_rate;
        /* 1/sample_rate keeps audio timestamps sample-accurate */
        par->time_base   = (AVRational){ 1, par->sample_rate };
        ret = av_channel_layout_copy(&par->ch_layout, frame ? &frame->ch_layout : &os->dec->ch_layout);
        if (ret < 0)
            goto end;
        if (par->format == AV_SAMPLE_FMT_NONE || par->sample_rate <= 0 || !par->ch_layout.nb_channels) {
            ret = fail(e, AVERROR(EINVAL), "stream #%d: input audio format unknown", os->job_index);
            goto end;
        }
    }

    src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name(video ? "buffer" : "abuffer"), "in");
    if (!src) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((ret = av_buffersrc_parameters_set(src, par)) < 0 ||
        (ret = avfilter_init_str(src, NULL)) < 0 ||
        (ret = avfilter_graph_create_filter(&sink, avfilter_get_by_name(video ? "buffersink" : "abuffersink"),
                                            "out", NULL, NULL, graph)) < 0) {
        ret = fail(e, ret, "stream #%d: cannot create filter graph endpoints", os->job_index);
        goto end;
    }

    av_bprintf(&desc, "%s", os->chain[0] ? os->chain : (video ? "null" : "anull"));
    {
        AVBPrint tail;
        av_bprint_init(&tail, 0, AV_BPRINT_SIZE_UNLIMITED);
        if (video)
            append_video_tail(os, &tail, exact);
        else
            append_audio_tail(os, &tail, exact);
        if (tail.len)
            av_bprintf(&desc, ",%s", tail.str);
        av_bprint_finalize(&tail, NULL);
    }
    if (!av_bprint_is_complete(&desc)) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    outputs = avfilter_inout_alloc();
    inputs  = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = src;
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = sink;

    if ((ret = avfilter_graph_parse_ptr(graph, desc.str, &inputs, &outputs, NULL)) < 0) {
        ret = fail(e, ret, "stream #%d: invalid filter chain \"%s\"", os->job_index, os->chain);
        goto end;
    }
    if ((ret = avfilter_graph_config(graph, NULL)) < 0) {
        ret = fail(e, ret, "stream #%d: cannot configure filters \"%s\"", os->job_index, desc.str);
        goto end;
    }

    /* fixed-frame-size audio encoders (aac: 1024, opus: 960...) */
    if (!video && os->enc && !(os->encoder->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) &&
        os->enc->frame_size > 0)
        av_buffersink_set_frame_size(sink, os->enc->frame_size);

    avfilter_graph_free(&os->graph);
    os->graph   = graph;
    os->src     = src;
    os->sink    = sink;
    if (av_cmp_q(os->src_tb, par->time_base))
        os->audio_next_pts = AV_NOPTS_VALUE;   /* prediction was in the old time base */
    os->src_tb  = par->time_base;
    os->cfg_fmt = par->format;
    os->cfg_w   = par->width;
    os->cfg_h   = par->height;
    os->cfg_rate = par->sample_rate;
    av_channel_layout_uninit(&os->cfg_layout);
    ret = av_channel_layout_copy(&os->cfg_layout, &par->ch_layout);
    graph = NULL;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&graph);
    if (par)
        av_channel_layout_uninit(&par->ch_layout);
    av_free(par);
    av_bprint_finalize(&desc, NULL);
    return ret;
}

static int params_changed(const OutStream *os, const AVFrame *f)
{
    if (os->type == AVMEDIA_TYPE_VIDEO)
        return f->width != os->cfg_w || f->height != os->cfg_h || f->format != os->cfg_fmt;
    return f->sample_rate != os->cfg_rate || f->format != os->cfg_fmt ||
           av_channel_layout_compare(&f->ch_layout, &os->cfg_layout);
}

/* ------------------------------------------------------------------------- */
/* encoders                                                                  */

static int try_open_encoder(Engine *e, OutStream *os, AVRational tb, AVRational frame_rate)
{
    AVCodecContext *enc = avcodec_alloc_context3(os->encoder);
    AVDictionary *opts = NULL;
    const AVDictionaryEntry *left;
    int ret;

    if (!enc)
        return AVERROR(ENOMEM);

    switch (os->type) {
    case AVMEDIA_TYPE_VIDEO:
        enc->width                  = av_buffersink_get_w(os->sink);
        enc->height                 = av_buffersink_get_h(os->sink);
        enc->pix_fmt                = av_buffersink_get_format(os->sink);
        enc->sample_aspect_ratio    = av_buffersink_get_sample_aspect_ratio(os->sink);
        enc->framerate              = frame_rate;
        enc->time_base              = tb;
        enc->colorspace             = av_buffersink_get_colorspace(os->sink);
        enc->color_range            = av_buffersink_get_color_range(os->sink);
        enc->color_primaries        = os->dec->color_primaries;
        enc->color_trc              = os->dec->color_trc;
        enc->chroma_sample_location = os->dec->chroma_sample_location;
        break;
    case AVMEDIA_TYPE_AUDIO:
        enc->sample_fmt  = av_buffersink_get_format(os->sink);
        enc->sample_rate = av_buffersink_get_sample_rate(os->sink);
        enc->time_base   = (AVRational){ 1, enc->sample_rate };
        if ((ret = av_buffersink_get_ch_layout(os->sink, &enc->ch_layout)) < 0)
            goto end;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        enc->time_base = AV_TIME_BASE_Q;
        enc->width     = os->dec->width;
        enc->height    = os->dec->height;
        if (os->dec->subtitle_header) {
            /* text encoders need the ASS header the decoder generated */
            enc->subtitle_header = av_mallocz(os->dec->subtitle_header_size + 1);
            if (!enc->subtitle_header) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            memcpy(enc->subtitle_header, os->dec->subtitle_header, os->dec->subtitle_header_size);
            enc->subtitle_header_size = os->dec->subtitle_header_size;
        }
        break;
    default:
        break;
    }

    if (e->ofmt->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (os->encoder->capabilities & AV_CODEC_CAP_EXPERIMENTAL)
        enc->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL; /* chosen explicitly */

    if ((ret = av_dict_copy(&opts, os->js->encoder_options, 0)) < 0)
        goto end;
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);

    if ((ret = avcodec_open2(enc, os->encoder, &opts)) < 0)
        goto end;
    if ((left = av_dict_iterate(opts, NULL))) {
        ret = fail(e, AVERROR_OPTION_NOT_FOUND, "stream #%d: encoder %s has no option '%s'",
                   os->job_index, os->encoder->name, left->key);
        goto end;
    }
    os->enc = enc;
    enc = NULL;

end:
    av_dict_free(&opts);
    avcodec_free_context(&enc);
    return ret;
}

static int open_encoder(Engine *e, OutStream *os)
{
    AVRational tb = AV_TIME_BASE_Q, fr = { 0, 1 };
    int ret;

    if (os->type == AVMEDIA_TYPE_VIDEO) {
        AVRational in_avg = os->in_st->avg_frame_rate, in_r = os->in_st->r_frame_rate;

        fr = av_buffersink_get_frame_rate(os->sink);
        if (fr.num <= 0 || fr.den <= 0)
            fr = av_guess_frame_rate(e->ifmt, os->in_st, NULL);
        tb = av_buffersink_get_time_base(os->sink);
        /* constant-rate input (or an fps filter): 1/fps, what most encoders
         * and players expect; otherwise keep the exact timestamps */
        if (fr.num > 0 && fr.den > 0 &&
            ((in_avg.num > 0 && !av_cmp_q(in_avg, in_r)) || strstr(os->chain, "fps")))
            tb = av_inv_q(fr);
    }

    ret = try_open_encoder(e, os, tb, fr);
    /* some encoders (mpeg4, mpeg2video...) reject large time base
     * denominators: retry with 1/fps */
    if (ret < 0 && ret != AVERROR_OPTION_NOT_FOUND && os->type == AVMEDIA_TYPE_VIDEO &&
        fr.num > 0 && av_cmp_q(tb, av_inv_q(fr)))
        ret = try_open_encoder(e, os, av_inv_q(fr), fr);
    if (ret < 0)
        return fail(e, ret, "stream #%d: cannot open encoder %s", os->job_index, os->encoder->name);

    if (os->type == AVMEDIA_TYPE_AUDIO &&
        !(os->encoder->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) && os->enc->frame_size > 0)
        av_buffersink_set_frame_size(os->sink, os->enc->frame_size);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* packet flow                                                               */

static int write_packet(Engine *e, OutStream *os, AVPacket *pkt, AVRational tb)
{
    int64_t ts;
    int ret;

    pkt->stream_index = os->out_st->index;
    pkt->pos          = -1;
    av_packet_rescale_ts(pkt, tb, os->out_st->time_base);

    ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (ts != AV_NOPTS_VALUE) {
        double t = ts * av_q2d(os->out_st->time_base) - e->in_start;
        if (t > e->out_time)
            e->out_time = t;
    }
    if ((ret = av_interleaved_write_frame(e->ofmt, pkt)) < 0)
        return fail(e, ret, "stream #%d: error writing packet", os->job_index);
    return 0;
}

static int encode_frame(Engine *e, OutStream *os, AVFrame *frame)
{
    int ret;

    if (frame) {
        AVRational stb = av_buffersink_get_time_base(os->sink);
        int64_t dur = frame->duration > 0 ? frame->duration
                    : os->type == AVMEDIA_TYPE_AUDIO
                        ? av_rescale_q(frame->nb_samples, (AVRational){ 1, frame->sample_rate }, stb)
                        : 1;

        if (frame->pts == AV_NOPTS_VALUE)
            frame->pts = os->next_pts;
        os->next_pts = frame->pts + dur;

        frame->pts       = av_rescale_q(frame->pts, stb, os->enc->time_base);
        frame->duration  = av_rescale_q(dur, stb, os->enc->time_base);
        frame->time_base = os->enc->time_base;
        /* don't let the decoder's picture types force keyframes */
        frame->pict_type = AV_PICTURE_TYPE_NONE;

        if (os->type == AVMEDIA_TYPE_VIDEO) {
            if (os->last_enc_pts != AV_NOPTS_VALUE && frame->pts <= os->last_enc_pts) {
                e->stats.dropped_frames++;
                return 0;
            }
            os->last_enc_pts = frame->pts;
            e->frames++;
        }
    }

    ret = avcodec_send_frame(os->enc, frame);
    if (ret < 0 && ret != AVERROR_EOF)
        return fail(e, ret, "stream #%d: error encoding with %s", os->job_index, os->encoder->name);

    for (;;) {
        ret = avcodec_receive_packet(os->enc, os->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return fail(e, ret, "stream #%d: error encoding with %s", os->job_index, os->encoder->name);
        if ((ret = write_packet(e, os, os->pkt, os->enc->time_base)) < 0)
            return ret;
    }
}

static int drain_sink(Engine *e, OutStream *os, int flush_encoder)
{
    int ret;

    for (;;) {
        ret = av_buffersink_get_frame(os->sink, os->filt_frame);
        if (ret == AVERROR(EAGAIN))
            return 0;
        if (ret == AVERROR_EOF)
            return flush_encoder ? encode_frame(e, os, NULL) : 0;
        if (ret < 0)
            return fail(e, ret, "stream #%d: error filtering", os->job_index);
        ret = encode_frame(e, os, os->filt_frame);
        av_frame_unref(os->filt_frame);
        if (ret < 0)
            return ret;
    }
}

/* frame == NULL: end of stream */
static int filter_frame(Engine *e, OutStream *os, AVFrame *frame)
{
    int ret;

    if (frame && params_changed(os, frame)) {
        /* finish the old graph, then rebuild it for the new input while
         * keeping the encoder's parameters */
        if ((ret = av_buffersrc_add_frame(os->src, NULL)) < 0 ||
            (ret = drain_sink(e, os, 0)) < 0 ||
            (ret = build_graph(e, os, frame, 1)) < 0)
            return ret;
        e->stats.graph_reinits++;
    }
    if (frame && frame->pts != AV_NOPTS_VALUE)
        frame->pts = av_rescale_q(frame->pts, os->in_st->time_base, os->src_tb);

    if (frame && os->type == AVMEDIA_TYPE_AUDIO) {
        /* Containers round timestamps (Matroska: 1 ms = 48 samples at
         * 48 kHz). Keep audio contiguous: if a frame starts within one input
         * tick of where the previous one ended, use the predicted position;
         * larger differences are real gaps and resync. */
        int64_t tolerance = av_rescale_q_rnd(1, os->in_st->time_base, os->src_tb, AV_ROUND_UP);

        if (frame->pts == AV_NOPTS_VALUE ||
            (os->audio_next_pts != AV_NOPTS_VALUE && llabs(frame->pts - os->audio_next_pts) <= tolerance))
            frame->pts = os->audio_next_pts;
        if (frame->pts != AV_NOPTS_VALUE)
            os->audio_next_pts = frame->pts + frame->nb_samples;
    }

    ret = av_buffersrc_add_frame_flags(os->src, frame, frame ? AV_BUFFERSRC_FLAG_KEEP_REF : 0);
    if (ret < 0)
        return fail(e, ret, "stream #%d: error feeding the filter graph", os->job_index);
    return drain_sink(e, os, frame == NULL);
}

/* pkt == NULL: flush the decoder */
static int decode_packet(Engine *e, OutStream *os, const AVPacket *pkt)
{
    int ret = avcodec_send_packet(os->dec, pkt);

    if (ret < 0 && ret != AVERROR_EOF) {
        if (ret == AVERROR(ENOMEM))
            return fail(e, ret, "stream #%d: decoding", os->job_index);
        e->stats.decode_errors++;   /* corrupt packet: skip it, like ffmpeg does */
        if (pkt)
            return 0;
    }

    for (;;) {
        ret = avcodec_receive_frame(os->dec, os->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret == AVERROR_INVALIDDATA) {
            e->stats.decode_errors++;
            continue;
        }
        if (ret < 0)
            return fail(e, ret, "stream #%d: decoding", os->job_index);

        os->frame->pts = os->frame->best_effort_timestamp;
        ret = filter_frame(e, os, os->frame);
        av_frame_unref(os->frame);
        if (ret < 0)
            return ret;
    }
    return pkt ? 0 : filter_frame(e, os, NULL);
}

static int transcode_subtitle(Engine *e, OutStream *os, const AVPacket *pkt)
{
    const AVRational ms = { 1, 1000 };
    AVSubtitle sub;
    int got = 0, size, ret = 0;

    if (avcodec_decode_subtitle2(os->dec, &sub, &got, pkt) < 0) {
        e->stats.decode_errors++;
        return 0;
    }
    if (!got)
        return 0;

    if (sub.pts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE)
        sub.pts = av_rescale_q(pkt->pts, os->in_st->time_base, AV_TIME_BASE_Q);
    if (sub.pts == AV_NOPTS_VALUE || !sub.num_rects)
        goto end;
    if ((!sub.end_display_time || sub.end_display_time == UINT32_MAX) && pkt->duration > 0)
        sub.end_display_time = sub.start_display_time +
                               av_rescale_q(pkt->duration, os->in_st->time_base, ms);
    /* normalise: the event starts at pts, display times relative to it */
    sub.pts += av_rescale_q(sub.start_display_time, ms, AV_TIME_BASE_Q);
    sub.end_display_time -= sub.start_display_time;
    sub.start_display_time = 0;

    size = avcodec_encode_subtitle(os->enc, os->sub_buf, SUB_BUF_SIZE, &sub);
    if (size < 0) {
        ret = fail(e, size, "stream #%d: error encoding subtitle with %s", os->job_index, os->encoder->name);
        goto end;
    }
    if (!size)
        goto end;
    if ((ret = av_new_packet(os->pkt, size)) < 0)
        goto end;
    memcpy(os->pkt->data, os->sub_buf, size);
    os->pkt->pts      = sub.pts;
    os->pkt->dts      = sub.pts;
    os->pkt->duration = av_rescale_q(sub.end_display_time, ms, AV_TIME_BASE_Q);
    ret = write_packet(e, os, os->pkt, AV_TIME_BASE_Q);

end:
    avsubtitle_free(&sub);
    return ret;
}

static int process_packet(Engine *e, OutStream *os, const AVPacket *pkt)
{
    int ret;

    switch (os->js->action) {
    case JOB_COPY:
        if ((ret = av_packet_ref(os->pkt, pkt)) < 0)
            return ret;
        return write_packet(e, os, os->pkt, os->in_st->time_base);
    case JOB_TRANSCODE:
        if (os->type == AVMEDIA_TYPE_SUBTITLE)
            return transcode_subtitle(e, os, pkt);
        return decode_packet(e, os, pkt);
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* setup                                                                     */

static int copy_stream_metadata(OutStream *os)
{
    const AVDictionaryEntry *t = NULL;
    int ret;

    if (os->js->action == JOB_TRANSCODE) {
        while ((t = av_dict_iterate(os->in_st->metadata, t)))
            if (!is_stale_tag(t->key) && (ret = av_dict_set(&os->out_st->metadata, t->key, t->value, 0)) < 0)
                return ret;
    } else if ((ret = av_dict_copy(&os->out_st->metadata, os->in_st->metadata, 0)) < 0) {
        return ret;
    }
    return apply_overrides(&os->out_st->metadata, os->js->metadata);
}

static int setup_copy(Engine *e, OutStream *os)
{
    int ret;

    if (!(os->out_st = avformat_new_stream(e->ofmt, NULL)))
        return AVERROR(ENOMEM);
    if ((ret = avcodec_parameters_copy(os->out_st->codecpar, os->in_st->codecpar)) < 0)
        return ret;
    /* the input container's fourcc may mean nothing in the output one */
    os->out_st->codecpar->codec_tag   = 0;
    os->out_st->time_base             = os->in_st->time_base;
    os->out_st->avg_frame_rate        = os->in_st->avg_frame_rate;
    os->out_st->sample_aspect_ratio   = os->in_st->sample_aspect_ratio;
    return 0;
}

static int open_decoder(Engine *e, OutStream *os)
{
    const AVCodec *dc = avcodec_find_decoder(os->in_st->codecpar->codec_id);
    AVDictionary *opts = NULL;
    int ret;

    if (!dc)
        return fail(e, AVERROR_DECODER_NOT_FOUND, "stream #%d: no decoder for %s", os->job_index,
                    avcodec_get_name(os->in_st->codecpar->codec_id));
    if (!(os->dec = avcodec_alloc_context3(dc)))
        return AVERROR(ENOMEM);
    if ((ret = avcodec_parameters_to_context(os->dec, os->in_st->codecpar)) < 0)
        return ret;
    os->dec->pkt_timebase = os->in_st->time_base;
    if (os->type == AVMEDIA_TYPE_VIDEO)
        os->dec->framerate = av_guess_frame_rate(e->ifmt, os->in_st, NULL);
    av_dict_set(&opts, "threads", "auto", 0);
    ret = avcodec_open2(os->dec, dc, &opts);
    av_dict_free(&opts);
    if (ret < 0)
        return fail(e, ret, "stream #%d: cannot open decoder %s", os->job_index, dc->name);
    return 0;
}

static int setup_transcode(Engine *e, OutStream *os)
{
    const AVPacketSideData *sd;
    AVBPrint bp;
    int ret;

    if (os->type != AVMEDIA_TYPE_VIDEO && os->type != AVMEDIA_TYPE_AUDIO &&
        os->type != AVMEDIA_TYPE_SUBTITLE)
        return fail(e, AVERROR(EINVAL), "stream #%d: %s streams can only be copied", os->job_index,
                    av_get_media_type_string(os->type));

    if (!(os->encoder = avcodec_find_encoder_by_name(os->js->encoder)))
        return fail(e, AVERROR_ENCODER_NOT_FOUND, "stream #%d: unknown encoder '%s'", os->job_index,
                    os->js->encoder);
    if (os->encoder->type != os->type)
        return fail(e, AVERROR(EINVAL), "stream #%d: %s is a %s encoder but the input stream is %s",
                    os->job_index, os->encoder->name, av_get_media_type_string(os->encoder->type),
                    av_get_media_type_string(os->type));

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    job_stream_filter_string(os->js, &bp);
    ret = av_bprint_finalize(&bp, &os->chain);
    if (ret < 0)
        return ret;
    if (os->type == AVMEDIA_TYPE_SUBTITLE && os->chain[0])
        return fail(e, AVERROR(EINVAL), "stream #%d: subtitle streams cannot be filtered", os->job_index);

    if ((ret = open_decoder(e, os)) < 0)
        return ret;

    if (os->type == AVMEDIA_TYPE_SUBTITLE) {
        if (!(os->sub_buf = av_malloc(SUB_BUF_SIZE)))
            return AVERROR(ENOMEM);
    } else if ((ret = build_graph(e, os, NULL, 0)) < 0) {
        return ret;
    }
    if ((ret = open_encoder(e, os)) < 0)
        return ret;

    if (!(os->out_st = avformat_new_stream(e->ofmt, NULL)))
        return AVERROR(ENOMEM);
    if ((ret = avcodec_parameters_from_context(os->out_st->codecpar, os->enc)) < 0)
        return ret;
    os->out_st->time_base = os->enc->time_base;
    if (os->type == AVMEDIA_TYPE_VIDEO) {
        os->out_st->avg_frame_rate      = os->enc->framerate;
        os->out_st->sample_aspect_ratio = os->enc->sample_aspect_ratio;
        /* keep the rotation flag: decoders do not rotate pixels */
        sd = av_packet_side_data_get(os->in_st->codecpar->coded_side_data,
                                     os->in_st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
        if (sd) {
            uint8_t *data = av_memdup(sd->data, sd->size);
            if (!data || !av_packet_side_data_add(&os->out_st->codecpar->coded_side_data,
                                                  &os->out_st->codecpar->nb_coded_side_data,
                                                  AV_PKT_DATA_DISPLAYMATRIX, data, sd->size, 0)) {
                av_free(data);
                return AVERROR(ENOMEM);
            }
        }
    }
    return 0;
}

static int copy_chapters(Engine *e)
{
    for (unsigned i = 0; i < e->ifmt->nb_chapters; i++) {
        const AVChapter *in = e->ifmt->chapters[i];
        AVChapter *ch = av_mallocz(sizeof(*ch));
        AVChapter **tmp;
        int ret;

        if (!ch)
            return AVERROR(ENOMEM);
        ch->id        = in->id;
        ch->time_base = in->time_base;
        ch->start     = in->start;
        ch->end       = in->end;
        if ((ret = av_dict_copy(&ch->metadata, in->metadata, 0)) < 0 ||
            !(tmp = av_realloc_array(e->ofmt->chapters, e->ofmt->nb_chapters + 1, sizeof(*tmp)))) {
            av_dict_free(&ch->metadata);
            av_free(ch);
            return ret < 0 ? ret : AVERROR(ENOMEM);
        }
        e->ofmt->chapters = tmp;
        e->ofmt->chapters[e->ofmt->nb_chapters++] = ch;
    }
    return 0;
}

static int setup(Engine *e)
{
    const ConvJob *job = e->job;
    const AVOutputFormat *of = NULL;
    AVDictionary *opts = NULL;
    const AVDictionaryEntry *left;
    int ret;

    if (same_path(job->input, job->output))
        return fail(e, AVERROR(EINVAL), "output file is the input file");

    /* input */
    if (!(e->ifmt = avformat_alloc_context()))
        return AVERROR(ENOMEM);
    e->ifmt->interrupt_callback = (AVIOInterruptCB){ interrupt_cb, e };
    if ((ret = avformat_open_input(&e->ifmt, job->input, NULL, NULL)) < 0)
        return fail(e, ret, "cannot open input %s", job->input);
    if ((ret = avformat_find_stream_info(e->ifmt, NULL)) < 0)
        return fail(e, ret, "cannot read stream info of %s", job->input);
    e->in_start = e->ifmt->start_time != AV_NOPTS_VALUE ? e->ifmt->start_time / 1e6 : 0;
    e->duration = e->ifmt->duration != AV_NOPTS_VALUE ? e->ifmt->duration / 1e6 : 0;

    /* output */
    if (job->muxer && !(of = caps_lookup_muxer(job->muxer)))
        return fail(e, AVERROR_MUXER_NOT_FOUND, "unknown muxer '%s'", job->muxer);
    if ((ret = avformat_alloc_output_context2(&e->ofmt, of, NULL, job->output)) < 0 || !e->ofmt)
        return fail(e, ret < 0 ? ret : AVERROR_MUXER_NOT_FOUND,
                    "cannot choose a container for %s (set \"muxer\")", job->output);

    if (!(e->os = av_calloc(job->nb_streams ? job->nb_streams : 1, sizeof(*e->os))))
        return AVERROR(ENOMEM);

    for (int i = 0; i < job->nb_streams; i++) {
        const JobStream *js = &job->streams[i];
        OutStream *os;

        if (js->input_index < 0 || js->input_index >= (int)e->ifmt->nb_streams)
            return fail(e, AVERROR(EINVAL), "stream #%d: input has no stream %d", i, js->input_index);
        if (js->action == JOB_DROP)
            continue;

        os = &e->os[e->nb_os++];
        os->js           = js;
        os->job_index    = i;
        os->in_st        = e->ifmt->streams[js->input_index];
        os->type         = os->in_st->codecpar->codec_type;
        os->last_enc_pts = AV_NOPTS_VALUE;
        os->audio_next_pts = AV_NOPTS_VALUE;
        os->frame        = av_frame_alloc();
        os->filt_frame   = av_frame_alloc();
        os->pkt          = av_packet_alloc();
        if (!os->frame || !os->filt_frame || !os->pkt)
            return AVERROR(ENOMEM);

        ret = js->action == JOB_COPY ? setup_copy(e, os) : setup_transcode(e, os);
        if (ret < 0)
            return fail(e, ret, "stream #%d: setup failed", i);
        os->out_st->disposition = os->in_st->disposition;
        if ((ret = copy_stream_metadata(os)) < 0)
            return ret;
    }
    if (!e->nb_os)
        return fail(e, AVERROR(EINVAL), "no output streams (every stream is dropped)");

    if (job->copy_metadata && (ret = av_dict_copy(&e->ofmt->metadata, e->ifmt->metadata, 0)) < 0)
        return ret;
    if ((ret = apply_overrides(&e->ofmt->metadata, job->metadata)) < 0)
        return ret;
    if (job->copy_chapters && (ret = copy_chapters(e)) < 0)
        return ret;

    if (!(e->ofmt->oformat->flags & AVFMT_NOFILE)) {
        if (!job->overwrite && avio_check(job->output, 0) >= 0)
            return fail(e, AVERROR(EEXIST), "%s already exists (set \"overwrite\": true)", job->output);
        if ((ret = avio_open(&e->ofmt->pb, job->output, AVIO_FLAG_WRITE)) < 0)
            return fail(e, ret, "cannot create %s", job->output);
        e->output_opened = 1;
    }

    if ((ret = av_dict_copy(&opts, job->muxer_options, 0)) < 0)
        return ret;
    ret = avformat_write_header(e->ofmt, &opts);
    left = av_dict_iterate(opts, NULL);
    if (ret >= 0 && left)
        ret = fail(e, AVERROR_OPTION_NOT_FOUND, "muxer %s has no option '%s'", e->ofmt->oformat->name, left->key);
    av_dict_free(&opts);
    if (ret < 0)
        return fail(e, ret, "cannot write the %s header", e->ofmt->oformat->name);
    e->header_written = 1;
    return 0;
}

/* ------------------------------------------------------------------------- */

static int run(Engine *e)
{
    int ret;

    if ((ret = setup(e)) < 0)
        return ret;
    if (!(e->pkt = av_packet_alloc()))
        return AVERROR(ENOMEM);

    while (!cancelled(e)) {
        ret = av_read_frame(e->ifmt, e->pkt);
        if (ret == AVERROR_EOF || (ret == AVERROR_EXIT && cancelled(e)))
            break;
        if (ret < 0)
            return fail(e, ret, "error reading %s", e->job->input);

        for (int i = 0; i < e->nb_os && ret >= 0; i++)
            if (e->os[i].in_st->index == e->pkt->stream_index)
                ret = process_packet(e, &e->os[i], e->pkt);
        av_packet_unref(e->pkt);
        if (ret < 0)
            return ret;
        report(e, 0);
    }

    /* flush decoders, filters and encoders, even when cancelled, so the
     * output is a valid (if shorter) file */
    for (int i = 0; i < e->nb_os; i++) {
        OutStream *os = &e->os[i];
        if (os->js->action == JOB_TRANSCODE && os->type != AVMEDIA_TYPE_SUBTITLE &&
            (ret = decode_packet(e, os, NULL)) < 0)
            return ret;
    }
    return cancelled(e) ? AVERROR_EXIT : 0;
}

int engine_run(const ConvJob *job, const EngineCallbacks *cb, atomic_int *cancel,
               EngineStats *stats, char *err, size_t errlen)
{
    Engine e = {
        .job    = job,
        .cancel = cancel,
        .err    = err,
        .errlen = errlen,
        .t0     = av_gettime_relative(),
    };
    int ret, tret;

    if (err && errlen)
        err[0] = '\0';
    if (cb)
        e.cb = *cb;

    ret = run(&e);

    if (e.header_written && (tret = av_write_trailer(e.ofmt)) < 0 && ret >= 0)
        ret = fail(&e, tret, "cannot finalise %s", job->output);
    report(&e, 1);
    if (ret == AVERROR_EXIT && err && errlen && !err[0])
        snprintf(err, errlen, "cancelled");

    for (int i = 0; i < e.nb_os; i++) {
        OutStream *os = &e.os[i];
        avcodec_free_context(&os->dec);
        avcodec_free_context(&os->enc);
        avfilter_graph_free(&os->graph);
        av_channel_layout_uninit(&os->cfg_layout);
        av_frame_free(&os->frame);
        av_frame_free(&os->filt_frame);
        av_packet_free(&os->pkt);
        av_free(os->chain);
        av_free(os->sub_buf);
    }
    av_free(e.os);
    av_packet_free(&e.pkt);
    if (e.ofmt && e.output_opened)
        avio_closep(&e.ofmt->pb);
    avformat_free_context(e.ofmt);
    avformat_close_input(&e.ifmt);

    if (ret < 0 && e.output_opened && !job->keep_partial)
        delete_file(job->output);
    if (stats)
        *stats = e.stats;
    return ret;
}

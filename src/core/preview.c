/*
 * preview.c - one frame before and after conversion.
 */
#include "preview.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include "engine.h"
#include "fsutil.h"

static int set_err(char *err, size_t errlen, int ret, const char *fmt, const char *arg)
{
    if (err && errlen) {
        char msg[512];
        snprintf(msg, sizeof(msg), fmt, arg);
        if (ret < 0 && ret != AVERROR_EXIT)
            snprintf(err, errlen, "%s: %s", msg, av_err2str(ret));
        else
            snprintf(err, errlen, "%s", msg);
    }
    return ret < 0 ? ret : AVERROR(EINVAL);
}

/* ------------------------------------------------------------------------- */
/* frame -> RGBA, display aspect applied                                     */

static int to_rgba(const AVFrame *f, AVRational sar, PreviewImage *img)
{
    struct SwsContext *sws;
    int dw = f->width, dh = f->height, ret = 0;
    int full_range = f->color_range == AVCOL_RANGE_JPEG;
    uint8_t *dst[4] = { NULL };
    int dst_stride[4] = { 0 };
    const int *coefs;

    if (sar.num > 0 && sar.den > 0 && av_cmp_q(sar, (AVRational){ 1, 1 }))
        dw = (int)lrint(f->width * av_q2d(sar)) & ~1;   /* non-square pixels: widen */
    if (dw <= 0)
        dw = f->width;

    sws = sws_getContext(f->width, f->height, f->format, dw, dh, AV_PIX_FMT_RGBA,
                         SWS_BICUBIC, NULL, NULL, NULL);
    if (!sws)
        return AVERROR(EINVAL);
    /* YUV -> RGB with the frame's own matrix (BT.709 for HD...) */
    coefs = sws_getCoefficients(f->colorspace != AVCOL_SPC_UNSPECIFIED ? f->colorspace
                                : f->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG);
    sws_setColorspaceDetails(sws, coefs, full_range, sws_getCoefficients(AVCOL_SPC_BT709), 1, 0, 1 << 16, 1 << 16);

    img->width  = dw;
    img->height = dh;
    img->stride = dw * 4;
    if (!(img->rgba = av_malloc((size_t)img->stride * dh))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    dst[0]        = img->rgba;
    dst_stride[0] = img->stride;
    sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dst_stride);

    img->coded_width  = f->width;
    img->coded_height = f->height;
    av_strlcpy(img->pix_fmt, av_get_pix_fmt_name(f->format) ? av_get_pix_fmt_name(f->format) : "?",
               sizeof(img->pix_fmt));
end:
    sws_freeContext(sws);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* the frame shown at `target` seconds (from the file's start)               */

typedef struct Grab {
    const char *path;
    int         stream_index;
    double      target;
    int         seek;
    int         from_start;    /* times from the file's start time (the input), or
                                  from 0 (the clip: its timeline starts at the cut) */
    /* out */
    int64_t     bytes;         /* the stream's packets, whole file */
    double      span;          /* seconds between its first and last packet */
} Grab;

static int grab_frame(Grab *g, atomic_int *cancel, PreviewImage *img, char *err, size_t errlen)
{
    AVFormatContext *fc = NULL;
    AVCodecContext *dc = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc(), *best = av_frame_alloc();
    const AVCodec *codec;
    AVDictionary *opts = NULL;
    AVStream *st;
    int64_t base, target_ts, half = 0, first_ts = AV_NOPTS_VALUE, last_ts = AV_NOPTS_VALUE;
    int have_best = 0, found = 0, ret;
    const int stats = g->bytes >= 0;   /* read the whole file for its size and span */

    if (!pkt || !frame || !best) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((ret = avformat_open_input(&fc, g->path, NULL, NULL)) < 0 ||
        (ret = avformat_find_stream_info(fc, NULL)) < 0) {
        ret = set_err(err, errlen, ret, "cannot read %s", g->path);
        goto end;
    }
    if (g->stream_index < 0 || g->stream_index >= (int)fc->nb_streams ||
        fc->streams[g->stream_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        ret = set_err(err, errlen, AVERROR(EINVAL), "%s: not a video stream", g->path);
        goto end;
    }
    st = fc->streams[g->stream_index];
    if (!(codec = avcodec_find_decoder(st->codecpar->codec_id)) || !(dc = avcodec_alloc_context3(codec)) ||
        (ret = avcodec_parameters_to_context(dc, st->codecpar)) < 0) {
        ret = set_err(err, errlen, codec ? AVERROR(ENOMEM) : AVERROR_DECODER_NOT_FOUND, "cannot decode %s",
                      avcodec_get_name(st->codecpar->codec_id));
        goto end;
    }
    dc->pkt_timebase = st->time_base;
    av_dict_set(&opts, "threads", "auto", 0);
    if ((ret = avcodec_open2(dc, codec, &opts)) < 0) {
        ret = set_err(err, errlen, ret, "cannot open the %s decoder", codec->name);
        goto end;
    }

    base      = g->from_start && fc->start_time != AV_NOPTS_VALUE ? fc->start_time : 0;
    target_ts = av_rescale_q(base + llrint(g->target * AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base);
    {
        AVRational fr = av_guess_frame_rate(fc, st, NULL);
        if (fr.num > 0 && fr.den > 0)
            half = av_rescale_q(1, av_inv_q(fr), st->time_base) / 2;
    }
    if (g->seek && g->target > 0)
        avformat_seek_file(fc, -1, INT64_MIN, base + llrint(g->target * AV_TIME_BASE),
                           base + llrint(g->target * AV_TIME_BASE), 0);

    /* decode until the first frame after the target: the frame shown at
     * the target is the last one starting at or before it */
    for (int eof = 0; !found || stats;) {
        if (cancel && atomic_load(cancel)) {
            ret = AVERROR_EXIT;
            goto end;
        }
        ret = av_read_frame(fc, pkt);
        if (ret == AVERROR_EOF)
            eof = 1;
        else if (ret < 0)
            break;
        if (!eof && pkt->stream_index != g->stream_index) {
            av_packet_unref(pkt);
            continue;
        }
        if (!eof && stats) {
            int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            g->bytes += pkt->size;
            if (ts != AV_NOPTS_VALUE) {
                if (first_ts == AV_NOPTS_VALUE || ts < first_ts)
                    first_ts = ts;
                if (last_ts == AV_NOPTS_VALUE || ts + pkt->duration > last_ts)
                    last_ts = ts + pkt->duration;
            }
        }
        if (!found)
            avcodec_send_packet(dc, eof ? NULL : pkt);
        av_packet_unref(pkt);

        while (!found && avcodec_receive_frame(dc, frame) >= 0) {
            int64_t pts = frame->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE && pts > target_ts + half && have_best) {
                found = 1;              /* `best` is the frame on screen at the target */
            } else {
                av_frame_unref(best);
                av_frame_move_ref(best, frame);
                have_best = 1;
                if (pts != AV_NOPTS_VALUE && pts > target_ts + half)
                    found = 1;          /* nothing earlier: the first frame after it */
            }
            av_frame_unref(frame);
        }
        if (eof)
            break;
    }
    if (!have_best) {
        ret = set_err(err, errlen, AVERROR_INVALIDDATA, "no frame decoded from %s", g->path);
        goto end;
    }
    if ((ret = to_rgba(best, av_guess_sample_aspect_ratio(fc, st, best), img)) < 0) {
        ret = set_err(err, errlen, ret, "cannot convert the %s frame", av_get_pix_fmt_name(best->format));
        goto end;
    }
    img->time = best->best_effort_timestamp != AV_NOPTS_VALUE
              ? best->best_effort_timestamp * av_q2d(st->time_base) - base / (double)AV_TIME_BASE : 0;
    if (first_ts != AV_NOPTS_VALUE)
        g->span = (last_ts - first_ts) * av_q2d(st->time_base);
    ret = 0;

end:
    av_dict_free(&opts);
    av_frame_free(&frame);
    av_frame_free(&best);
    av_packet_free(&pkt);
    avcodec_free_context(&dc);
    avformat_close_input(&fc);
    return ret;
}

/* ------------------------------------------------------------------------- */

/* A job with only this stream, trimmed to [start, end], into `muxer`. */
static ConvJob *clip_job(const PreviewRequest *req, const char *muxer, double start, double end)
{
    const JobStream *src = &req->job->streams[req->stream];
    ConvJob *job = job_alloc();
    JobStream *s;

    if (!job || !(job->input = av_strdup(req->job->input)) || !(job->output = av_strdup(req->scratch)) ||
        !(job->muxer = av_strdup(muxer)) || !(s = job_add_stream(job, src->input_index, JOB_TRANSCODE)) ||
        !(s->encoder = av_strdup(src->encoder)) || av_dict_copy(&s->encoder_options, src->encoder_options, 0) < 0)
        goto fail;
    job->overwrite     = 1;
    job->copy_metadata = 0;
    job->copy_chapters = 0;
    job->trim_start    = start;
    job->trim_end      = end;
    if (src->filter_string) {
        if (!(s->filter_string = av_strdup(src->filter_string)))
            goto fail;
    }
    for (int i = 0; i < src->nb_filters; i++) {
        JobFilter *f = job_stream_add_filter(s, src->filters[i].name);
        if (!f || av_dict_copy(&f->options, src->filters[i].options, 0) < 0)
            goto fail;
    }
    return job;
fail:
    job_free(&job);
    return NULL;
}

static int copy_image(PreviewImage *dst, const PreviewImage *src)
{
    *dst = *src;
    if (!(dst->rgba = av_memdup(src->rgba, (size_t)src->stride * src->height)))
        return AVERROR(ENOMEM);
    return 0;
}

int preview_render(const PreviewRequest *req, atomic_int *cancel, PreviewResult *out, char *err, size_t errlen)
{
    const JobStream *js;
    double lead = req->lead > 0 ? req->lead : 0.5, tail = req->tail > 0 ? req->tail : 1.0;
    double start, end;
    Grab g = { 0 };
    int64_t t0;
    int ret;

    memset(out, 0, sizeof(*out));
    if (err && errlen)
        err[0] = '\0';
    if (!req->job || req->stream < 0 || req->stream >= req->job->nb_streams)
        return set_err(err, errlen, AVERROR(EINVAL), "no such stream in the job%s", "");
    js = &req->job->streams[req->stream];
    if (js->action == JOB_DROP)
        return set_err(err, errlen, AVERROR(EINVAL), "this stream is dropped%s", "");

    /* before: the input frame */
    g.path = req->job->input;
    g.stream_index = js->input_index;
    g.target = req->time;
    g.seek = 1;
    g.from_start = 1;                   /* like the trim times */
    g.bytes = -1;                       /* no statistics needed */
    if ((ret = grab_frame(&g, cancel, &out->before, err, errlen)) < 0)
        goto fail;

    if (js->action == JOB_COPY) {       /* copied: the output frame is the input frame */
        out->copied = 1;
        av_strlcpy(out->encoder, "copy", sizeof(out->encoder));
        if ((ret = copy_image(&out->after, &out->before)) < 0)
            goto fail;
        return 0;
    }
    av_strlcpy(out->encoder, js->encoder ? js->encoder : "", sizeof(out->encoder));

    /* after: a short clip through the real pipeline */
    start = req->time - lead > 0 ? req->time - lead : 0;
    end   = req->time + tail;
    t0    = av_gettime_relative();
    {
        static const char *const muxers[] = { "matroska", "nut" };   /* nut takes what mkv refuses */
        char first_err[1024] = "";

        ret = AVERROR_UNKNOWN;
        for (int i = 0; i < 2 && ret < 0; i++) {
            ConvJob *clip = clip_job(req, muxers[i], start, end);
            if (!clip) {
                ret = AVERROR(ENOMEM);
                break;
            }
            ret = engine_run(clip, NULL, cancel, NULL, err, errlen);
            job_free(&clip);
            if (ret == AVERROR_EXIT)
                break;
            if (ret < 0 && !first_err[0] && err)
                av_strlcpy(first_err, err, sizeof(first_err));
        }
        if (ret < 0) {
            if (ret != AVERROR_EXIT && err && errlen && first_err[0])
                av_strlcpy(err, first_err, errlen);   /* the more likely explanation */
            goto fail;
        }
    }
    out->encode_seconds = (av_gettime_relative() - t0) / 1e6;

    g.path = req->scratch;
    g.stream_index = 0;
    g.target = out->before.time - start;   /* the same frame, on the clip's timeline */
    g.seek = 0;
    g.from_start = 0;
    g.bytes = 0;
    if ((ret = grab_frame(&g, cancel, &out->after, err, errlen)) < 0)
        goto fail;
    out->clip_bytes   = g.bytes;
    out->clip_seconds = g.span > 0 ? g.span : end - start;
    out->kbps         = out->clip_seconds > 0 ? g.bytes * 8 / out->clip_seconds / 1000 : 0;
    conv_delete_file(req->scratch);
    return 0;

fail:
    conv_delete_file(req->scratch);
    preview_result_free(out);
    return ret;
}

void preview_result_free(PreviewResult *r)
{
    if (!r)
        return;
    av_freep(&r->before.rgba);
    av_freep(&r->after.rgba);
}

/* ------------------------------------------------------------------------- */

int preview_save_png(const PreviewImage *img, const char *path)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext *enc = codec ? avcodec_alloc_context3(codec) : NULL;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    AVIOContext *pb = NULL;
    int ret;

    if (!enc || !frame || !pkt) {
        ret = codec ? AVERROR(ENOMEM) : AVERROR_ENCODER_NOT_FOUND;
        goto end;
    }
    enc->width     = img->width;
    enc->height    = img->height;
    enc->pix_fmt   = AV_PIX_FMT_RGBA;
    enc->time_base = (AVRational){ 1, 25 };
    if ((ret = avcodec_open2(enc, codec, NULL)) < 0)
        goto end;

    frame->format = AV_PIX_FMT_RGBA;
    frame->width  = img->width;
    frame->height = img->height;
    if ((ret = av_frame_get_buffer(frame, 0)) < 0)
        goto end;
    av_image_copy_plane(frame->data[0], frame->linesize[0], img->rgba, img->stride, img->width * 4, img->height);

    if ((ret = avcodec_send_frame(enc, frame)) < 0 || (ret = avcodec_send_frame(enc, NULL)) < 0 ||
        (ret = avcodec_receive_packet(enc, pkt)) < 0)
        goto end;
    if ((ret = avio_open(&pb, path, AVIO_FLAG_WRITE)) < 0)
        goto end;
    avio_write(pb, pkt->data, pkt->size);
    ret = avio_closep(&pb);

end:
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    return ret;
}

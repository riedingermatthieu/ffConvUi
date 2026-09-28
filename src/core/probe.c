/*
 * probe.c - build a MediaInfo snapshot from an input file.
 */
#include "probe.h"

#include <math.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>

static void set_str(char *dst, size_t size, const char *src)
{
    av_strlcpy(dst, src ? src : "", size);
}

static const char *dict_get(const AVDictionary *d, const char *key)
{
    const AVDictionaryEntry *e = av_dict_get(d, key, NULL, 0);
    return e ? e->value : NULL;
}

static void set_error(char *err, size_t errlen, const char *what, int ret)
{
    char buf[AV_ERROR_MAX_STRING_SIZE];

    if (!err || !errlen)
        return;
    av_strerror(ret, buf, sizeof(buf));
    snprintf(err, errlen, "%s: %s", what, buf);
}

static void free_tags(MiTag *tags, int nb)
{
    for (int i = 0; i < nb; i++) {
        av_free(tags[i].key);
        av_free(tags[i].value);
    }
    av_free(tags);
}

static int copy_tags(const AVDictionary *d, MiTag **out, int *nb_out)
{
    const AVDictionaryEntry *e = NULL;
    int n = av_dict_count(d), i = 0;
    MiTag *tags;

    *out    = NULL;
    *nb_out = 0;
    if (n <= 0)
        return 0;

    tags = av_calloc(n, sizeof(*tags));
    if (!tags)
        return AVERROR(ENOMEM);

    while (i < n && (e = av_dict_iterate(d, e))) {
        tags[i].key   = av_strdup(e->key);
        tags[i].value = av_strdup(e->value);
        i++;
        if (!tags[i - 1].key || !tags[i - 1].value) {
            free_tags(tags, i);
            return AVERROR(ENOMEM);
        }
    }
    *out    = tags;
    *nb_out = i;
    return 0;
}

static const AVPacketSideData *side_data(const AVCodecParameters *par,
                                         enum AVPacketSideDataType type)
{
    return av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, type);
}

static void fill_video(MediaStream *ms, AVFormatContext *fmt, AVStream *st)
{
    const AVCodecParameters *par = st->codecpar;
    const AVPacketSideData *sd;
    AVRational sar;

    ms->width   = par->width;
    ms->height  = par->height;
    ms->pix_fmt = par->format;
    set_str(ms->pix_fmt_name, sizeof(ms->pix_fmt_name),
            av_get_pix_fmt_name((enum AVPixelFormat)par->format));

    sar = av_guess_sample_aspect_ratio(fmt, st, NULL);
    ms->sar = sar;
    if (par->width > 0 && par->height > 0) {
        if (sar.num <= 0 || sar.den <= 0)
            sar = (AVRational){ 1, 1 };
        av_reduce(&ms->dar.num, &ms->dar.den,
                  (int64_t)par->width * sar.num, (int64_t)par->height * sar.den,
                  1024 * 1024);
    }

    ms->avg_frame_rate     = st->avg_frame_rate;
    ms->r_frame_rate       = st->r_frame_rate;
    ms->guessed_frame_rate = av_guess_frame_rate(fmt, st, NULL);

    ms->color_range     = par->color_range;
    ms->color_space     = par->color_space;
    ms->color_primaries = par->color_primaries;
    ms->color_trc       = par->color_trc;
    ms->field_order     = par->field_order;
    ms->is_attached_pic = !!(st->disposition & AV_DISPOSITION_ATTACHED_PIC);

    sd = side_data(par, AV_PKT_DATA_DISPLAYMATRIX);
    if (sd && sd->size >= 9 * sizeof(int32_t)) {
        double r = av_display_rotation_get((const int32_t *)sd->data);
        if (!isnan(r)) {
            ms->has_rotation = 1;
            ms->rotation     = r;
        }
    }
    ms->has_mastering_display = side_data(par, AV_PKT_DATA_MASTERING_DISPLAY_METADATA) != NULL;
    ms->has_content_light     = side_data(par, AV_PKT_DATA_CONTENT_LIGHT_LEVEL) != NULL;
    ms->has_dovi              = side_data(par, AV_PKT_DATA_DOVI_CONF) != NULL;
}

static void fill_audio(MediaStream *ms, const AVCodecParameters *par)
{
    ms->sample_rate = par->sample_rate;
    ms->sample_fmt  = par->format;
    set_str(ms->sample_fmt_name, sizeof(ms->sample_fmt_name),
            av_get_sample_fmt_name((enum AVSampleFormat)par->format));
    ms->channels   = par->ch_layout.nb_channels;
    ms->frame_size = par->frame_size;
    if (av_channel_layout_describe(&par->ch_layout, ms->ch_layout, sizeof(ms->ch_layout)) < 0)
        ms->ch_layout[0] = '\0';
}

static int fill_stream(MediaStream *ms, AVFormatContext *fmt, AVStream *st)
{
    const AVCodecParameters *par  = st->codecpar;
    const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);

    ms->index    = st->index;
    ms->type     = par->codec_type;
    ms->codec_id = par->codec_id;
    set_str(ms->codec_name, sizeof(ms->codec_name), avcodec_get_name(par->codec_id));
    set_str(ms->codec_long_name, sizeof(ms->codec_long_name), desc ? desc->long_name : NULL);
    set_str(ms->profile, sizeof(ms->profile), avcodec_profile_name(par->codec_id, par->profile));
    ms->codec_tag           = par->codec_tag;
    ms->bit_rate            = par->bit_rate;
    ms->bits_per_raw_sample = par->bits_per_raw_sample;
    ms->has_decoder         = avcodec_find_decoder(par->codec_id) != NULL;

    ms->time_base  = st->time_base;
    ms->start_time = st->start_time;
    ms->duration   = st->duration;
    ms->nb_frames  = st->nb_frames;

    ms->disposition = st->disposition;
    set_str(ms->language, sizeof(ms->language), dict_get(st->metadata, "language"));
    set_str(ms->title, sizeof(ms->title), dict_get(st->metadata, "title"));

    switch (par->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        fill_video(ms, fmt, st);
        break;
    case AVMEDIA_TYPE_AUDIO:
        fill_audio(ms, par);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if (desc) {
            ms->is_text_sub   = !!(desc->props & AV_CODEC_PROP_TEXT_SUB);
            ms->is_bitmap_sub = !!(desc->props & AV_CODEC_PROP_BITMAP_SUB);
        }
        break;
    case AVMEDIA_TYPE_ATTACHMENT:
        set_str(ms->filename, sizeof(ms->filename), dict_get(st->metadata, "filename"));
        set_str(ms->mimetype, sizeof(ms->mimetype), dict_get(st->metadata, "mimetype"));
        break;
    default:
        break;
    }

    return copy_tags(st->metadata, &ms->tags, &ms->nb_tags);
}

static int fill_chapters(MediaInfo *mi, const AVFormatContext *fmt)
{
    if (!fmt->nb_chapters)
        return 0;

    mi->chapters = av_calloc(fmt->nb_chapters, sizeof(*mi->chapters));
    if (!mi->chapters)
        return AVERROR(ENOMEM);
    mi->nb_chapters = fmt->nb_chapters;

    for (unsigned i = 0; i < fmt->nb_chapters; i++) {
        const AVChapter *ch = fmt->chapters[i];
        const char *title   = dict_get(ch->metadata, "title");

        mi->chapters[i].start = ch->start * av_q2d(ch->time_base);
        mi->chapters[i].end   = ch->end * av_q2d(ch->time_base);
        if (title && !(mi->chapters[i].title = av_strdup(title)))
            return AVERROR(ENOMEM);
    }
    return 0;
}

int mi_probe(const char *path, MediaInfo **out, char *err, size_t errlen)
{
    AVFormatContext *fmt = NULL;
    MediaInfo *mi = NULL;
    int ret;

    *out = NULL;

    ret = avformat_open_input(&fmt, path, NULL, NULL);
    if (ret < 0) {
        set_error(err, errlen, "cannot open input", ret);
        return ret;
    }
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        set_error(err, errlen, "cannot read stream info", ret);
        goto end;
    }

    mi = av_mallocz(sizeof(*mi));
    if (!mi || !(mi->path = av_strdup(path))) {
        ret = AVERROR(ENOMEM);
        set_error(err, errlen, "probe", ret);
        goto end;
    }

    set_str(mi->format_name, sizeof(mi->format_name), fmt->iformat->name);
    set_str(mi->format_long_name, sizeof(mi->format_long_name), fmt->iformat->long_name);
    mi->duration_us   = fmt->duration;
    mi->start_time_us = fmt->start_time;
    mi->bit_rate      = fmt->bit_rate;
    mi->file_size     = fmt->pb ? avio_size(fmt->pb) : -1;

    if (fmt->nb_streams) {
        mi->streams = av_calloc(fmt->nb_streams, sizeof(*mi->streams));
        if (!mi->streams) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        mi->nb_streams = fmt->nb_streams;
        for (unsigned i = 0; i < fmt->nb_streams; i++)
            if ((ret = fill_stream(&mi->streams[i], fmt, fmt->streams[i])) < 0)
                goto end;
    }

    if ((ret = fill_chapters(mi, fmt)) < 0 ||
        (ret = copy_tags(fmt->metadata, &mi->tags, &mi->nb_tags)) < 0) {
        set_error(err, errlen, "probe", ret);
        goto end;
    }
    ret = 0;

end:
    avformat_close_input(&fmt);
    if (ret < 0)
        mi_free(&mi);
    else
        *out = mi;
    return ret;
}

void mi_free(MediaInfo **pmi)
{
    MediaInfo *mi = pmi ? *pmi : NULL;

    if (!mi)
        return;
    for (int i = 0; i < mi->nb_streams; i++)
        free_tags(mi->streams[i].tags, mi->streams[i].nb_tags);
    av_free(mi->streams);
    for (int i = 0; i < mi->nb_chapters; i++)
        av_free(mi->chapters[i].title);
    av_free(mi->chapters);
    free_tags(mi->tags, mi->nb_tags);
    av_free(mi->path);
    av_freep(pmi);
}

int mi_count_type(const MediaInfo *mi, enum AVMediaType type)
{
    int n = 0;

    for (int i = 0; i < mi->nb_streams; i++)
        n += mi->streams[i].type == type;
    return n;
}

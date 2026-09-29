/*
 * convcli - headless front end for the converter core (milestones M1-M4).
 *
 *   convcli version [--config]
 *   convcli probe <file>
 *   convcli muxers   [--for <file>] [--common] [--search <text>]
 *   convcli encoders [--type video|audio|subtitle] [--muxer <name>]
 *                    [--common] [--search <text>]
 *   convcli actions  <file> --muxer <name> [--experimental] [--all]
 *   convcli filters  [--type video|audio] [--class <list>|all] [--search <text>]
 *   convcli options  encoder|muxer|filter <name> [--generic]
 *   convcli hw
 *   convcli job-template <file> [--muxer <key>] [--output <file>]
 *   convcli run <job.json> [--overwrite] [--quiet] [--cancel-after <seconds>]
 *   convcli validate <job.json> [--static]
 *   convcli command <job.json> [--shell bash|powershell|cmd]
 */
#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "avopt_schema.h"
#include "caps.h"
#include "cmdline.h"
#include "engine.h"
#include "job.h"
#include "probe.h"
#include "validate.h"

/* ------------------------------------------------------------------------- */
/* argument helpers                                                          */

typedef struct Args {
    int    argc;
    char **argv;
} Args;

static const char *arg_value(const Args *a, const char *flag)
{
    for (int i = 0; i < a->argc - 1; i++)
        if (!strcmp(a->argv[i], flag))
            return a->argv[i + 1];
    return NULL;
}

static int arg_flag(const Args *a, const char *flag)
{
    for (int i = 0; i < a->argc; i++)
        if (!strcmp(a->argv[i], flag))
            return 1;
    return 0;
}

/* First argument that is neither a flag nor the value of a flag taking one. */
static const char *arg_positional(const Args *a, int index)
{
    static const char *const with_value[] = {
        "--for", "--type", "--muxer", "--search", "--class", "--output",
        "--cancel-after", "--shell", NULL
    };
    int n = 0;

    for (int i = 0; i < a->argc; i++) {
        int takes_value = 0;

        if (a->argv[i][0] == '-' && a->argv[i][1] == '-') {
            for (const char *const *w = with_value; *w; w++)
                takes_value |= !strcmp(a->argv[i], *w);
            i += takes_value;
            continue;
        }
        if (n++ == index)
            return a->argv[i];
    }
    return NULL;
}

static int matches_search(const char *search, const char *a, const char *b)
{
    if (!search)
        return 1;
    return (a && av_stristr(a, search)) || (b && av_stristr(b, search));
}

static int parse_media_type(const char *s, enum AVMediaType *t)
{
    if (!s)                       *t = AVMEDIA_TYPE_UNKNOWN;
    else if (!strcmp(s, "video")) *t = AVMEDIA_TYPE_VIDEO;
    else if (!strcmp(s, "audio")) *t = AVMEDIA_TYPE_AUDIO;
    else if (!strcmp(s, "subtitle")) *t = AVMEDIA_TYPE_SUBTITLE;
    else {
        fprintf(stderr, "unknown media type '%s' (video|audio|subtitle)\n", s);
        return -1;
    }
    return 0;
}

static const char *type_str(enum AVMediaType t)
{
    const char *s = av_get_media_type_string(t);
    return s ? s : "unknown";
}

static char type_letter(enum AVMediaType t)
{
    switch (t) {
    case AVMEDIA_TYPE_VIDEO:      return 'V';
    case AVMEDIA_TYPE_AUDIO:      return 'A';
    case AVMEDIA_TYPE_SUBTITLE:   return 'S';
    case AVMEDIA_TYPE_DATA:       return 'D';
    case AVMEDIA_TYPE_ATTACHMENT: return 'T';
    default:                      return '?';
    }
}

static int load_caps(Caps **caps)
{
    int ret = caps_build(caps);
    if (ret < 0)
        fprintf(stderr, "cannot build capability catalog: %s\n", av_err2str(ret));
    return ret;
}

static int load_media(const char *path, MediaInfo **mi)
{
    char err[256];
    int ret;

    if (!path) {
        fprintf(stderr, "missing input file\n");
        return AVERROR(EINVAL);
    }
    if ((ret = mi_probe(path, mi, err, sizeof(err))) < 0)
        fprintf(stderr, "%s: %s\n", path, err);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* version                                                                   */

static void print_lib(const char *name, unsigned v)
{
    printf("  %-14s %u.%u.%u\n", name, AV_VERSION_MAJOR(v), AV_VERSION_MINOR(v), AV_VERSION_MICRO(v));
}

static int cmd_version(const Args *a)
{
    printf("convcli 0.1.0 - FFmpeg %s\n", av_version_info());
    print_lib("libavutil", avutil_version());
    print_lib("libavcodec", avcodec_version());
    print_lib("libavformat", avformat_version());
    print_lib("libavfilter", avfilter_version());
    print_lib("libswscale", swscale_version());
    print_lib("libswresample", swresample_version());
    printf("  license        %s\n", avcodec_license());
    if (arg_flag(a, "--config"))
        printf("  configuration  %s\n", avcodec_configuration());
    return 0;
}

/* ------------------------------------------------------------------------- */
/* probe                                                                     */

static void format_time(int64_t us, char *buf, size_t size)
{
    int64_t s;

    if (us == AV_NOPTS_VALUE) {
        av_strlcpy(buf, "N/A", size);
        return;
    }
    if (us < 0)
        us = -us;
    s = us / AV_TIME_BASE;
    snprintf(buf, size, "%02" PRId64 ":%02d:%02d.%03d", s / 3600, (int)(s / 60 % 60),
             (int)(s % 60), (int)(us % AV_TIME_BASE / 1000));
}

static void format_size(int64_t bytes, char *buf, size_t size)
{
    if (bytes < 0)
        av_strlcpy(buf, "N/A", size);
    else if (bytes < 1024 * 1024)
        snprintf(buf, size, "%.1f KiB", bytes / 1024.0);
    else if (bytes < (int64_t)1024 * 1024 * 1024)
        snprintf(buf, size, "%.1f MiB", bytes / (1024.0 * 1024));
    else
        snprintf(buf, size, "%.2f GiB", bytes / (1024.0 * 1024 * 1024));
}

static void print_tags(const MiTag *tags, int nb, const char *indent)
{
    for (int i = 0; i < nb; i++)
        printf("%s%-16s: %s\n", indent, tags[i].key, tags[i].value);
}

static const char *field_order_str(int fo)
{
    switch (fo) {
    case AV_FIELD_PROGRESSIVE: return "progressive";
    case AV_FIELD_TT:          return "interlaced (top first)";
    case AV_FIELD_BB:          return "interlaced (bottom first)";
    case AV_FIELD_TB:
    case AV_FIELD_BT:          return "interlaced";
    default:                   return NULL;
    }
}

static void print_disposition(int disp)
{
    int first = 1;

    for (int bit = 0; bit < 31; bit++) {
        const char *name;
        if (!(disp & (1 << bit)) || !(name = av_disposition_to_string(1 << bit)))
            continue;
        printf("%s%s", first ? "    disposition: " : ", ", name);
        first = 0;
    }
    if (!first)
        printf("\n");
}

static void print_video(const MediaStream *s)
{
    const char *fo = field_order_str(s->field_order);

    printf("    %dx%d", s->width, s->height);
    if (s->sar.num > 0 && s->sar.den > 0)
        printf(" SAR %d:%d", s->sar.num, s->sar.den);
    if (s->dar.num > 0)
        printf(" DAR %d:%d", s->dar.num, s->dar.den);
    printf(", %s", s->pix_fmt_name[0] ? s->pix_fmt_name : "unknown pix_fmt");
    if (s->guessed_frame_rate.num > 0 && s->guessed_frame_rate.den > 0)
        printf(", %.3f fps", av_q2d(s->guessed_frame_rate));
    if (s->avg_frame_rate.num > 0 && s->r_frame_rate.num > 0 &&
        av_cmp_q(s->avg_frame_rate, s->r_frame_rate))
        printf(" (avg %.3f, base %.3f: possibly VFR)", av_q2d(s->avg_frame_rate), av_q2d(s->r_frame_rate));
    printf("\n");

    printf("    color: range=%s space=%s primaries=%s trc=%s",
           av_color_range_name(s->color_range), av_color_space_name(s->color_space),
           av_color_primaries_name(s->color_primaries), av_color_transfer_name(s->color_trc));
    if (fo)
        printf(", %s", fo);
    printf("\n");

    if (s->has_rotation)
        printf("    rotation: %.1f degrees\n", s->rotation);
    if (s->has_mastering_display || s->has_content_light || s->has_dovi)
        printf("    HDR: %s%s%s\n", s->has_mastering_display ? "mastering-display " : "",
               s->has_content_light ? "content-light-level " : "",
               s->has_dovi ? "dolby-vision" : "");
    if (s->is_attached_pic)
        printf("    attached picture (cover art)\n");
}

static void print_stream(const MediaStream *s)
{
    printf("  Stream #%d %s: %s", s->index, type_str(s->type), s->codec_name);
    if (s->profile[0])
        printf(" (%s)", s->profile);
    if (s->codec_long_name[0])
        printf(" - %s", s->codec_long_name);
    if (!s->has_decoder && s->codec_id != AV_CODEC_ID_NONE)
        printf(" [no decoder]");
    printf("\n");

    switch (s->type) {
    case AVMEDIA_TYPE_VIDEO:
        print_video(s);
        break;
    case AVMEDIA_TYPE_AUDIO:
        printf("    %d Hz, %s (%d ch), %s", s->sample_rate,
               s->ch_layout[0] ? s->ch_layout : "unknown layout", s->channels,
               s->sample_fmt_name[0] ? s->sample_fmt_name : "unknown sample_fmt");
        if (s->frame_size)
            printf(", frame_size %d", s->frame_size);
        printf("\n");
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        printf("    %s subtitles\n", s->is_text_sub ? "text" : s->is_bitmap_sub ? "bitmap" : "unknown");
        break;
    case AVMEDIA_TYPE_ATTACHMENT:
        printf("    file: %s (%s)\n", s->filename[0] ? s->filename : "?", s->mimetype[0] ? s->mimetype : "?");
        break;
    default:
        break;
    }

    if (s->bit_rate > 0)
        printf("    bitrate: %" PRId64 " kb/s\n", s->bit_rate / 1000);
    if (s->duration != AV_NOPTS_VALUE) {
        char d[32];
        format_time(av_rescale_q(s->duration, s->time_base, AV_TIME_BASE_Q), d, sizeof(d));
        printf("    duration: %s", d);
        if (s->nb_frames > 0)
            printf(", %" PRId64 " frames", s->nb_frames);
        printf("\n");
    }
    printf("    time_base: %d/%d\n", s->time_base.num, s->time_base.den);
    if (s->language[0])
        printf("    language: %s\n", s->language);
    if (s->title[0])
        printf("    title: %s\n", s->title);
    print_disposition(s->disposition);
    for (int i = 0; i < s->nb_tags; i++)
        if (av_strcasecmp(s->tags[i].key, "language") && av_strcasecmp(s->tags[i].key, "title"))
            printf("    tag %-12s: %s\n", s->tags[i].key, s->tags[i].value);
}

static int cmd_probe(const Args *a)
{
    MediaInfo *mi = NULL;
    char dur[32], start[32], size[32];
    int ret;

    if ((ret = load_media(arg_positional(a, 0), &mi)) < 0)
        return ret;

    format_time(mi->duration_us, dur, sizeof(dur));
    format_time(mi->start_time_us, start, sizeof(start));
    format_size(mi->file_size, size, sizeof(size));

    printf("Input: %s\n", mi->path);
    printf("  Format  : %s (%s)\n", mi->format_name, mi->format_long_name);
    printf("  Duration: %s, start: %s, bitrate: ", dur, start);
    if (mi->bit_rate > 0)
        printf("%" PRId64 " kb/s", mi->bit_rate / 1000);
    else
        printf("N/A");
    printf(", size: %s\n", size);
    printf("  Streams : %d (video %d, audio %d, subtitle %d, data %d, attachment %d)\n",
           mi->nb_streams, mi_count_type(mi, AVMEDIA_TYPE_VIDEO),
           mi_count_type(mi, AVMEDIA_TYPE_AUDIO), mi_count_type(mi, AVMEDIA_TYPE_SUBTITLE),
           mi_count_type(mi, AVMEDIA_TYPE_DATA), mi_count_type(mi, AVMEDIA_TYPE_ATTACHMENT));
    if (mi->nb_tags) {
        printf("  Metadata:\n");
        print_tags(mi->tags, mi->nb_tags, "    ");
    }
    if (mi->nb_chapters) {
        printf("  Chapters: %d\n", mi->nb_chapters);
        for (int i = 0; i < mi->nb_chapters; i++)
            printf("    #%d %.3f - %.3f %s\n", i, mi->chapters[i].start, mi->chapters[i].end,
                   mi->chapters[i].title ? mi->chapters[i].title : "");
    }
    for (int i = 0; i < mi->nb_streams; i++)
        print_stream(&mi->streams[i]);

    mi_free(&mi);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* muxers                                                                    */

static void print_muxer_line(const CapsMuxer *m)
{
    printf("%c %-16s %-40.40s ext: %-12.12s defaults: V=%s A=%s S=%s\n",
           m->is_common ? '*' : ' ', m->key, m->long_name,
           m->extensions ? m->extensions : "-",
           avcodec_get_name(m->fmt->video_codec), avcodec_get_name(m->fmt->audio_codec),
           avcodec_get_name(m->fmt->subtitle_codec));
}

static int cmd_muxers(const Args *a)
{
    const char *search = arg_value(a, "--search");
    const char *file   = arg_value(a, "--for");
    int common_only    = arg_flag(a, "--common");
    Caps *caps = NULL;
    int ret, shown = 0;

    if ((ret = load_caps(&caps)) < 0)
        return ret;

    if (file) {
        MediaInfo *mi = NULL;
        CapsMuxerFit *fits = NULL;
        int nb;

        if ((ret = load_media(file, &mi)) < 0 ||
            (ret = caps_muxers_for(caps, mi, &fits, &nb)) < 0)
            goto end_file;

        printf("Containers for %s (%d video, %d audio, %d subtitle streams):\n", file,
               mi_count_type(mi, AVMEDIA_TYPE_VIDEO), mi_count_type(mi, AVMEDIA_TYPE_AUDIO),
               mi_count_type(mi, AVMEDIA_TYPE_SUBTITLE));
        for (int i = 0; i < nb; i++) {
            const CapsMuxerFit *f = &fits[i];
            if ((common_only && !f->mux->is_common) ||
                !matches_search(search, f->mux->key, f->mux->long_name))
                continue;
            printf("%c %-16s keeps %d/%d streams (V%d A%d S%d)  %s\n",
                   f->mux->is_common ? '*' : ' ', f->mux->key, f->nb_kept, f->nb_considered,
                   f->kept_video, f->kept_audio, f->kept_subtitle, f->mux->long_name);
            shown++;
        }
        printf("%d container(s) listed (* = common)\n", shown);
    end_file:
        av_free(fits);
        mi_free(&mi);
        caps_free(&caps);
        return ret;
    }

    for (int i = 0; i < caps->nb_muxers; i++) {
        const CapsMuxer *m = &caps->muxers[i];
        if ((common_only && !m->is_common) || !matches_search(search, m->key, m->long_name))
            continue;
        print_muxer_line(m);
        shown++;
    }
    printf("%d of %d muxer(s) listed (* = common)\n", shown, caps->nb_muxers);
    caps_free(&caps);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* encoders                                                                  */

static void print_encoder_flags(const CapsEncoder *e)
{
    printf("%c%c%c%c%c",
           type_letter(e->type),
           e->is_experimental ? 'X' : '.',
           e->is_hardware ? 'H' : '.',
           (e->props & AV_CODEC_PROP_LOSSLESS) ? 'L' : '.',
           (e->props & AV_CODEC_PROP_INTRA_ONLY) ? 'I' : '.');
}

static int cmd_encoders(const Args *a)
{
    const char *search    = arg_value(a, "--search");
    const char *mux_name  = arg_value(a, "--muxer");
    int common_only       = arg_flag(a, "--common");
    const CapsMuxer *mux  = NULL;
    enum AVMediaType type;
    Caps *caps = NULL;
    int ret, shown = 0;

    if (parse_media_type(arg_value(a, "--type"), &type) < 0)
        return AVERROR(EINVAL);
    if ((ret = load_caps(&caps)) < 0)
        return ret;
    if (mux_name && !(mux = caps_find_muxer(caps, mux_name))) {
        fprintf(stderr, "unknown muxer '%s'\n", mux_name);
        caps_free(&caps);
        return AVERROR_MUXER_NOT_FOUND;
    }

    for (int i = 0; i < caps->nb_encoders; i++) {
        const CapsEncoder *e = &caps->encoders[i];
        CapsCompat compat = CAPS_YES;

        if ((type != AVMEDIA_TYPE_UNKNOWN && e->type != type) ||
            (common_only && !e->is_common) ||
            !matches_search(search, e->name, e->long_name))
            continue;
        if (mux && (compat = caps_mux_codec(mux, e->id)) == CAPS_NO)
            continue;

        printf("%c ", e->is_common ? '*' : ' ');
        print_encoder_flags(e);
        printf(" %-22s %-6s %s", e->name, mux ? caps_compat_name(compat) : "", e->long_name);
        if (e->wrapper_name)
            printf(" [%s]", e->wrapper_name);
        printf("\n");
        shown++;
    }
    printf("%d encoder(s) listed%s%s\n", shown, mux ? " for muxer " : "", mux ? mux->key : "");
    printf("flags: V/A/S type, X experimental, H hardware, L lossless, I intra-only; * common\n");
    caps_free(&caps);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* actions                                                                   */

static int cmd_actions(const Args *a)
{
    const char *file     = arg_positional(a, 0);
    const char *mux_name = arg_value(a, "--muxer");
    int show_all         = arg_flag(a, "--all");
    int experimental     = arg_flag(a, "--experimental");
    const CapsMuxer *mux;
    MediaInfo *mi = NULL;
    Caps *caps = NULL;
    int ret;

    if (!mux_name) {
        fprintf(stderr, "actions: --muxer <name> is required\n");
        return AVERROR(EINVAL);
    }
    if ((ret = load_caps(&caps)) < 0)
        return ret;
    if (!(mux = caps_find_muxer(caps, mux_name))) {
        fprintf(stderr, "unknown muxer '%s'\n", mux_name);
        ret = AVERROR_MUXER_NOT_FOUND;
        goto end;
    }
    if ((ret = load_media(file, &mi)) < 0)
        goto end;

    printf("Writing %s to %s (%s):\n", file, mux->key, mux->long_name);
    for (int i = 0; i < mi->nb_streams; i++) {
        const MediaStream *s = &mi->streams[i];
        CapsStreamActions act;
        int yes = 0, maybe = 0, limit;

        if ((ret = caps_stream_actions(caps, mux, s, experimental, &act)) < 0)
            goto end;

        printf("\n  Stream #%d %s: %s\n", s->index, type_str(s->type), s->codec_name);
        printf("    copy      : %s\n", caps_compat_name(act.copy));
        if (act.can_transcode) {
            for (int j = 0; j < act.nb_encoders; j++) {
                yes   += act.encoders[j].compat == CAPS_YES;
                maybe += act.encoders[j].compat == CAPS_MAYBE;
            }
            printf("    transcode : %d encoder(s) (%d yes, %d maybe)\n", act.nb_encoders, yes, maybe);
            limit = show_all ? act.nb_encoders : FFMIN(act.nb_encoders, 12);
            for (int j = 0; j < limit; j++) {
                const CapsEncChoice *ch = &act.encoders[j];
                printf("      %c %-22s %-5s %s\n", ch->enc->is_common ? '*' : ' ', ch->enc->name,
                       caps_compat_name(ch->compat), ch->enc->long_name);
            }
            if (limit < act.nb_encoders)
                printf("      ... %d more (use --all)\n", act.nb_encoders - limit);
        } else {
            printf("    transcode : not possible\n");
        }
        printf("    drop      : yes\n");
        if (act.note)
            printf("    note      : %s\n", act.note);
        caps_stream_actions_free(&act);
    }
    printf("\n'maybe' = FFmpeg has no information for this pair; validation (M4) will try it.\n");

end:
    mi_free(&mi);
    caps_free(&caps);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* filters                                                                   */

static int parse_filter_classes(const char *s, unsigned *mask)
{
    static const struct { const char *name; unsigned bit; } names[] = {
        { "simple", CAPS_FILTER_SIMPLE },      { "hw", CAPS_FILTER_HW },
        { "convert", CAPS_FILTER_CONVERT },    { "multi-in", CAPS_FILTER_MULTI_IN },
        { "multi-out", CAPS_FILTER_MULTI_OUT }, { "source", CAPS_FILTER_SOURCE },
        { "sink", CAPS_FILTER_SINK },          { "all", CAPS_FILTER_ALL },
    };
    char buf[256], *tok, *save = NULL;

    *mask = 0;
    if (!s) {
        *mask = CAPS_FILTER_SIMPLE;
        return 0;
    }
    av_strlcpy(buf, s, sizeof(buf));
    for (tok = av_strtok(buf, ",", &save); tok; tok = av_strtok(NULL, ",", &save)) {
        unsigned bit = 0;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (!strcmp(tok, names[i].name))
                bit = names[i].bit;
        if (!bit) {
            fprintf(stderr, "unknown filter class '%s' "
                    "(simple,hw,convert,multi-in,multi-out,source,sink,all)\n", tok);
            return -1;
        }
        *mask |= bit;
    }
    return 0;
}

static int cmd_filters(const Args *a)
{
    const char *search = arg_value(a, "--search");
    const CapsFilter **list = NULL;
    enum AVMediaType type;
    unsigned mask;
    Caps *caps = NULL;
    int ret, nb, shown = 0;

    if (parse_media_type(arg_value(a, "--type"), &type) < 0 ||
        parse_filter_classes(arg_value(a, "--class"), &mask) < 0)
        return AVERROR(EINVAL);
    if ((ret = load_caps(&caps)) < 0)
        return ret;
    if ((ret = caps_filters_for(caps, type, mask, &list, &nb)) < 0)
        goto end;

    for (int i = 0; i < nb; i++) {
        const CapsFilter *f = list[i];
        if (!matches_search(search, f->name, f->description))
            continue;
        printf("%-22s %-9s %c->%c %c%c  %s\n", f->name, caps_filter_class_name(f->cls),
               f->dynamic_inputs && !f->default_pads_probed ? 'N'
                   : f->nb_inputs ? type_letter(f->in_type) : '|',
               f->dynamic_outputs && !f->default_pads_probed ? 'N'
                   : f->nb_outputs ? type_letter(f->out_type) : '|',
               f->supports_timeline ? 'T' : '.', f->metadata_only ? 'M' : '.', f->description);
        shown++;
    }
    printf("%d filter(s) listed (of %d in this build)\n", shown, caps->nb_filters);
    printf("pads: V video, A audio, N dynamic, | none; flags: T timeline (enable=), M metadata only\n");

end:
    av_free(list);
    caps_free(&caps);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* options                                                                   */

static void print_schema(const char *title, const AVClass *cls, int required_flags, int children)
{
    OptSchemaList list;
    int ret;

    if (!cls) {
        printf("\n%s: none\n", title);
        return;
    }
    if ((ret = optschema_from_class(cls, required_flags, children, &list)) < 0) {
        fprintf(stderr, "cannot read options: %s\n", av_err2str(ret));
        return;
    }
    printf("\n%s (%s): %d option(s)\n", title, cls->class_name, list.nb_opts);

    for (int i = 0; i < list.nb_opts; i++) {
        const OptSchema *s = &list.opts[i];
        char type[48];

        if (s->alias_of) {
            printf("  -%-24s alias of -%s\n", s->name, s->alias_of);
            continue;
        }
        snprintf(type, sizeof(type), "%s%s", s->is_array ? "list of " : "", optschema_type_name(s->type));
        printf("  -%-24s %-14s", s->name, type);
        if (s->owner != cls)
            printf(" [%s]", s->owner->class_name);
        if (s->default_str[0])
            printf(" default: %s", s->default_str);
        if (s->widget == OPT_W_INT || s->widget == OPT_W_FLOAT ||
            (s->widget == OPT_W_ENUM && s->type != AV_OPT_TYPE_STRING))
            printf("  [%g .. %g]", s->min, s->max);
        printf("  (%s)\n", optschema_widget_name(s->widget));
        if (s->help[0])
            printf("        %s\n", s->help);
        for (int j = 0; j < s->nb_choices; j++)
            printf("          %-20s %-8" PRId64 " %s\n", s->choices[j].name, s->choices[j].value,
                   s->choices[j].help ? s->choices[j].help : "");
    }
    optschema_free(&list);
}

static void print_encoder_formats(const CapsEncoder *e)
{
    printf("Encoder %s - %s\n", e->name, e->long_name);
    printf("  type: %s, codec: %s", type_str(e->type), avcodec_get_name(e->id));
    if (e->wrapper_name)
        printf(", wrapper: %s", e->wrapper_name);
    printf("%s%s\n", e->is_experimental ? ", experimental" : "", e->is_hardware ? ", hardware" : "");

    if (e->type == AVMEDIA_TYPE_VIDEO) {
        printf("  pixel formats:");
        if (!e->pix_fmts)
            printf(" any");
        for (int i = 0; i < e->nb_pix_fmts; i++)
            printf(" %s", av_get_pix_fmt_name(e->pix_fmts[i]));
        printf("\n");
        if (e->frame_rates) {
            printf("  frame rates:");
            for (int i = 0; i < e->nb_frame_rates; i++)
                printf(" %d/%d", e->frame_rates[i].num, e->frame_rates[i].den);
            printf("\n");
        }
    } else if (e->type == AVMEDIA_TYPE_AUDIO) {
        printf("  sample formats:");
        if (!e->sample_fmts)
            printf(" any");
        for (int i = 0; i < e->nb_sample_fmts; i++)
            printf(" %s", av_get_sample_fmt_name(e->sample_fmts[i]));
        printf("\n  sample rates:");
        if (!e->sample_rates)
            printf(" any");
        for (int i = 0; i < e->nb_sample_rates; i++)
            printf(" %d", e->sample_rates[i]);
        printf("\n  channel layouts:");
        if (!e->ch_layouts)
            printf(" any");
        for (int i = 0; i < e->nb_ch_layouts; i++) {
            char buf[64];
            if (av_channel_layout_describe(&e->ch_layouts[i], buf, sizeof(buf)) >= 0)
                printf(" %s", buf);
        }
        printf("\n");
    }
}

static int media_param_flag(enum AVMediaType t)
{
    switch (t) {
    case AVMEDIA_TYPE_VIDEO:    return AV_OPT_FLAG_VIDEO_PARAM;
    case AVMEDIA_TYPE_AUDIO:    return AV_OPT_FLAG_AUDIO_PARAM;
    case AVMEDIA_TYPE_SUBTITLE: return AV_OPT_FLAG_SUBTITLE_PARAM;
    default:                    return 0;
    }
}

static int cmd_options(const Args *a)
{
    const char *kind = arg_positional(a, 0);
    const char *name = arg_positional(a, 1);
    int generic      = arg_flag(a, "--generic");
    Caps *caps = NULL;
    int ret;

    if (!kind || !name) {
        fprintf(stderr, "usage: convcli options encoder|muxer|filter <name> [--generic]\n");
        return AVERROR(EINVAL);
    }
    if ((ret = load_caps(&caps)) < 0)
        return ret;

    if (!strcmp(kind, "encoder")) {
        const CapsEncoder *e = caps_find_encoder(caps, name);
        if (!e) {
            fprintf(stderr, "unknown encoder '%s'\n", name);
            ret = AVERROR_ENCODER_NOT_FOUND;
            goto end;
        }
        print_encoder_formats(e);
        print_schema("Private options", e->codec->priv_class, 0, 1);
        if (generic)
            print_schema("Generic encoding options", avcodec_get_class(),
                         AV_OPT_FLAG_ENCODING_PARAM | media_param_flag(e->type), 0);
    } else if (!strcmp(kind, "muxer")) {
        const CapsMuxer *m = caps_find_muxer(caps, name);
        if (!m) {
            fprintf(stderr, "unknown muxer '%s'\n", name);
            ret = AVERROR_MUXER_NOT_FOUND;
            goto end;
        }
        print_muxer_line(m);
        print_schema("Private options", m->fmt->priv_class, 0, 1);
        if (generic)
            print_schema("Generic muxing options", avformat_get_class(), AV_OPT_FLAG_ENCODING_PARAM, 0);
    } else if (!strcmp(kind, "filter")) {
        const CapsFilter *f = caps_find_filter(caps, name);
        if (!f) {
            fprintf(stderr, "unknown filter '%s'\n", name);
            ret = AVERROR_FILTER_NOT_FOUND;
            goto end;
        }
        printf("Filter %s - %s\n", f->name, f->description);
        printf("  class: %s, inputs: %d%s, outputs: %d%s%s%s\n", caps_filter_class_name(f->cls),
               f->nb_inputs, f->dynamic_inputs ? " (dynamic)" : "",
               f->nb_outputs, f->dynamic_outputs ? " (dynamic)" : "",
               f->supports_timeline ? ", timeline" : "", f->metadata_only ? ", metadata-only" : "");
        if (f->default_pads_probed)
            printf("  (pad counts are those created with default options)\n");
        /* static pad descriptions; dynamic pads are only known per instance */
        for (unsigned i = 0; i < avfilter_filter_pad_count(f->filter, 0); i++)
            printf("  in  #%d %-10s %s\n", i, avfilter_pad_get_name(f->filter->inputs, i),
                   type_str(avfilter_pad_get_type(f->filter->inputs, i)));
        for (unsigned i = 0; i < avfilter_filter_pad_count(f->filter, 1); i++)
            printf("  out #%d %-10s %s\n", i, avfilter_pad_get_name(f->filter->outputs, i),
                   type_str(avfilter_pad_get_type(f->filter->outputs, i)));
        print_schema("Options", f->filter->priv_class, 0, 1);
    } else {
        fprintf(stderr, "unknown kind '%s' (encoder|muxer|filter)\n", kind);
        ret = AVERROR(EINVAL);
    }

end:
    caps_free(&caps);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* hw                                                                        */

static int cmd_hw(const Args *a)
{
    CapsHwDevice *devs = NULL;
    int level = av_log_get_level(), nb, ret;

    av_log_set_level(AV_LOG_QUIET);   /* failing device opens are expected */
    ret = caps_probe_hw(&devs, &nb);
    av_log_set_level(level);
    if (ret < 0)
        return ret;

    printf("Hardware device types in this build:\n");
    for (int i = 0; i < nb; i++)
        printf("  %-14s %s%s%s\n", devs[i].name, devs[i].available ? "available" : "not available",
               devs[i].available ? "" : ": ", devs[i].available ? "" : devs[i].error);
    if (!nb)
        printf("  none\n");
    av_free(devs);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* job-template                                                              */

static int cmd_job_template(const Args *a)
{
    const char *file     = arg_positional(a, 0);
    const char *mux_key  = arg_value(a, "--muxer");
    const char *output   = arg_value(a, "--output");
    const CapsMuxer *mux = NULL;
    MediaInfo *mi = NULL;
    Caps *caps = NULL;
    ConvJob *job = NULL;
    AVBPrint bp;
    int ret;

    if ((ret = load_caps(&caps)) < 0 || (ret = load_media(file, &mi)) < 0)
        goto end;

    if (!mux_key && output) {
        const AVOutputFormat *of = av_guess_format(NULL, output, NULL);
        mux_key = of ? of->name : NULL;
    }
    if (!(mux = caps_find_muxer(caps, mux_key ? mux_key : "matroska"))) {
        fprintf(stderr, "unknown muxer '%s'\n", mux_key);
        ret = AVERROR_MUXER_NOT_FOUND;
        goto end;
    }

    if (!(job = job_alloc()) || !(job->input = av_strdup(file)) ||
        !(job->output = output ? av_strdup(output)
                               : job_default_output(file, mux->extensions ? mux->extensions : mux->name)) ||
        !(job->muxer = av_strdup(mux->key))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    for (int i = 0; i < mi->nb_streams; i++) {
        CapsStreamActions act;
        const CapsEncoder *enc;
        JobStream *js;

        if ((ret = caps_stream_actions(caps, mux, &mi->streams[i], 0, &act)) < 0)
            goto end;
        switch (caps_default_action(&act, &enc)) {
        case CAPS_DEFAULT_COPY:
            js = job_add_stream(job, i, JOB_COPY);
            break;
        case CAPS_DEFAULT_TRANSCODE:
            if ((js = job_add_stream(job, i, JOB_TRANSCODE)))
                js->encoder = av_strdup(enc->name);
            break;
        default:
            js = job_add_stream(job, i, JOB_DROP);
            break;
        }
        caps_stream_actions_free(&act);
        if (!js || (js->action == JOB_TRANSCODE && !js->encoder)) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    job_to_json(job, &bp);
    fputs(bp.str, stdout);
    av_bprint_finalize(&bp, NULL);

end:
    job_free(&job);
    mi_free(&mi);
    caps_free(&caps);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* run                                                                       */

static atomic_int g_cancel;

static void on_sigint(int sig)
{
    atomic_store(&g_cancel, 1);
    signal(sig, on_sigint);
}

typedef struct RunState {
    int     quiet;
    double  cancel_after;   /* seconds, 0 = never (for testing cancellation) */
} RunState;

static void on_progress(const EngineProgress *p, void *opaque)
{
    RunState *rs = opaque;
    char t[32], d[32], size[32];

    if (rs->cancel_after > 0 && p->elapsed >= rs->cancel_after)
        atomic_store(&g_cancel, 1);
    if (rs->quiet)
        return;

    format_time((int64_t)(p->out_time * AV_TIME_BASE), t, sizeof(t));
    format_time(p->duration > 0 ? (int64_t)(p->duration * AV_TIME_BASE) : AV_NOPTS_VALUE, d, sizeof(d));
    format_size(p->out_bytes, size, sizeof(size));
    if (p->percent >= 0)
        fprintf(stderr, "\r  %5.1f%%", p->percent);
    else
        fprintf(stderr, "\r     ?%%");
    fprintf(stderr, "  %s / %s  frames %-7" PRId64 " fps %-6.1f speed %5.2fx  %s   ",
            t, d, p->frames, p->fps, p->speed, size);
    if (p->finished)
        fprintf(stderr, "\n");
}

static int cmd_run(const Args *a)
{
    const char *path = arg_positional(a, 0);
    const char *ca   = arg_value(a, "--cancel-after");
    RunState rs = { .quiet = arg_flag(a, "--quiet"), .cancel_after = ca ? atof(ca) : 0 };
    EngineCallbacks cb = { on_progress, &rs };
    EngineStats stats;
    ConvJob *job = NULL;
    char err[1024];
    int64_t t0;
    int ret;

    if (!path) {
        fprintf(stderr, "usage: convcli run <job.json> [--overwrite] [--quiet]\n");
        return AVERROR(EINVAL);
    }
    if ((ret = job_load(path, &job, err, sizeof(err))) < 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        return ret;
    }
    if (arg_flag(a, "--overwrite"))
        job->overwrite = 1;

    if (!rs.quiet)
        fprintf(stderr, "Converting %s -> %s\n", job->input, job->output);
    signal(SIGINT, on_sigint);
    t0 = av_gettime_relative();
    ret = engine_run(job, &cb, &g_cancel, &stats, err, sizeof(err));
    signal(SIGINT, SIG_DFL);

    if (ret == AVERROR_EXIT)
        fprintf(stderr, "Cancelled%s\n", job->keep_partial ? " (partial output kept)" : " (output removed)");
    else if (ret < 0)
        fprintf(stderr, "Error: %s\n", err);
    else
        fprintf(stderr, "Done in %.2f s\n", (av_gettime_relative() - t0) / 1e6);
    if (stats.decode_errors || stats.dropped_frames || stats.graph_reinits)
        fprintf(stderr, "  decode errors: %" PRId64 ", dropped frames: %" PRId64 ", filter graph rebuilds: %d\n",
                stats.decode_errors, stats.dropped_frames, stats.graph_reinits);
    job_free(&job);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* command                                                                   */

static int cmd_command(const Args *a)
{
    const char *path  = arg_positional(a, 0);
    const char *shell = arg_value(a, "--shell");
    CmdShell sh = cmd_default_shell();
    MediaInfo *mi = NULL;
    ConvJob *job = NULL;
    char err[1024];
    AVBPrint bp;
    int ret;

    if (!path || (shell && cmd_shell_from_name(shell, &sh) < 0)) {
        fprintf(stderr, "usage: convcli command <job.json> [--shell bash|powershell|cmd]\n");
        return AVERROR(EINVAL);
    }
    if ((ret = job_load(path, &job, err, sizeof(err))) < 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        return ret;
    }
    /* the input's stream types give readable specifiers (-c:v:0); without
     * them output indices are used (-c:0) */
    if (mi_probe(job->input, &mi, err, sizeof(err)) < 0)
        fprintf(stderr, "note: %s: %s (using stream indices)\n", job->input, err);

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    job_to_ffmpeg_command(job, mi, sh, &bp);
    printf("%s\n", bp.str);
    av_bprint_finalize(&bp, NULL);
    mi_free(&mi);
    job_free(&job);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* validate                                                                  */

static int cmd_validate(const Args *a)
{
    const char *path = arg_positional(a, 0);
    unsigned flags   = arg_flag(a, "--static") ? 0 : VALIDATE_DRY_RUN;
    ConvJob *job = NULL;
    Caps *caps = NULL;
    ValReport rep;
    char err[1024];
    int ret;

    if (!path) {
        fprintf(stderr, "usage: convcli validate <job.json> [--static]\n");
        return AVERROR(EINVAL);
    }
    if ((ret = job_load(path, &job, err, sizeof(err))) < 0) {
        printf("error    job: %s\n", err);
        printf("Result: invalid job file\n");
        return ret;
    }
    if ((ret = load_caps(&caps)) < 0 || (ret = validate_job(job, caps, NULL, flags, &rep)) < 0)
        goto end;

    for (int i = 0; i < rep.nb_issues; i++) {
        const ValIssue *is = &rep.issues[i];
        char where[96];

        if (is->stream >= 0)
            snprintf(where, sizeof(where), "#%d%s%s", is->stream, is->field[0] ? " " : "", is->field);
        else
            snprintf(where, sizeof(where), "%s", is->field[0] ? is->field : "job");
        printf("%-8s %-22s %s\n", validate_severity_name(is->severity), where, is->message);
    }
    printf("Result: %d error(s), %d warning(s)%s - %s\n", rep.nb_errors, rep.nb_warnings,
           !(flags & VALIDATE_DRY_RUN) ? ", static checks only" :
           !rep.dry_run_done ? ", dry run skipped until these are fixed" :
           rep.nb_errors ? ", dry run failed" : ", dry run passed",
           rep.nb_errors ? "the job would fail" : "the job is valid");
    ret = rep.nb_errors ? AVERROR(EINVAL) : 0;
    validate_report_free(&rep);

end:
    caps_free(&caps);
    job_free(&job);
    return ret;
}

/* ------------------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
        "usage: convcli <command> [args]\n"
        "\n"
        "  version [--config]                          FFmpeg library versions and license\n"
        "  probe <file>                                describe an input file\n"
        "  muxers [--for <file>] [--common] [--search <text>]\n"
        "                                              list output containers\n"
        "  encoders [--type video|audio|subtitle] [--muxer <name>] [--common] [--search <text>]\n"
        "                                              list encoders\n"
        "  actions <file> --muxer <name> [--experimental] [--all]\n"
        "                                              per-stream copy/transcode choices\n"
        "  filters [--type video|audio] [--class simple,hw,convert,multi-in,multi-out,source,sink|all]\n"
        "          [--search <text>]                   list filters (default class: simple)\n"
        "  options encoder|muxer|filter <name> [--generic]\n"
        "                                              option schema for the UI\n"
        "  hw                                          hardware devices usable on this machine\n"
        "  job-template <file> [--muxer <key>] [--output <file>]\n"
        "                                              print a starting job (JSON) for a file\n"
        "  run <job.json> [--overwrite] [--quiet] [--cancel-after <seconds>]\n"
        "                                              run a conversion job (Ctrl+C cancels)\n"
        "  validate <job.json> [--static]              check a job: static checks, then a dry run\n");
}

#ifdef _WIN32
/* The C runtime's argv uses the ANSI code page; FFmpeg expects UTF-8 paths. */
static char **utf8_argv(int *argc)
{
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), argc);
    char **argv;

    if (!wargv)
        return NULL;
    argv = calloc(*argc + 1, sizeof(*argv));
    for (int i = 0; argv && i < *argc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if ((argv[i] = malloc(len)))
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], len, NULL, NULL);
    }
    LocalFree(wargv);
    return argv;
}
#endif

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        int (*fn)(const Args *);
    } commands[] = {
        { "version", cmd_version }, { "probe", cmd_probe },     { "muxers", cmd_muxers },
        { "encoders", cmd_encoders }, { "actions", cmd_actions }, { "filters", cmd_filters },
        { "options", cmd_options }, { "hw", cmd_hw },         { "job-template", cmd_job_template },
        { "run", cmd_run },           { "validate", cmd_validate }, { "command", cmd_command },
    };
    char **wargs = NULL;
    Args args;
    int ret = -1;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    if ((wargs = utf8_argv(&argc)))
        argv = wargs;
#endif

    av_log_set_level(AV_LOG_ERROR);

    if (argc < 2) {
        usage();
        return 2;
    }
    args.argc = argc - 2;
    args.argv = argv + 2;

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (!strcmp(argv[1], commands[i].name)) {
            ret = commands[i].fn(&args) < 0;
            goto done;
        }
    }
    fprintf(stderr, "unknown command '%s'\n\n", argv[1]);
    usage();
    ret = 2;

done:
    if (wargs) {
        for (int i = 0; i < argc; i++)
            free(wargs[i]);
        free(wargs);
    }
    return ret;
}

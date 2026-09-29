/*
 * validate.c - static checks and dry run of a ConvJob.
 */
#include "validate.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>

#include "engine.h"
#include "logcap.h"

#define MAX_CLASSES 16

typedef struct V {
    const ConvJob   *job;
    const Caps      *caps;
    const MediaInfo *mi;
    const CapsMuxer *mux;
    ValReport       *r;
    int              oom;
    int             *maybe_issue;   /* per job stream: index of its "maybe" warning, or -1 */
} V;

/* ------------------------------------------------------------------------- */
/* report                                                                    */

static int add(V *v, ValSeverity sev, int stream, const char *field, const char *fmt, ...)
{
    ValIssue *tmp, *is;
    va_list ap;

    if (v->oom)
        return -1;
    tmp = av_realloc_array(v->r->issues, v->r->nb_issues + 1, sizeof(*tmp));
    if (!tmp) {
        v->oom = 1;
        return -1;
    }
    v->r->issues = tmp;
    is = &tmp[v->r->nb_issues];
    memset(is, 0, sizeof(*is));
    is->severity = sev;
    is->stream   = stream;
    av_strlcpy(is->field, field ? field : "", sizeof(is->field));
    va_start(ap, fmt);
    vsnprintf(is->message, sizeof(is->message), fmt, ap);
    va_end(ap);

    v->r->nb_errors   += sev == VAL_ERROR;
    v->r->nb_warnings += sev == VAL_WARNING;
    return v->r->nb_issues++;
}

static void set_severity(V *v, int idx, ValSeverity sev)
{
    ValIssue *is = &v->r->issues[idx];

    v->r->nb_errors   -= is->severity == VAL_ERROR;
    v->r->nb_warnings -= is->severity == VAL_WARNING;
    is->severity = sev;
    v->r->nb_errors   += sev == VAL_ERROR;
    v->r->nb_warnings += sev == VAL_WARNING;
}

const char *validate_severity_name(ValSeverity s)
{
    switch (s) {
    case VAL_ERROR:   return "error";
    case VAL_WARNING: return "warning";
    default:          return "info";
    }
}

void validate_report_free(ValReport *r)
{
    if (!r)
        return;
    av_freep(&r->issues);
    memset(r, 0, sizeof(*r));
}

/* ------------------------------------------------------------------------- */
/* "did you mean"                                                            */

static int edit_distance(const char *a, const char *b)
{
    int la = FFMIN((int)strlen(a), 63), lb = FFMIN((int)strlen(b), 63);
    int prev[64], cur[64];

    for (int j = 0; j <= lb; j++)
        prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = av_tolower(a[i - 1]) != av_tolower(b[j - 1]);
            cur[j] = FFMIN3(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost);
        }
        memcpy(prev, cur, sizeof(int) * (lb + 1));
    }
    return prev[lb];
}

typedef struct Suggest {
    const char *name;
    const char *best;
    int         best_dist;
    int         best_preferred;
} Suggest;

static void suggest_init(Suggest *s, const char *name)
{
    s->name           = name;
    s->best           = NULL;
    s->best_dist      = FFMAX(1, (int)strlen(name) / 3) + 1;   /* must beat this */
    s->best_preferred = 0;
}

static int common_prefix(const char *a, const char *b)
{
    int n = 0;

    while (a[n] && b[n] && av_tolower(a[n]) == av_tolower(b[n]))
        n++;
    return n;
}

/* `preferred`: priority among equally close candidates (common muxer or
 * encoder: 1, matching the output file extension: 2) */
static void suggest_try_pref(Suggest *s, const char *candidate, int preferred)
{
    int d, better;

    if (!candidate)
        return;
    d = edit_distance(s->name, candidate);
    /* a prefix match ("crf" for "cr") is a good hint too */
    if (d >= s->best_dist && !av_strncasecmp(candidate, s->name, strlen(s->name)) && strlen(s->name) >= 3)
        d = s->best_dist - 1;

    if (!s->best) {
        better = d < s->best_dist;
    } else if (d != s->best_dist) {
        better = d < s->best_dist;
    } else {
        /* ties: the longer shared prefix wins ("mp5" -> "mp4" rather than
         * "md5"), then preferred candidates ("mp4" rather than "mp2") */
        int pc = common_prefix(candidate, s->name), pb = common_prefix(s->best, s->name);
        better = pc > pb || (pc == pb && preferred > s->best_preferred);
    }
    if (better) {
        s->best           = candidate;
        s->best_dist      = d;
        s->best_preferred = preferred;
    }
}

static void suggest_try(Suggest *s, const char *candidate)
{
    suggest_try_pref(s, candidate, 0);
}

static const char *did_you_mean(const Suggest *s, char *buf, size_t size)
{
    if (!s->best)
        return "";
    snprintf(buf, size, " (did you mean '%s'?)", s->best);
    return buf;
}

/* ------------------------------------------------------------------------- */
/* options                                                                   */

typedef struct ClassSet {
    const AVClass *cls[MAX_CLASSES];
    int            nb;
} ClassSet;

static void classes_add(ClassSet *set, const AVClass *cls, int children)
{
    const AVClass *child;
    void *iter = NULL;

    if (!cls || set->nb >= MAX_CLASSES)
        return;
    for (int i = 0; i < set->nb; i++)
        if (set->cls[i] == cls)
            return;
    set->cls[set->nb++] = cls;
    if (children && cls->child_class_iterate)
        while ((child = cls->child_class_iterate(&iter)))
            classes_add(set, child, 1);
}

static int option_exists(const ClassSet *set, const char *name)
{
    for (int i = 0; i < set->nb; i++) {
        const AVClass *c = set->cls[i];
        if (av_opt_find2(&c, name, NULL, 0, AV_OPT_SEARCH_FAKE_OBJ, NULL))
            return 1;
    }
    return 0;
}

static void option_suggest(const ClassSet *set, Suggest *s)
{
    for (int i = 0; i < set->nb; i++) {
        const AVClass *obj = set->cls[i];
        const AVOption *o = NULL;
        while ((o = av_opt_next(&obj, o)))
            if (o->type != AV_OPT_TYPE_CONST)
                suggest_try(s, o->name);
    }
}

/* Check option names; values are checked by the dry run / filter parse. */
/* Check option values by setting them on a scratch context, on the object
 * FFmpeg would give each one to: avcodec_open2() applies private options
 * first, avformat_write_header() generic ones first. Unknown names are
 * skipped (check_options reports them). */
static void check_values(V *v, int stream, const char *prefix, const char *owner,
                         void *obj, void *priv, int priv_first, const AVDictionary *opts)
{
    const AVDictionaryEntry *e = NULL;
    LogCapture cap = { .level = AV_LOG_ERROR };

    if (!obj)
        return;
    logcap_ensure_installed();
    while ((e = av_dict_iterate(opts, e))) {
        int in_priv = priv && av_opt_find(priv, e->key, NULL, 0, AV_OPT_SEARCH_CHILDREN);
        int in_gen  = av_opt_find(obj, e->key, NULL, 0, 0) != NULL;
        void *target = in_priv && (priv_first || !in_gen) ? priv : in_gen ? obj : NULL;
        char field[64];
        int ret;

        if (!target)
            continue;
        logcap_begin(&cap);
        ret = av_opt_set(target, e->key, e->value, target == priv ? AV_OPT_SEARCH_CHILDREN : 0);
        logcap_end();
        if (ret >= 0)
            continue;
        snprintf(field, sizeof(field), "%s.%s", prefix, e->key);
        add(v, VAL_ERROR, stream, field, "%s: invalid value '%s' for '%s': %s%s%s%s", owner, e->value,
            e->key, av_err2str(ret), cap.first[0] ? " (" : "", cap.first, cap.first[0] ? ")" : "");
    }
}

static void check_options(V *v, int stream, const char *prefix, const char *owner,
                          const ClassSet *set, const AVDictionary *opts)
{
    const AVDictionaryEntry *e = NULL;

    while ((e = av_dict_iterate(opts, e))) {
        char field[64], hint[96];
        Suggest s;

        if (option_exists(set, e->key))
            continue;
        snprintf(field, sizeof(field), "%s.%s", prefix, e->key);

        if (!strcmp(e->key, "q") || !strncmp(e->key, "q:", 2) || !strncmp(e->key, "qscale", 6)) {
            add(v, VAL_ERROR, stream, field,
                "'%s' is ffmpeg CLI shorthand, not an option of %s: use \"global_quality\" "
                "(quality x 118) together with \"flags\": \"+qscale\"", e->key, owner);
            continue;
        }
        if (strchr(e->key, ':')) {
            add(v, VAL_ERROR, stream, field,
                "'%s': stream specifiers (\":v\", \":a\"...) are ffmpeg CLI syntax; options already "
                "apply to this stream, use '%.*s'", e->key, (int)strcspn(e->key, ":"), e->key);
            continue;
        }
        suggest_init(&s, e->key);
        option_suggest(set, &s);
        add(v, VAL_ERROR, stream, field, "%s has no option '%s'%s", owner, e->key,
            did_you_mean(&s, hint, sizeof(hint)));
    }
}

/* ------------------------------------------------------------------------- */
/* job-level checks                                                          */

static int same_path(const char *a, const char *b)
{
#ifdef _WIN32
    return !av_strcasecmp(a, b);
#else
    return !strcmp(a, b);
#endif
}

static const CapsMuxer *muxer_of(const Caps *caps, const AVOutputFormat *of)
{
    for (int i = 0; i < caps->nb_muxers; i++)
        if (caps->muxers[i].fmt == of)
            return &caps->muxers[i];
    return NULL;
}

static void check_muxer(V *v)
{
    const ConvJob *job = v->job;
    const char *ext = strrchr(job->output, '.');
    ClassSet set = { 0 };
    char hint[96], owner[96];

    if (job->muxer) {
        if (!(v->mux = caps_find_muxer(v->caps, job->muxer))) {
            const AVOutputFormat *guessed = job->output ? av_guess_format(NULL, job->output, NULL) : NULL;
            Suggest s;
            suggest_init(&s, job->muxer);
            for (int i = 0; i < v->caps->nb_muxers; i++)
                suggest_try_pref(&s, v->caps->muxers[i].key,
                                 v->caps->muxers[i].fmt == guessed ? 2 : v->caps->muxers[i].is_common);
            add(v, VAL_ERROR, -1, "muxer", "unknown muxer '%s'%s", job->muxer,
                did_you_mean(&s, hint, sizeof(hint)));
            return;
        }
        if (v->mux->extensions && ext && !av_match_ext(job->output, v->mux->extensions))
            add(v, VAL_WARNING, -1, "output",
                "'%s' is not a usual extension for %s (expected: %s)", ext, v->mux->key,
                v->mux->extensions);
    } else {
        const AVOutputFormat *of = av_guess_format(NULL, job->output, NULL);
        if (!of || !(v->mux = muxer_of(v->caps, of))) {
            add(v, VAL_ERROR, -1, "muxer",
                "cannot guess the container from '%s': set \"muxer\"", ext ? ext : job->output);
            return;
        }
    }

    classes_add(&set, v->mux->fmt->priv_class, 1);
    classes_add(&set, avformat_get_class(), 0);
    snprintf(owner, sizeof(owner), "muxer %s", v->mux->key);
    check_options(v, -1, "muxer_options", owner, &set, job->muxer_options);
    if (av_dict_count(job->muxer_options)) {
        AVFormatContext *fc = NULL;
        if (avformat_alloc_output_context2(&fc, v->mux->fmt, NULL, NULL) >= 0) {
            check_values(v, -1, "muxer_options", owner, fc, fc->priv_data, 0, job->muxer_options);
            avformat_free_context(fc);
        }
    }
}

static void check_paths(V *v)
{
    const ConvJob *job = v->job;

    if (!job->input || !*job->input)
        add(v, VAL_ERROR, -1, "input", "no input file");
    if (!job->output || !*job->output) {
        add(v, VAL_ERROR, -1, "output", "no output file");
        return;
    }
    if (job->input && same_path(job->input, job->output))
        add(v, VAL_ERROR, -1, "output", "the output file is the input file");

    if (v->mux && (v->mux->flags & AVFMT_NOFILE))
        return;   /* image2, hls...: output is a pattern or a playlist */

    if (avio_check(job->output, 0) >= 0) {
        if (!job->overwrite)
            add(v, VAL_ERROR, -1, "overwrite", "%s already exists (set \"overwrite\": true)", job->output);
        else
            add(v, VAL_INFO, -1, "overwrite", "%s will be overwritten", job->output);
    } else {
        char *dir = av_strdup(job->output);
        if (dir) {
            const char *d = av_dirname(dir);
            if (strcmp(d, ".") && avio_check(d, 0) < 0)
                add(v, VAL_ERROR, -1, "output", "folder %s does not exist", d);
            av_free(dir);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* per-stream checks                                                         */

/* Parse the chain in a scratch graph: this creates and initialises every
 * filter with its options, so bad values are caught without any input. */
static void parse_chain(V *v, int stream, const char *chain, enum AVMediaType type)
{
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterInOut *ins = NULL, *outs = NULL;
    LogCapture cap = { .level = AV_LOG_ERROR };
    int ret, nb_in = 0, nb_out = 0;

    if (!graph) {
        v->oom = 1;
        return;
    }
    logcap_ensure_installed();
    logcap_begin(&cap);
    ret = avfilter_graph_parse2(graph, chain, &ins, &outs);
    logcap_end();

    if (ret < 0) {
        add(v, VAL_ERROR, stream, "filters", "invalid filter chain \"%s\": %s%s%s%s", chain,
            av_err2str(ret), cap.first[0] ? " (" : "", cap.first, cap.first[0] ? ")" : "");
        goto end;
    }
    for (AVFilterInOut *p = ins; p; p = p->next) {
        nb_in++;
        if (avfilter_pad_get_type(p->filter_ctx->input_pads, p->pad_idx) != type)
            add(v, VAL_ERROR, stream, "filters", "the chain's input is %s but the stream is %s",
                av_get_media_type_string(avfilter_pad_get_type(p->filter_ctx->input_pads, p->pad_idx)),
                av_get_media_type_string(type));
    }
    for (AVFilterInOut *p = outs; p; p = p->next) {
        nb_out++;
        if (avfilter_pad_get_type(p->filter_ctx->output_pads, p->pad_idx) != type)
            add(v, VAL_ERROR, stream, "filters", "the chain outputs %s but the stream is %s",
                av_get_media_type_string(avfilter_pad_get_type(p->filter_ctx->output_pads, p->pad_idx)),
                av_get_media_type_string(type));
    }
    if (nb_in != 1 || nb_out != 1)
        add(v, VAL_ERROR, stream, "filters",
            "a stream's chain needs exactly one input and one output (this one has %d and %d); "
            "multi-input filters such as overlay or amix are not supported yet", nb_in, nb_out);
end:
    avfilter_inout_free(&ins);
    avfilter_inout_free(&outs);
    avfilter_graph_free(&graph);
}

static void check_filters(V *v, int si, const JobStream *js, enum AVMediaType type)
{
    int named_ok = 1, errors_before = v->r->nb_errors;
    AVBPrint bp;

    for (int i = 0; i < js->nb_filters; i++) {
        const JobFilter *jf = &js->filters[i];
        const CapsFilter *cf = caps_find_filter(v->caps, jf->name);
        char field[64], hint[96], owner[96];
        ClassSet set = { 0 };

        snprintf(field, sizeof(field), "filters[%d]", i);
        if (!cf) {
            Suggest s;
            suggest_init(&s, jf->name);
            for (int j = 0; j < v->caps->nb_filters; j++)
                suggest_try(&s, v->caps->filters[j].name);
            add(v, VAL_ERROR, si, field, "unknown filter '%s'%s", jf->name, did_you_mean(&s, hint, sizeof(hint)));
            named_ok = 0;
            continue;
        }
        switch (cf->cls) {
        case CAPS_FILTER_SIMPLE:
            if (cf->in_type != type) {
                add(v, VAL_ERROR, si, field, "%s is a%s %s filter but the stream is %s", cf->name,
                    cf->in_type == AVMEDIA_TYPE_AUDIO ? "n" : "", av_get_media_type_string(cf->in_type),
                    av_get_media_type_string(type));
                named_ok = 0;
            }
            break;
        case CAPS_FILTER_HW:
            add(v, VAL_ERROR, si, field, "%s works on hardware frames, which are not supported yet", cf->name);
            named_ok = 0;
            break;
        case CAPS_FILTER_CONVERT:
            add(v, VAL_ERROR, si, field, "%s turns %s into %s, which a stream's chain cannot do", cf->name,
                av_get_media_type_string(cf->in_type), av_get_media_type_string(cf->out_type));
            named_ok = 0;
            break;
        default:
            add(v, VAL_ERROR, si, field, "%s is a %s filter; a stream's chain only takes one-input, "
                "one-output filters", cf->name, caps_filter_class_name(cf->cls));
            named_ok = 0;
            break;
        }

        classes_add(&set, cf->filter->priv_class, 1);
        classes_add(&set, avfilter_get_class(), 0);   /* enable, threads... */
        snprintf(owner, sizeof(owner), "filter %s", cf->name);
        check_options(v, si, field, owner, &set, jf->options);
    }

    /* values (and raw filtergraph strings) */
    if (!named_ok || v->r->nb_errors > errors_before)
        return;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    job_stream_filter_string(js, &bp);
    if (bp.len)
        parse_chain(v, si, bp.str, type);
    av_bprint_finalize(&bp, NULL);
}

static int subtitle_kind(enum AVCodecID id)
{
    const AVCodecDescriptor *d = avcodec_descriptor_get(id);

    if (!d)
        return 0;
    return (d->props & AV_CODEC_PROP_TEXT_SUB) ? 1 : (d->props & AV_CODEC_PROP_BITMAP_SUB) ? 2 : 0;
}

static void check_stream(V *v, int si)
{
    const JobStream *js = &v->job->streams[si];
    const MediaStream *ms;
    const CapsEncoder *enc;
    const char *tname;
    CapsCompat compat;

    if (!v->mi)
        return;
    if (js->input_index < 0 || js->input_index >= v->mi->nb_streams) {
        add(v, VAL_ERROR, si, "input", "the input has no stream %d (it has %d)", js->input_index,
            v->mi->nb_streams);
        return;
    }
    if (js->action == JOB_DROP || !v->mux)
        return;

    ms    = &v->mi->streams[js->input_index];
    tname = av_get_media_type_string(ms->type);
    tname = tname ? tname : "unknown";

    if (js->action == JOB_COPY) {
        CapsStreamActions act;
        if (caps_stream_actions(v->caps, v->mux, ms, 1, &act) < 0) {
            v->oom = 1;
            return;
        }
        compat = act.copy;
        caps_stream_actions_free(&act);
        if (compat == CAPS_NO)
            add(v, VAL_ERROR, si, "action", "%s cannot store %s %s: transcode this stream or choose "
                "another container", v->mux->key, ms->codec_name, tname);
        else if (compat == CAPS_MAYBE)
            v->maybe_issue[si] = add(v, VAL_WARNING, si, "action",
                "%s has no information about storing %s; the dry run will tell", v->mux->key,
                ms->codec_name);
        return;
    }

    /* transcode */
    if (!(enc = caps_find_encoder(v->caps, js->encoder))) {
        char hint[96];
        Suggest s;
        suggest_init(&s, js->encoder);
        for (int i = 0; i < v->caps->nb_encoders; i++)
            if (v->caps->encoders[i].type == ms->type)
                suggest_try_pref(&s, v->caps->encoders[i].name, v->caps->encoders[i].is_common);
        add(v, VAL_ERROR, si, "encoder", "unknown encoder '%s'%s", js->encoder,
            did_you_mean(&s, hint, sizeof(hint)));
        return;
    }
    if (enc->type != ms->type) {
        add(v, VAL_ERROR, si, "encoder", "%s is a%s %s encoder but input stream %d is %s", enc->name,
            enc->type == AVMEDIA_TYPE_AUDIO ? "n" : "", av_get_media_type_string(enc->type),
            js->input_index, tname);
        return;
    }
    if (ms->type != AVMEDIA_TYPE_VIDEO && ms->type != AVMEDIA_TYPE_AUDIO && ms->type != AVMEDIA_TYPE_SUBTITLE) {
        add(v, VAL_ERROR, si, "action", "%s streams can only be copied or dropped", tname);
        return;
    }
    if (!ms->has_decoder) {
        add(v, VAL_ERROR, si, "action", "this FFmpeg build cannot decode %s: copy or drop the stream",
            ms->codec_name);
        return;
    }
    if (ms->type == AVMEDIA_TYPE_SUBTITLE) {
        int in = ms->is_text_sub ? 1 : ms->is_bitmap_sub ? 2 : 0, out = subtitle_kind(enc->id);
        if (in != out) {
            add(v, VAL_ERROR, si, "encoder", "%s subtitles cannot be converted to %s (%s)",
                in == 1 ? "text" : in == 2 ? "bitmap" : "these", out == 1 ? "text" : "bitmap", enc->name);
            return;
        }
        if (js->nb_filters || js->filter_string)
            add(v, VAL_ERROR, si, "filters", "subtitle streams cannot be filtered");
    }

    compat = caps_mux_codec(v->mux, enc->id);
    if (compat == CAPS_NO)
        add(v, VAL_ERROR, si, "encoder", "%s cannot store %s (from %s): choose another encoder or container",
            v->mux->key, avcodec_get_name(enc->id), enc->name);
    else if (compat == CAPS_MAYBE)
        v->maybe_issue[si] = add(v, VAL_WARNING, si, "encoder",
            "%s has no information about storing %s; the dry run will tell", v->mux->key,
            avcodec_get_name(enc->id));
    if (enc->is_experimental)
        add(v, VAL_WARNING, si, "encoder", "%s is experimental", enc->name);
    if (enc->is_hardware)
        add(v, VAL_INFO, si, "encoder", "%s is a hardware encoder: it needs a matching GPU and driver",
            enc->name);

    {
        ClassSet set = { 0 };
        char owner[96];
        classes_add(&set, enc->codec->priv_class, 1);
        classes_add(&set, avcodec_get_class(), 0);
        snprintf(owner, sizeof(owner), "encoder %s", enc->name);
        check_options(v, si, "options", owner, &set, js->encoder_options);
        if (av_dict_count(js->encoder_options)) {
            AVCodecContext *cc = avcodec_alloc_context3(enc->codec);
            if (cc)
                check_values(v, si, "options", owner, cc, cc->priv_data, 1, js->encoder_options);
            avcodec_free_context(&cc);
        }
    }
    if (ms->type != AVMEDIA_TYPE_SUBTITLE)
        check_filters(v, si, js, ms->type);
}

/* ------------------------------------------------------------------------- */
/* dry run                                                                   */

static void dry_run(V *v)
{
    const ConvJob *job = v->job;
    char err[1024];
    int stream_errors = 0, header_checked = 0;

    v->r->dry_run_done = 1;

    /* each stream alone, so every failing stream is reported */
    for (int i = 0; i < job->nb_streams; i++) {
        ConvJob sub = *job;

        if (job->streams[i].action == JOB_DROP)
            continue;
        sub.streams       = (JobStream *)&job->streams[i];
        sub.nb_streams    = 1;
        sub.muxer_options = NULL;
        if (engine_dry_run(&sub, &header_checked, err, sizeof(err)) < 0) {
            /* the sub-job's own "stream #0: " prefix would be misleading;
             * the issue already names the stream */
            const char *msg = err;
            if (!strncmp(msg, "stream #0: ", 11))
                msg += 11;
            add(v, VAL_ERROR, i, "", "%s", msg);
            stream_errors++;
            continue;
        }
        if (v->maybe_issue[i] >= 0 && header_checked) {
            ValIssue *is = &v->r->issues[v->maybe_issue[i]];
            set_severity(v, v->maybe_issue[i], VAL_INFO);
            snprintf(is->message, sizeof(is->message), "%s accepts this stream (dry run)", v->mux->key);
        }
    }
    if (stream_errors)
        return;

    /* the whole job: stream combination and muxer options */
    if (engine_dry_run(job, &header_checked, err, sizeof(err)) < 0)
        add(v, VAL_ERROR, -1, "", "%s", err);
    else if (!header_checked)
        add(v, VAL_INFO, -1, "muxer", "%s writes its own files: its header and options were not "
            "dry-run", v->mux->key);
}

/* ------------------------------------------------------------------------- */

int validate_job(const ConvJob *job, const Caps *caps, const MediaInfo *mi,
                 unsigned flags, ValReport *report)
{
    V v = { .job = job, .caps = caps, .mi = mi, .r = report };
    MediaInfo *probed = NULL;
    int nb_kept = 0;

    memset(report, 0, sizeof(*report));
    if (job->nb_streams && !(v.maybe_issue = av_malloc_array(job->nb_streams, sizeof(int))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < job->nb_streams; i++)
        v.maybe_issue[i] = -1;

    check_muxer(&v);
    check_paths(&v);

    if (!v.mi && job->input && *job->input) {
        char err[256];
        if (mi_probe(job->input, &probed, err, sizeof(err)) < 0)
            add(&v, VAL_ERROR, -1, "input", "%s: %s", job->input, err);
        v.mi = probed;
    }

    if (!job->nb_streams)
        add(&v, VAL_ERROR, -1, "streams", "the job has no streams");
    for (int i = 0; i < job->nb_streams; i++) {
        nb_kept += job->streams[i].action != JOB_DROP;
        check_stream(&v, i);
    }
    if (job->nb_streams && !nb_kept)
        add(&v, VAL_ERROR, -1, "streams", "every stream is dropped: the output would be empty");

    if ((flags & VALIDATE_DRY_RUN) && !report->nb_errors && !v.oom)
        dry_run(&v);

    mi_free(&probed);
    av_free(v.maybe_issue);
    if (v.oom) {
        validate_report_free(report);
        return AVERROR(ENOMEM);
    }
    return 0;
}

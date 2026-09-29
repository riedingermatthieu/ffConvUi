/*
 * job.c - ConversionJob model and JSON (de)serialisation.
 */
#include "job.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/error.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>

#include "json.h"

#define MAX_JOB_FILE_SIZE (16 * 1024 * 1024)

ConvJob *job_alloc(void)
{
    ConvJob *job = av_mallocz(sizeof(*job));

    if (job) {
        job->copy_metadata = 1;
        job->copy_chapters = 1;
    }
    return job;
}

static void free_stream(JobStream *s)
{
    av_freep(&s->encoder);
    av_dict_free(&s->encoder_options);
    for (int i = 0; i < s->nb_filters; i++) {
        av_freep(&s->filters[i].name);
        av_dict_free(&s->filters[i].options);
    }
    av_freep(&s->filters);
    av_freep(&s->filter_string);
    av_dict_free(&s->metadata);
}

void job_free(ConvJob **pjob)
{
    ConvJob *job = pjob ? *pjob : NULL;

    if (!job)
        return;
    av_freep(&job->input);
    av_freep(&job->output);
    av_freep(&job->muxer);
    av_dict_free(&job->muxer_options);
    av_dict_free(&job->metadata);
    for (int i = 0; i < job->nb_streams; i++)
        free_stream(&job->streams[i]);
    av_freep(&job->streams);
    av_freep(pjob);
}

JobStream *job_add_stream(ConvJob *job, int input_index, JobAction action)
{
    JobStream *tmp = av_realloc_array(job->streams, job->nb_streams + 1, sizeof(*tmp));

    if (!tmp)
        return NULL;
    job->streams = tmp;
    tmp = &job->streams[job->nb_streams++];
    memset(tmp, 0, sizeof(*tmp));
    tmp->input_index = input_index;
    tmp->action      = action;
    return tmp;
}

JobFilter *job_stream_add_filter(JobStream *s, const char *name)
{
    JobFilter *tmp = av_realloc_array(s->filters, s->nb_filters + 1, sizeof(*tmp));

    if (!tmp)
        return NULL;
    s->filters = tmp;
    tmp = &s->filters[s->nb_filters];
    memset(tmp, 0, sizeof(*tmp));
    if (!(tmp->name = av_strdup(name)))
        return NULL;
    s->nb_filters++;
    return tmp;
}

/* length of `path` without its extension (0 if the file name has none) */
static size_t stem_length(const char *path)
{
    const char *slash = strrchr(path, '/'), *bslash = strrchr(path, '\\');
    const char *base = FFMAX(slash, bslash) ? FFMAX(slash, bslash) + 1 : path;
    const char *dot = strrchr(base, '.');

    return dot && dot != base ? (size_t)(dot - path) : strlen(path);
}

char *job_default_output(const char *input, const char *ext)
{
    return av_asprintf("%.*s.converted.%.*s", (int)stem_length(input), input,
                       (int)strcspn(ext, ","), ext);
}

char *job_replace_extension(const char *path, const char *ext)
{
    return av_asprintf("%.*s.%.*s", (int)stem_length(path), path, (int)strcspn(ext, ","), ext);
}

const char *job_action_name(JobAction a)
{
    switch (a) {
    case JOB_COPY:      return "copy";
    case JOB_TRANSCODE: return "transcode";
    default:            return "drop";
    }
}

/* ------------------------------------------------------------------------- */
/* JSON -> job                                                               */

typedef struct Ctx {
    char  *err;
    size_t errlen;
} Ctx;

static int bad(Ctx *c, const char *path, const char *msg)
{
    if (c->err && c->errlen)
        snprintf(c->err, c->errlen, "%s: %s", path, msg);
    return AVERROR(EINVAL);
}

static int expect(Ctx *c, const JsonValue *v, JsonType t, const char *path)
{
    char msg[64];

    if (v->type == t)
        return 0;
    snprintf(msg, sizeof(msg), "expected %s, got %s", json_type_name(t), json_type_name(v->type));
    return bad(c, path, msg);
}

static int get_string(Ctx *c, const JsonValue *obj, const char *key, const char *path,
                      int required, char **out)
{
    const JsonValue *v = json_get(obj, key);
    char p[128];
    int ret;

    snprintf(p, sizeof(p), "%s%s%s", path, *path ? "." : "", key);
    if (!v || v->type == JSON_NULL)
        return required ? bad(c, p, "missing") : 0;
    if ((ret = expect(c, v, JSON_STRING, p)) < 0)
        return ret;
    av_freep(out);
    return (*out = av_strdup(v->string)) ? 0 : AVERROR(ENOMEM);
}

static int get_bool(Ctx *c, const JsonValue *obj, const char *key, int *out)
{
    const JsonValue *v = json_get(obj, key);
    int ret;

    if (!v || v->type == JSON_NULL)
        return 0;
    if ((ret = expect(c, v, JSON_BOOL, key)) < 0)
        return ret;
    *out = v->boolean;
    return 0;
}

/* Option values may be written as strings, numbers or booleans. */
static int value_to_string(Ctx *c, const JsonValue *v, const char *path, char *buf, size_t size)
{
    switch (v->type) {
    case JSON_STRING:
        av_strlcpy(buf, v->string, size);
        return 0;
    case JSON_BOOL:
        av_strlcpy(buf, v->boolean ? "1" : "0", size);
        return 0;
    case JSON_NUMBER:
        if (v->number == floor(v->number) && fabs(v->number) < 1e15)
            snprintf(buf, size, "%.0f", v->number);
        else
            snprintf(buf, size, "%.17g", v->number);
        /* never let a locale's decimal comma reach FFmpeg's option parser */
        for (char *s = buf; *s; s++)
            if (*s == ',')
                *s = '.';
        return 0;
    default:
        return bad(c, path, "option values must be strings, numbers or booleans");
    }
}

static int get_dict(Ctx *c, const JsonValue *obj, const char *key, const char *path,
                    AVDictionary **out)
{
    const JsonValue *v = json_get(obj, key);
    char p[160], buf[4096];
    int ret;

    snprintf(p, sizeof(p), "%s%s%s", path, *path ? "." : "", key);
    if (!v || v->type == JSON_NULL)
        return 0;
    if ((ret = expect(c, v, JSON_OBJECT, p)) < 0)
        return ret;
    for (int i = 0; i < v->nb_items; i++) {
        char ip[256];
        snprintf(ip, sizeof(ip), "%s.%s", p, v->keys[i]);
        if ((ret = value_to_string(c, &v->items[i], ip, buf, sizeof(buf))) < 0)
            return ret;
        if ((ret = av_dict_set(out, v->keys[i], buf, 0)) < 0)
            return ret;
    }
    return 0;
}

static int parse_filters(Ctx *c, const JsonValue *v, JobStream *s, const char *path)
{
    char p[128];
    int ret;

    snprintf(p, sizeof(p), "%s.filters", path);
    if (!v || v->type == JSON_NULL)
        return 0;
    if (v->type == JSON_STRING) {
        if (!*v->string)
            return 0;
        return (s->filter_string = av_strdup(v->string)) ? 0 : AVERROR(ENOMEM);
    }
    if (v->type != JSON_ARRAY)
        return bad(c, p, "expected a filtergraph string or an array of {name, options}");

    for (int i = 0; i < v->nb_items; i++) {
        const JsonValue *f = &v->items[i];
        char fp[160], *name = NULL;
        JobFilter *jf;

        snprintf(fp, sizeof(fp), "%s[%d]", p, i);
        if ((ret = expect(c, f, JSON_OBJECT, fp)) < 0 ||
            (ret = get_string(c, f, "name", fp, 1, &name)) < 0)
            return ret;
        jf = job_stream_add_filter(s, name);
        av_free(name);
        if (!jf)
            return AVERROR(ENOMEM);
        if ((ret = get_dict(c, f, "options", fp, &jf->options)) < 0)
            return ret;
    }
    return 0;
}

static int parse_stream(Ctx *c, const JsonValue *v, ConvJob *job, int idx)
{
    const JsonValue *in;
    char path[32], *action = NULL;
    JobAction act;
    JobStream *s;
    int ret;

    snprintf(path, sizeof(path), "streams[%d]", idx);
    if ((ret = expect(c, v, JSON_OBJECT, path)) < 0)
        return ret;

    in = json_get(v, "input");
    if (!in || in->type != JSON_NUMBER || in->number != floor(in->number) || in->number < 0)
        return bad(c, path, "\"input\" must be the input stream index (integer >= 0)");

    if ((ret = get_string(c, v, "action", path, 1, &action)) < 0)
        return ret;
    if (!strcmp(action, "copy"))           act = JOB_COPY;
    else if (!strcmp(action, "transcode")) act = JOB_TRANSCODE;
    else if (!strcmp(action, "drop"))      act = JOB_DROP;
    else {
        av_free(action);
        return bad(c, path, "\"action\" must be \"copy\", \"transcode\" or \"drop\"");
    }
    av_free(action);

    if (!(s = job_add_stream(job, (int)in->number, act)))
        return AVERROR(ENOMEM);
    if ((ret = get_string(c, v, "encoder", path, act == JOB_TRANSCODE, &s->encoder)) < 0 ||
        (ret = get_dict(c, v, "options", path, &s->encoder_options)) < 0 ||
        (ret = get_dict(c, v, "metadata", path, &s->metadata)) < 0 ||
        (ret = parse_filters(c, json_get(v, "filters"), s, path)) < 0)
        return ret;
    if (act != JOB_TRANSCODE && (s->nb_filters || s->filter_string))
        return bad(c, path, "filters need \"action\": \"transcode\"");
    return 0;
}

int job_from_json(const char *text, ConvJob **out, char *err, size_t errlen)
{
    Ctx c = { err, errlen };
    JsonValue *root = NULL;
    const JsonValue *streams;
    ConvJob *job = NULL;
    char jerr[128];
    int ret;

    *out = NULL;
    if ((ret = json_parse(text, &root, jerr, sizeof(jerr))) < 0) {
        if (err && errlen)
            snprintf(err, errlen, "invalid JSON: %s", jerr);
        return ret;
    }
    if ((ret = expect(&c, root, JSON_OBJECT, "job")) < 0)
        goto end;
    if (!(job = job_alloc())) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = get_string(&c, root, "input", "", 1, &job->input)) < 0 ||
        (ret = get_string(&c, root, "output", "", 1, &job->output)) < 0 ||
        (ret = get_string(&c, root, "muxer", "", 0, &job->muxer)) < 0 ||
        (ret = get_dict(&c, root, "muxer_options", "", &job->muxer_options)) < 0 ||
        (ret = get_dict(&c, root, "metadata", "", &job->metadata)) < 0 ||
        (ret = get_bool(&c, root, "overwrite", &job->overwrite)) < 0 ||
        (ret = get_bool(&c, root, "keep_partial", &job->keep_partial)) < 0 ||
        (ret = get_bool(&c, root, "copy_metadata", &job->copy_metadata)) < 0 ||
        (ret = get_bool(&c, root, "copy_chapters", &job->copy_chapters)) < 0)
        goto end;

    streams = json_get(root, "streams");
    if (!streams || streams->type != JSON_ARRAY || !streams->nb_items) {
        ret = bad(&c, "streams", "must be a non-empty array");
        goto end;
    }
    for (int i = 0; i < streams->nb_items; i++)
        if ((ret = parse_stream(&c, &streams->items[i], job, i)) < 0)
            goto end;
    ret = 0;

end:
    json_free(&root);
    if (ret < 0)
        job_free(&job);
    *out = job;
    return ret;
}

int job_load(const char *path, ConvJob **out, char *err, size_t errlen)
{
    AVIOContext *pb = NULL;
    AVBPrint bp;
    char *text = NULL;
    int ret;

    *out = NULL;
    /* avio handles UTF-8 paths on Windows */
    if ((ret = avio_open(&pb, path, AVIO_FLAG_READ)) < 0) {
        if (err && errlen)
            snprintf(err, errlen, "cannot open job file: %s", av_err2str(ret));
        return ret;
    }
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    ret = avio_read_to_bprint(pb, &bp, MAX_JOB_FILE_SIZE);
    avio_closep(&pb);
    if (ret < 0 || !av_bprint_is_complete(&bp) || av_bprint_finalize(&bp, &text) < 0) {
        if (ret >= 0)
            ret = AVERROR(ENOMEM);
        if (err && errlen)
            snprintf(err, errlen, "cannot read job file: %s", av_err2str(ret));
        av_bprint_finalize(&bp, NULL);
        return ret;
    }
    ret = job_from_json(text, out, err, errlen);
    av_free(text);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* job -> JSON                                                               */

static void write_dict(AVBPrint *bp, const char *key, const AVDictionary *d, const char *indent)
{
    const AVDictionaryEntry *e = NULL;
    int first = 1;

    av_bprintf(bp, ",\n%s", indent);
    json_write_string(bp, key);
    av_bprintf(bp, ": {");
    while ((e = av_dict_iterate(d, e))) {
        av_bprintf(bp, "%s ", first ? "" : ",");
        json_write_string(bp, e->key);
        av_bprintf(bp, ": ");
        json_write_string(bp, e->value);
        first = 0;
    }
    av_bprintf(bp, "%s}", first ? "" : " ");
}

void job_to_json(const ConvJob *job, AVBPrint *bp)
{
    av_bprintf(bp, "{\n  \"input\": ");
    json_write_string(bp, job->input);
    av_bprintf(bp, ",\n  \"output\": ");
    json_write_string(bp, job->output);
    if (job->muxer) {
        av_bprintf(bp, ",\n  \"muxer\": ");
        json_write_string(bp, job->muxer);
    }
    if (av_dict_count(job->muxer_options))
        write_dict(bp, "muxer_options", job->muxer_options, "  ");
    av_bprintf(bp, ",\n  \"overwrite\": %s,\n  \"keep_partial\": %s,"
                   "\n  \"copy_metadata\": %s,\n  \"copy_chapters\": %s",
               job->overwrite ? "true" : "false", job->keep_partial ? "true" : "false",
               job->copy_metadata ? "true" : "false", job->copy_chapters ? "true" : "false");
    if (av_dict_count(job->metadata))
        write_dict(bp, "metadata", job->metadata, "  ");

    av_bprintf(bp, ",\n  \"streams\": [");
    for (int i = 0; i < job->nb_streams; i++) {
        const JobStream *s = &job->streams[i];

        av_bprintf(bp, "%s\n    { \"input\": %d, \"action\": \"%s\"", i ? "," : "",
                   s->input_index, job_action_name(s->action));
        if (s->encoder) {
            av_bprintf(bp, ",\n      \"encoder\": ");
            json_write_string(bp, s->encoder);
        }
        if (av_dict_count(s->encoder_options))
            write_dict(bp, "options", s->encoder_options, "      ");
        if (s->filter_string) {
            av_bprintf(bp, ",\n      \"filters\": ");
            json_write_string(bp, s->filter_string);
        } else if (s->nb_filters) {
            av_bprintf(bp, ",\n      \"filters\": [");
            for (int j = 0; j < s->nb_filters; j++) {
                av_bprintf(bp, "%s\n        { \"name\": ", j ? "," : "");
                json_write_string(bp, s->filters[j].name);
                if (av_dict_count(s->filters[j].options))
                    write_dict(bp, "options", s->filters[j].options, "          ");
                av_bprintf(bp, " }");
            }
            av_bprintf(bp, "\n      ]");
        }
        if (av_dict_count(s->metadata))
            write_dict(bp, "metadata", s->metadata, "      ");
        av_bprintf(bp, " }");
    }
    av_bprintf(bp, "\n  ]\n}\n");
}

/* ------------------------------------------------------------------------- */
/* filter chain rendering                                                    */

/* Filtergraph strings have two escaping levels: option values inside a
 * filter's arguments (':' '=' separate options), and filter descriptions
 * inside the graph (',' ';' '[' ']' separate filters and links). */
static int append_escaped(AVBPrint *bp, const char *s, const char *special)
{
    char *esc = NULL;
    int ret = av_escape(&esc, s, special, AV_ESCAPE_MODE_BACKSLASH, 0);

    if (ret < 0)
        return ret;
    av_bprintf(bp, "%s", esc);
    av_free(esc);
    return 0;
}

void job_stream_filter_string(const JobStream *s, AVBPrint *bp)
{
    if (s->filter_string) {
        av_bprintf(bp, "%s", s->filter_string);
        return;
    }
    for (int i = 0; i < s->nb_filters; i++) {
        const JobFilter *f = &s->filters[i];
        const AVDictionaryEntry *e = NULL;
        AVBPrint desc;
        int first = 1;

        av_bprint_init(&desc, 0, AV_BPRINT_SIZE_UNLIMITED);
        av_bprintf(&desc, "%s", f->name);
        while ((e = av_dict_iterate(f->options, e))) {
            av_bprintf(&desc, "%c%s=", first ? '=' : ':', e->key);
            append_escaped(&desc, e->value, ":=");
            first = 0;
        }
        if (i)
            av_bprint_chars(bp, ',', 1);
        if (av_bprint_is_complete(&desc))
            append_escaped(bp, desc.str, "[],;");
        av_bprint_finalize(&desc, NULL);
    }
}

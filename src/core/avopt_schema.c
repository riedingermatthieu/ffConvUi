/*
 * avopt_schema.c - AVOption metadata -> OptSchema.
 */
#include "avopt_schema.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/avstring.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>

#define SKIP_FLAGS (AV_OPT_FLAG_DEPRECATED | AV_OPT_FLAG_READONLY | AV_OPT_FLAG_EXPORT)

static int keep_option(const AVOption *o, int required_flags)
{
    if (o->type == AV_OPT_TYPE_CONST)
        return 0;
    if (o->flags & SKIP_FLAGS)
        return 0;
    return (o->flags & required_flags) == required_flags;
}

static OptWidget widget_for(enum AVOptionType t, int nb_choices)
{
    switch (t) {
    case AV_OPT_TYPE_INT:
    case AV_OPT_TYPE_INT64:
    case AV_OPT_TYPE_UINT:
    case AV_OPT_TYPE_UINT64:     return nb_choices ? OPT_W_ENUM : OPT_W_INT;
    case AV_OPT_TYPE_FLAGS:      return OPT_W_FLAGS;
    case AV_OPT_TYPE_FLOAT:
    case AV_OPT_TYPE_DOUBLE:     return OPT_W_FLOAT;
    case AV_OPT_TYPE_BOOL:       return OPT_W_BOOL;
    case AV_OPT_TYPE_STRING:     return nb_choices ? OPT_W_ENUM : OPT_W_STRING;
    case AV_OPT_TYPE_DICT:       return OPT_W_DICT;
    case AV_OPT_TYPE_IMAGE_SIZE: return OPT_W_IMAGE_SIZE;
    case AV_OPT_TYPE_VIDEO_RATE: return OPT_W_VIDEO_RATE;
    case AV_OPT_TYPE_RATIONAL:   return OPT_W_RATIONAL;
    case AV_OPT_TYPE_DURATION:   return OPT_W_DURATION;
    case AV_OPT_TYPE_COLOR:      return OPT_W_COLOR;
    case AV_OPT_TYPE_PIXEL_FMT:  return OPT_W_PIXEL_FMT;
    case AV_OPT_TYPE_SAMPLE_FMT: return OPT_W_SAMPLE_FMT;
    case AV_OPT_TYPE_CHLAYOUT:   return OPT_W_CHLAYOUT;
    case AV_OPT_TYPE_BINARY:     return OPT_W_BINARY;
    default:                     return OPT_W_OTHER;
    }
}

static int collect_choices(const AVClass *cls, const char *unit, OptChoice **out, int *nb)
{
    const AVClass *obj = cls;
    const AVOption *o = NULL;
    OptChoice *ch;
    int n = 0, i = 0;

    *out = NULL;
    *nb  = 0;
    if (!unit)
        return 0;

    while ((o = av_opt_next(&obj, o)))
        n += o->type == AV_OPT_TYPE_CONST && o->unit && !strcmp(o->unit, unit);
    if (!n)
        return 0;
    if (!(ch = av_calloc(n, sizeof(*ch))))
        return AVERROR(ENOMEM);

    o = NULL;
    while (i < n && (o = av_opt_next(&obj, o))) {
        if (o->type != AV_OPT_TYPE_CONST || !o->unit || strcmp(o->unit, unit))
            continue;
        ch[i].name  = o->name;
        ch[i].help  = o->help;
        ch[i].value = o->default_val.i64;
        i++;
    }
    *out = ch;
    *nb  = i;
    return 0;
}

static void format_flags(int64_t v, const OptChoice *ch, int nch, char *buf, size_t size)
{
    int64_t rest = v;

    buf[0] = '\0';
    if (!v) {
        av_strlcpy(buf, "0", size);
        return;
    }
    for (int i = 0; i < nch; i++) {
        if (ch[i].value && (v & ch[i].value) == ch[i].value) {
            if (buf[0])
                av_strlcat(buf, "+", size);
            av_strlcat(buf, ch[i].name, size);
            rest &= ~ch[i].value;
        }
    }
    if (rest) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%s0x%" PRIx64, buf[0] ? "+" : "", rest);
        av_strlcat(buf, tmp, size);
    }
}

static void format_default(const AVOption *o, enum AVOptionType t, int is_array,
                           const OptChoice *ch, int nch, char *buf, size_t size)
{
    int64_t v = o->default_val.i64;
    const char *name;

    buf[0] = '\0';
    if (is_array) {
        const AVOptionArrayDef *arr = o->default_val.arr;
        if (arr && arr->def)
            av_strlcpy(buf, arr->def, size);
        return;
    }

    switch (t) {
    case AV_OPT_TYPE_FLAGS:
        format_flags(v, ch, nch, buf, size);
        break;
    case AV_OPT_TYPE_INT:
    case AV_OPT_TYPE_INT64:
    case AV_OPT_TYPE_UINT:
    case AV_OPT_TYPE_UINT64:
        for (int i = 0; i < nch; i++) {
            if (ch[i].value == v) {
                av_strlcpy(buf, ch[i].name, size);
                return;
            }
        }
        if (t == AV_OPT_TYPE_UINT64)
            snprintf(buf, size, "%" PRIu64, (uint64_t)v);
        else
            snprintf(buf, size, "%" PRId64, v);
        break;
    case AV_OPT_TYPE_BOOL:
        av_strlcpy(buf, v < 0 ? "auto" : v ? "true" : "false", size);
        break;
    case AV_OPT_TYPE_FLOAT:
    case AV_OPT_TYPE_DOUBLE:
    case AV_OPT_TYPE_RATIONAL:
        snprintf(buf, size, "%g", o->default_val.dbl);
        break;
    case AV_OPT_TYPE_DURATION:
        snprintf(buf, size, "%gs", v / 1000000.0);
        break;
    case AV_OPT_TYPE_PIXEL_FMT:
        name = av_get_pix_fmt_name((enum AVPixelFormat)v);
        av_strlcpy(buf, name ? name : "none", size);
        break;
    case AV_OPT_TYPE_SAMPLE_FMT:
        name = av_get_sample_fmt_name((enum AVSampleFormat)v);
        av_strlcpy(buf, name ? name : "none", size);
        break;
    case AV_OPT_TYPE_STRING:
    case AV_OPT_TYPE_DICT:
    case AV_OPT_TYPE_IMAGE_SIZE:
    case AV_OPT_TYPE_VIDEO_RATE:
    case AV_OPT_TYPE_COLOR:
    case AV_OPT_TYPE_CHLAYOUT:
    case AV_OPT_TYPE_BINARY:
        if (o->default_val.str)
            av_strlcpy(buf, o->default_val.str, size);
        break;
    default:
        break;
    }
}

#define MAX_CLASSES 32

/* cls followed by its child classes, depth first, without duplicates */
static int gather_classes(const AVClass *cls, const AVClass **list, int n)
{
    const AVClass *child;
    void *iter = NULL;

    if (!cls || n >= MAX_CLASSES)
        return n;
    for (int i = 0; i < n; i++)
        if (list[i] == cls)
            return n;
    list[n++] = cls;
    if (cls->child_class_iterate)
        while ((child = cls->child_class_iterate(&iter)))
            n = gather_classes(child, list, n);
    return n;
}

static int fill_schema(OptSchemaList *out, const AVClass *cls, const AVOption *o, int first_of_class)
{
    OptSchema *s = &out->opts[out->nb_opts++];
    int ret;

    s->opt      = o;
    s->owner    = cls;
    s->name     = o->name;
    s->help     = o->help ? o->help : "";
    s->unit     = o->unit;
    s->is_array = !!(o->type & AV_OPT_TYPE_FLAG_ARRAY);
    s->type     = o->type & ~AV_OPT_TYPE_FLAG_ARRAY;
    s->flags    = o->flags;
    s->min      = o->min;
    s->max      = o->max;

    if ((ret = collect_choices(cls, o->unit, &s->choices, &s->nb_choices)) < 0)
        return ret;
    s->widget = widget_for(s->type, s->nb_choices);
    format_default(o, s->type, s->is_array, s->choices, s->nb_choices,
                   s->default_str, sizeof(s->default_str));

    /* aliases write the same field of the same struct; offset 0 is the
     * AVClass pointer, so a real option never has it */
    for (int j = first_of_class; j < out->nb_opts - 1 && o->offset > 0; j++) {
        const AVOption *p = out->opts[j].opt;
        if (p->offset == o->offset && p->type == o->type && !out->opts[j].alias_of) {
            s->alias_of = p->name;
            break;
        }
    }
    return 0;
}

int optschema_from_class(const AVClass *cls, int required_flags, int include_children,
                         OptSchemaList *out)
{
    const AVClass *classes[MAX_CLASSES];
    int nb_classes, n = 0, ret;

    memset(out, 0, sizeof(*out));
    if (!cls)
        return 0;
    nb_classes = include_children ? gather_classes(cls, classes, 0) : 1;
    classes[0] = cls;

    /* av_opt_next() expects a pointer to a struct whose first member is an
     * AVClass pointer; a pointer to an AVClass pointer satisfies that. */
    for (int c = 0; c < nb_classes; c++) {
        const AVClass *obj = classes[c];
        const AVOption *o = NULL;
        while ((o = av_opt_next(&obj, o)))
            n += keep_option(o, required_flags);
    }
    if (!n)
        return 0;
    if (!(out->opts = av_calloc(n, sizeof(*out->opts))))
        return AVERROR(ENOMEM);

    for (int c = 0; c < nb_classes; c++) {
        const AVClass *obj = classes[c];
        const AVOption *o = NULL;
        int first = out->nb_opts;

        while (out->nb_opts < n && (o = av_opt_next(&obj, o))) {
            if (!keep_option(o, required_flags))
                continue;
            if ((ret = fill_schema(out, classes[c], o, first)) < 0) {
                optschema_free(out);
                return ret;
            }
        }
    }
    return 0;
}

void optschema_free(OptSchemaList *list)
{
    if (!list)
        return;
    for (int i = 0; i < list->nb_opts; i++)
        av_free(list->opts[i].choices);
    av_freep(&list->opts);
    list->nb_opts = 0;
}

const char *optschema_widget_name(OptWidget w)
{
    static const char *const names[] = {
        [OPT_W_INT] = "int", [OPT_W_FLOAT] = "float", [OPT_W_BOOL] = "bool",
        [OPT_W_ENUM] = "enum", [OPT_W_FLAGS] = "flags", [OPT_W_STRING] = "string",
        [OPT_W_DICT] = "dict", [OPT_W_IMAGE_SIZE] = "image_size",
        [OPT_W_VIDEO_RATE] = "video_rate", [OPT_W_RATIONAL] = "rational",
        [OPT_W_DURATION] = "duration", [OPT_W_COLOR] = "color",
        [OPT_W_PIXEL_FMT] = "pix_fmt", [OPT_W_SAMPLE_FMT] = "sample_fmt",
        [OPT_W_CHLAYOUT] = "ch_layout", [OPT_W_BINARY] = "binary",
        [OPT_W_OTHER] = "other",
    };
    return (unsigned)w < sizeof(names) / sizeof(names[0]) && names[w] ? names[w] : "other";
}

const char *optschema_type_name(enum AVOptionType t)
{
    switch (t) {
    case AV_OPT_TYPE_FLAGS:      return "flags";
    case AV_OPT_TYPE_INT:        return "int";
    case AV_OPT_TYPE_INT64:      return "int64";
    case AV_OPT_TYPE_UINT:       return "uint";
    case AV_OPT_TYPE_UINT64:     return "uint64";
    case AV_OPT_TYPE_DOUBLE:     return "double";
    case AV_OPT_TYPE_FLOAT:      return "float";
    case AV_OPT_TYPE_STRING:     return "string";
    case AV_OPT_TYPE_RATIONAL:   return "rational";
    case AV_OPT_TYPE_BINARY:     return "binary";
    case AV_OPT_TYPE_DICT:       return "dictionary";
    case AV_OPT_TYPE_IMAGE_SIZE: return "image_size";
    case AV_OPT_TYPE_PIXEL_FMT:  return "pix_fmt";
    case AV_OPT_TYPE_SAMPLE_FMT: return "sample_fmt";
    case AV_OPT_TYPE_VIDEO_RATE: return "video_rate";
    case AV_OPT_TYPE_DURATION:   return "duration";
    case AV_OPT_TYPE_COLOR:      return "color";
    case AV_OPT_TYPE_BOOL:       return "boolean";
    case AV_OPT_TYPE_CHLAYOUT:   return "ch_layout";
    default:                     return "unknown";
    }
}

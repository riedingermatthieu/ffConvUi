/*
 * avopt_schema.h - turn FFmpeg AVOption metadata into a toolkit-neutral
 * schema, so option editors can be generated for any encoder, muxer or
 * filter without hard-coding their options.
 */
#ifndef CONV_AVOPT_SCHEMA_H
#define CONV_AVOPT_SCHEMA_H

#include <libavutil/log.h>
#include <libavutil/opt.h>

/* Which widget an editor should use for an option. */
typedef enum OptWidget {
    OPT_W_INT,          /* spin button */
    OPT_W_FLOAT,        /* spin button with decimals */
    OPT_W_BOOL,         /* checkbox (tri-state when the default is "auto") */
    OPT_W_ENUM,         /* dropdown of named constants; numbers also accepted */
    OPT_W_FLAGS,        /* group of checkboxes */
    OPT_W_STRING,
    OPT_W_DICT,         /* key=value:key=value */
    OPT_W_IMAGE_SIZE,   /* WxH or abbreviation (hd720...) */
    OPT_W_VIDEO_RATE,   /* 25, 30000/1001, ntsc... */
    OPT_W_RATIONAL,
    OPT_W_DURATION,     /* [-][HH:]MM:SS[.m...] or seconds */
    OPT_W_COLOR,
    OPT_W_PIXEL_FMT,
    OPT_W_SAMPLE_FMT,
    OPT_W_CHLAYOUT,
    OPT_W_BINARY,       /* hex string */
    OPT_W_OTHER,
} OptWidget;

typedef struct OptChoice {
    const char *name;
    const char *help;   /* may be NULL */
    int64_t     value;
} OptChoice;

typedef struct OptSchema {
    const AVOption    *opt;
    const AVClass     *owner;        /* class the option belongs to (cls or a child) */
    const char        *name;
    const char        *help;         /* never NULL */
    const char        *unit;         /* may be NULL */
    enum AVOptionType  type;         /* base type, AV_OPT_TYPE_FLAG_ARRAY stripped */
    int                is_array;
    OptWidget          widget;
    int                flags;        /* AV_OPT_FLAG_* */
    double             min, max;
    char               default_str[128];
    OptChoice         *choices;      /* named constants of `unit` */
    int                nb_choices;
    const char        *alias_of;     /* earlier option writing the same field
                                        (w/width, s/size...), or NULL; editors
                                        should show only the first one */
} OptSchema;

typedef struct OptSchemaList {
    OptSchema *opts;
    int        nb_opts;
} OptSchemaList;

/*
 * Build the schema of every option of `cls` and, if include_children is set,
 * of its child classes (e.g. the framesync options of multi-input filters).
 * Never set it for the generic codec/format classes: their children are the
 * private classes of every codec/format.
 * required_flags: only keep options having all of these AV_OPT_FLAG_* bits
 * (0 = keep all). Constants, deprecated, read-only and export-only options
 * are always skipped. `cls` may be NULL (empty list).
 */
int  optschema_from_class(const AVClass *cls, int required_flags, int include_children,
                          OptSchemaList *out);
void optschema_free(OptSchemaList *list);

const char *optschema_widget_name(OptWidget w);
const char *optschema_type_name(enum AVOptionType t);

#endif /* CONV_AVOPT_SCHEMA_H */

/*
 * job.h - ConversionJob: everything the user chose, as plain data.
 *
 * A job can be built in code (the UI) or loaded from / saved to JSON:
 *
 * {
 *   "input":  "in.mkv",
 *   "output": "out.mp4",
 *   "muxer":  "mp4",                        // optional, guessed from output
 *   "muxer_options": { "movflags": "+faststart" },
 *   "overwrite": false, "keep_partial": false,
 *   "copy_metadata": true, "copy_chapters": true,
 *   "metadata": { "title": "My film" },     // "" removes a key
 *   "streams": [
 *     { "input": 0, "action": "transcode", "encoder": "libx265",
 *       "options": { "crf": 26, "preset": "fast" },
 *       "filters": [ { "name": "scale", "options": { "w": 1280, "h": -2 } },
 *                    { "name": "fps", "options": { "fps": 25 } } ] },
 *     { "input": 1, "action": "transcode", "encoder": "aac",
 *       "options": { "b": "160k" }, "filters": "aresample=48000" },
 *     { "input": 2, "action": "copy", "metadata": { "language": "fra" } }
 *   ]
 * }
 *
 * Input streams not listed are dropped. "filters" is either a list of
 * {name, options} or a raw filtergraph string.
 */
#ifndef CONV_JOB_H
#define CONV_JOB_H

#include <stddef.h>

#include <libavutil/bprint.h>
#include <libavutil/dict.h>

typedef enum JobAction {
    JOB_DROP,
    JOB_COPY,
    JOB_TRANSCODE,
} JobAction;

typedef struct JobFilter {
    char         *name;
    AVDictionary *options;       /* in insertion order */
} JobFilter;

typedef struct JobStream {
    int           input_index;
    JobAction     action;
    char         *encoder;       /* JOB_TRANSCODE */
    AVDictionary *encoder_options;
    JobFilter    *filters;       /* used when filter_string is NULL */
    int           nb_filters;
    char         *filter_string; /* raw filtergraph, overrides `filters` */
    AVDictionary *metadata;      /* stream metadata overrides */
} JobStream;

typedef struct ConvJob {
    char         *input;
    char         *output;
    char         *muxer;         /* caps key ("mp4", "matroska:mka"), NULL = guess */
    AVDictionary *muxer_options;
    int           overwrite;
    int           keep_partial;  /* keep the output file on error/cancel */
    int           copy_metadata;
    int           copy_chapters;
    AVDictionary *metadata;      /* global metadata overrides */
    JobStream    *streams;
    int           nb_streams;
} ConvJob;

ConvJob   *job_alloc(void);
void       job_free(ConvJob **job);
JobStream *job_add_stream(ConvJob *job, int input_index, JobAction action);
JobFilter *job_stream_add_filter(JobStream *s, const char *name);

int job_from_json(const char *text, ConvJob **out, char *err, size_t errlen);
int job_load(const char *path, ConvJob **out, char *err, size_t errlen);
/* Appends pretty-printed JSON to bp. */
void job_to_json(const ConvJob *job, AVBPrint *bp);

/* The stream's filter chain as a filtergraph string (empty if none).
 * Option values are escaped for both filtergraph levels. */
void job_stream_filter_string(const JobStream *s, AVBPrint *bp);

const char *job_action_name(JobAction a);

/* "dir/name.mkv" + extension "mp4" -> "dir/name.converted.mp4" (av_free it).
 * `ext` may be a comma-separated list: its first entry is used. */
char *job_default_output(const char *input, const char *ext);

/* `path` with its extension replaced by the first entry of `ext` (av_free it). */
char *job_replace_extension(const char *path, const char *ext);

#endif /* CONV_JOB_H */

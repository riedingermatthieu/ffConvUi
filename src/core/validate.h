/*
 * validate.h - check a ConvJob before running it.
 *
 * Two passes:
 *  - static (fast, for live feedback while editing): input/output paths,
 *    stream indices, container <-> codec compatibility, encoder and filter
 *    names, option names (with "did you mean" suggestions), subtitle kinds;
 *  - dry run (VALIDATE_DRY_RUN): each transcoded/copied stream is set up
 *    alone with engine_dry_run() (decoder, filters, encoder with the real
 *    option values, muxer header), then the whole job with the muxer options.
 *    This settles the static pass's "maybe" results and catches constraints
 *    only enforced when an encoder opens (e.g. libopus + 5.1(side)).
 *
 * Every problem found is reported, not just the first.
 */
#ifndef CONV_VALIDATE_H
#define CONV_VALIDATE_H

#include "caps.h"
#include "job.h"
#include "probe.h"

typedef enum ValSeverity {
    VAL_INFO,
    VAL_WARNING,
    VAL_ERROR,
} ValSeverity;

typedef struct ValIssue {
    ValSeverity severity;
    int         stream;       /* index in job->streams, -1 for job-level issues */
    char        field[64];    /* "output", "muxer_options.movflags", "encoder",
                                 "options.crf", "filters[1].w"... or "" */
    char        message[768];
} ValIssue;

typedef struct ValReport {
    ValIssue *issues;
    int       nb_issues;
    int       nb_errors;
    int       nb_warnings;
    int       dry_run_done;   /* the dry-run pass ran (only when there were no static
                                 errors, so any error then comes from the dry run) */
} ValReport;

enum {
    VALIDATE_DRY_RUN = 1 << 0,
};

/*
 * caps: catalog to check against (required).
 * mi:   the job's input, already probed, or NULL to probe it here.
 * Returns 0 when the report was produced (check report->nb_errors), or a
 * negative AVERROR on allocation failure. Free with validate_report_free().
 */
int  validate_job(const ConvJob *job, const Caps *caps, const MediaInfo *mi,
                  unsigned flags, ValReport *report);
void validate_report_free(ValReport *report);

const char *validate_severity_name(ValSeverity s);

#endif /* CONV_VALIDATE_H */

/*
 * cmdline.c - ConvJob -> equivalent ffmpeg command line.
 */
#include "cmdline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/avstring.h>
#include <libavutil/avutil.h>

/* ------------------------------------------------------------------------- */
/* quoting                                                                   */

static int is_safe(unsigned char c, CmdShell sh)
{
    const char *extra;

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return 1;
    switch (sh) {
    /* PowerShell: ',' builds an array and splits the argument, '@' '#' '$'... */
    case CMD_SHELL_POWERSHELL: extra = "_-+=:./\\";   break;
    case CMD_SHELL_CMD:        extra = "_-+=:,./\\";  break;
    default:                   extra = "_-+=:,./%@^"; break;
    }
    return c && strchr(extra, c) != NULL;
}

static void append_arg(AVBPrint *bp, const char *s, CmdShell sh)
{
    int quote = !*s;

    for (const char *p = s; *p && !quote; p++)
        quote = !is_safe((unsigned char)*p, sh);

    av_bprint_chars(bp, ' ', 1);
    if (!quote) {
        av_bprintf(bp, "%s", s);
        return;
    }

    switch (sh) {
    case CMD_SHELL_POWERSHELL:          /* '...' is literal; ' is doubled */
        av_bprint_chars(bp, '\'', 1);
        for (const char *p = s; *p; p++) {
            if (*p == '\'')
                av_bprint_chars(bp, '\'', 1);
            av_bprint_chars(bp, *p, 1);
        }
        av_bprint_chars(bp, '\'', 1);
        break;
    case CMD_SHELL_CMD: {               /* the argv rules of CommandLineToArgvW */
        int backslashes = 0;
        av_bprint_chars(bp, '"', 1);
        for (const char *p = s;; p++) {
            if (*p == '\\') {
                backslashes++;
                continue;
            }
            if (!*p) {                  /* double them before the closing quote */
                av_bprint_chars(bp, '\\', backslashes * 2);
                break;
            }
            if (*p == '"') {
                av_bprint_chars(bp, '\\', backslashes * 2 + 1);
            } else {
                av_bprint_chars(bp, '\\', backslashes);
            }
            av_bprint_chars(bp, *p, 1);
            backslashes = 0;
        }
        av_bprint_chars(bp, '"', 1);
        break;
    }
    default:                            /* '...' is literal; ' becomes '\'' */
        av_bprint_chars(bp, '\'', 1);
        for (const char *p = s; *p; p++) {
            if (*p == '\'')
                av_bprintf(bp, "'\\''");
            else
                av_bprint_chars(bp, *p, 1);
        }
        av_bprint_chars(bp, '\'', 1);
        break;
    }
}

/* "-name:spec value" */
static void append_option(AVBPrint *bp, const char *name, const char *spec, const char *value, CmdShell sh)
{
    char flag[160];

    snprintf(flag, sizeof(flag), "-%s%s%s", name, spec ? ":" : "", spec ? spec : "");
    append_arg(bp, flag, sh);
    append_arg(bp, value, sh);
}

/* ------------------------------------------------------------------------- */
/* Quality: global_quality + qscale flag -> -q                               */

/* `flags` without its "qscale" token ("qscale+loop" -> "loop"); NULL if it
 * did not enable qscale. */
static char *strip_qscale(const char *flags)
{
    char **tok = av_malloc_array(strlen(flags) + 1, sizeof(*tok));
    char *copy = av_strdup(flags), *save = NULL, *t, *out = NULL;
    AVBPrint bp;
    int found = 0, n = 0;

    if (!tok || !copy)
        goto end;
    for (t = av_strtok(copy, "+", &save); t; t = av_strtok(NULL, "+", &save)) {
        if (!strcmp(t, "qscale"))
            found = 1;
        else
            tok[n++] = t;
    }
    if (found) {
        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        if (flags[0] == '+' && n)
            av_bprint_chars(&bp, '+', 1);
        for (int i = 0; i < n; i++)
            av_bprintf(&bp, "%s%s", i ? "+" : "", tok[i]);
        av_bprint_finalize(&bp, &out);
    }
end:
    av_free(tok);
    av_free(copy);
    return out;
}

static void append_encoder_options(AVBPrint *bp, const AVDictionary *opts, const char *spec, CmdShell sh)
{
    const AVDictionaryEntry *gq = av_dict_get(opts, "global_quality", NULL, 0);
    const AVDictionaryEntry *fl = av_dict_get(opts, "flags", NULL, 0);
    const AVDictionaryEntry *e = NULL;
    char *flags_left = gq && fl ? strip_qscale(fl->value) : NULL;
    int as_q = flags_left != NULL;

    if (as_q) {
        char q[32];
        snprintf(q, sizeof(q), "%g", atof(gq->value) / FF_QP2LAMBDA);
        for (char *c = q; *c; c++)       /* whatever the locale */
            if (*c == ',')
                *c = '.';
        append_option(bp, "q", spec, q, sh);
    }
    while ((e = av_dict_iterate(opts, e))) {
        if (as_q && e == gq)
            continue;
        if (as_q && e == fl) {
            if (*flags_left)
                append_option(bp, "flags", spec, flags_left, sh);
            continue;
        }
        append_option(bp, e->key, spec, e->value, sh);
    }
    av_free(flags_left);
}

/* ------------------------------------------------------------------------- */

static char type_letter(enum AVMediaType t)
{
    switch (t) {
    case AVMEDIA_TYPE_VIDEO:      return 'v';
    case AVMEDIA_TYPE_AUDIO:      return 'a';
    case AVMEDIA_TYPE_SUBTITLE:   return 's';
    case AVMEDIA_TYPE_DATA:       return 'd';
    case AVMEDIA_TYPE_ATTACHMENT: return 't';
    default:                      return 0;
    }
}

static void append_metadata(AVBPrint *bp, const AVDictionary *md, const char *spec, CmdShell sh)
{
    const AVDictionaryEntry *e = NULL;

    while ((e = av_dict_iterate(md, e))) {
        char *kv = av_asprintf("%s=%s", e->key, e->value);   /* "key=" removes it */
        char flag[64];
        if (!kv)
            return;
        if (spec)
            snprintf(flag, sizeof(flag), "-metadata:s:%s", spec);
        else
            av_strlcpy(flag, "-metadata", sizeof(flag));
        append_arg(bp, flag, sh);
        append_arg(bp, kv, sh);
        av_free(kv);
    }
}

void job_to_ffmpeg_command(const ConvJob *job, const MediaInfo *mi, CmdShell sh, AVBPrint *bp)
{
    int per_type[256] = { 0 }, out_index = 0;

    av_bprintf(bp, "ffmpeg");
    append_arg(bp, job->overwrite ? "-y" : "-n", sh);
    append_arg(bp, "-i", sh);
    append_arg(bp, job->input ? job->input : "", sh);
    if (!job->copy_metadata)
        av_bprintf(bp, " -map_metadata -1");
    if (!job->copy_chapters)
        av_bprintf(bp, " -map_chapters -1");

    for (int i = 0; i < job->nb_streams; i++) {
        const JobStream *js = &job->streams[i];
        char map[32], spec[32];
        char t = 0;

        if (js->action == JOB_DROP)
            continue;
        snprintf(map, sizeof(map), "0:%d", js->input_index);
        append_arg(bp, "-map", sh);
        append_arg(bp, map, sh);

        if (mi && js->input_index >= 0 && js->input_index < mi->nb_streams)
            t = type_letter(mi->streams[js->input_index].type);
        if (t)
            snprintf(spec, sizeof(spec), "%c:%d", t, per_type[(unsigned char)t]++);
        else
            snprintf(spec, sizeof(spec), "%d", out_index);
        out_index++;

        if (js->action == JOB_COPY) {
            append_option(bp, "c", spec, "copy", sh);
        } else {
            AVBPrint chain;
            append_option(bp, "c", spec, js->encoder ? js->encoder : "", sh);
            append_encoder_options(bp, js->encoder_options, spec, sh);
            av_bprint_init(&chain, 0, AV_BPRINT_SIZE_UNLIMITED);
            job_stream_filter_string(js, &chain);
            if (chain.len)
                append_option(bp, "filter", spec, chain.str, sh);
            av_bprint_finalize(&chain, NULL);
        }
        append_metadata(bp, js->metadata, spec, sh);
    }

    append_metadata(bp, job->metadata, NULL, sh);
    {
        const AVDictionaryEntry *e = NULL;
        while ((e = av_dict_iterate(job->muxer_options, e)))
            append_option(bp, e->key, NULL, e->value, sh);
    }
    if (job->muxer) {
        char name[64];
        /* "matroska:mka" -> "matroska" (the key's suffix only disambiguates) */
        av_strlcpy(name, job->muxer, FFMIN(sizeof(name), strcspn(job->muxer, ":") + 1));
        append_arg(bp, "-f", sh);
        append_arg(bp, name, sh);
    }
    append_arg(bp, job->output ? job->output : "", sh);
}

CmdShell cmd_default_shell(void)
{
#ifdef _WIN32
    return CMD_SHELL_POWERSHELL;
#else
    return CMD_SHELL_POSIX;
#endif
}

const char *cmd_shell_name(CmdShell sh)
{
    switch (sh) {
    case CMD_SHELL_POWERSHELL: return "powershell";
    case CMD_SHELL_CMD:        return "cmd";
    default:                   return "bash";
    }
}

int cmd_shell_from_name(const char *name, CmdShell *sh)
{
    if (!strcmp(name, "bash") || !strcmp(name, "posix") || !strcmp(name, "sh"))
        *sh = CMD_SHELL_POSIX;
    else if (!strcmp(name, "powershell") || !strcmp(name, "pwsh"))
        *sh = CMD_SHELL_POWERSHELL;
    else if (!strcmp(name, "cmd"))
        *sh = CMD_SHELL_CMD;
    else
        return -1;
    return 0;
}

/*
 * cmdline.h - the ffmpeg command line equivalent to a ConvJob.
 *
 *   ffmpeg -y -i in.mkv -map 0:0 -c:v:0 libx265 -crf:v:0 28 -filter:v:0 scale=w=1280:h=-2
 *          -map 0:1 -c:a:0 aac -b:a:0 192k -movflags +faststart -f mp4 out.mp4
 *
 * Per-stream options use output stream specifiers (v:0, a:1...), counted in
 * the job's stream order. Quality (global_quality + the qscale flag) is
 * written as the CLI's -q. Arguments are quoted for the chosen shell.
 */
#ifndef CONV_CMDLINE_H
#define CONV_CMDLINE_H

#include <libavutil/bprint.h>

#include "job.h"
#include "probe.h"

typedef enum CmdShell {
    CMD_SHELL_POSIX,        /* bash, zsh, sh, MSYS2 */
    CMD_SHELL_POWERSHELL,
    CMD_SHELL_CMD,          /* Windows cmd.exe */
} CmdShell;

/*
 * mi: the job's input, to name streams by type (-c:v:0). May be NULL: output
 * stream indices are used instead (-c:0), which ffmpeg accepts as well.
 * Appends the command to bp (no trailing newline).
 */
void job_to_ffmpeg_command(const ConvJob *job, const MediaInfo *mi, CmdShell shell, AVBPrint *bp);

CmdShell    cmd_default_shell(void);             /* PowerShell on Windows, POSIX elsewhere */
const char *cmd_shell_name(CmdShell shell);      /* "bash", "powershell", "cmd" */
int         cmd_shell_from_name(const char *name, CmdShell *shell);

#endif /* CONV_CMDLINE_H */

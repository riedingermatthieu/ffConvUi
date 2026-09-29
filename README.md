# ffconv: FFmpeg converter (M1–M6)

A C11 library (`convcore`) that uses the FFmpeg API to describe an input file,
list every conversion the linked FFmpeg build can do with it, check and run a
conversion; a headless CLI (`convcli`); and a GTK 4 user interface (`ffconv`)
built on the same library.

| Module | Milestone | What it does |
|---|---|---|
| `src/core/probe.*` | M1 | `MediaInfo` snapshot of an input: container, streams, codec/profile, video geometry/color/HDR/rotation, audio layout, subtitle kind, chapters, tags. Owns its memory, holds no FFmpeg contexts. |
| `src/core/caps.*` | M2 | Capability catalog: every muxer, encoder (with supported pixel/sample formats, rates, layouts) and filter (classified simple/hw/convert/multi-in/multi-out/source/sink). Narrowing queries: containers for a file, per-stream copy/transcode choices for a container, filters for a media type, usable hardware devices. |
| `src/core/avopt_schema.*` | M2 | Converts `AVOption` metadata (type, default, range, named constants, flags, aliases, child classes) into a toolkit-neutral schema from which option editors can be generated. |
| `src/core/job.*`, `json.*` | M3 | `ConvJob`: the user's choices as plain data (container, per-stream copy/transcode/drop, encoder options, filter chains, metadata), loaded from / saved to JSON. Small built-in JSON parser, locale-independent. |
| `src/core/engine.*` | M3 | Runs a job: demux → decode → filter → encode → mux, or stream copy. Progress callback, cancellation from another thread, dry run. |
| `src/core/validate.*` | M4 | Checks a job before running it: static checks, then a dry run; reports every problem with its stream and field. |
| `src/core/logcap.*` | M4 | Captures FFmpeg's log per thread, so errors carry FFmpeg's own reason. |
| `src/core/cmdline.*` | — | The `ffmpeg` command line equivalent to a job, quoted for Bash, PowerShell or cmd. |
| `src/cli/convcli.c` | M1–M4 | Test front end. |
| `src/ui/*` | M5–M6 | GTK 4 application: main window, stream rows, option editor, progress window, FFmpeg log view. |

## Build (Windows, MSYS2 MINGW64)

Requirements: CMake ≥ 3.20, a C11 compiler, pkg-config, and **FFmpeg ≥ 7.1**
development files (tested with FFmpeg 8.1).

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
                   mingw-w64-x86_64-pkgconf mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-gtk4
export PATH=/c/msys64/mingw64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
cd build && ctest
```

The UI (`build/ffconv.exe`) is built when GTK ≥ 4.12 is found. Run it from the
MSYS2 MINGW64 shell (it needs the FFmpeg and GTK DLLs from `mingw64/bin`):

```sh
./build/ffconv.exe [file]
```

`-DCONV_GUI_TESTS=ON` adds four UI tests (`ctest -L gui`); they open windows.

The tests use the synthetic files in [tests/media](tests/media) and the jobs in
[tests/jobs](tests/jobs); see [tests/README.md](tests/README.md) for what each
checks and how to regenerate the media. `-DCONV_TEST_MEDIA=/path/to/file`
runs the probe/end-to-end tests on another file. The VS Code workspace has an
"MSYS2 MINGW64" terminal profile (default) with this environment.

On Linux/macOS the same commands work with the distribution's FFmpeg `-dev` packages.

## CLI

```text
convcli version [--config]
convcli probe <file>
convcli muxers   [--for <file>] [--common] [--search <text>]
convcli encoders [--type video|audio|subtitle] [--muxer <key>] [--common] [--search <text>]
convcli actions  <file> --muxer <key> [--experimental] [--all]
convcli filters  [--type video|audio] [--class simple,hw,convert,multi-in,multi-out,source,sink|all] [--search <text>]
convcli options  encoder|muxer|filter <name> [--generic]
convcli hw
convcli job-template <file> [--muxer <key>] [--output <file>]
convcli run <job.json> [--overwrite] [--quiet] [--cancel-after <seconds>]
convcli validate <job.json> [--static]
convcli command <job.json> [--shell bash|powershell|cmd]
```

Examples:

```sh
convcli muxers --for movie.mkv --common           # containers that can keep its streams
convcli actions movie.mkv --muxer webm            # per stream: copy? which encoders?
convcli options encoder libx264                   # formats + option schema
convcli job-template movie.mkv --output movie.mp4 > job.json   # starting point
convcli run job.json                              # Ctrl+C cancels cleanly
```

## Job files

See [examples/mkv_to_mp4_hevc.json](examples/mkv_to_mp4_hevc.json) and the
comment at the top of [src/core/job.h](src/core/job.h). In short:

```jsonc
{
  "input": "in.mkv", "output": "out.mp4",
  "muxer": "mp4",                              // optional, guessed from the output name
  "muxer_options": { "movflags": "+faststart" },
  "overwrite": false, "keep_partial": false,
  "copy_metadata": true, "copy_chapters": true,
  "metadata": { "title": "My film" },          // "" removes a key
  "streams": [
    { "input": 0, "action": "transcode", "encoder": "libx265",
      "options": { "crf": 26, "preset": "fast" },
      "filters": [ { "name": "scale", "options": { "w": 1280, "h": -2 } } ] },
    { "input": 1, "action": "transcode", "encoder": "aac", "filters": "aresample=48000" },
    { "input": 2, "action": "copy", "metadata": { "language": "fra" } }
  ]
}
```

* Input streams not listed are dropped. The output keeps the job's stream order.
* `options` are FFmpeg AVOption names, generic (`b`, `g`, `threads`...) or
  private to the encoder (`crf`, `preset`...). Unknown options are errors.
  Values can be strings, numbers or booleans.
* `filters` is either a list of `{name, options}` (escaped automatically) or a
  raw filtergraph string.

## How the engine works

Based on FFmpeg's `doc/examples/transcode.c`, with these differences:

* **Filters before encoders.** Each stream's filter graph is configured first,
  and the encoder is opened with the graph's output size, format, rate and
  layout. The graph ends with the encoder's supported formats
  (`format=pix_fmts=…`, `aformat=…`), so FFmpeg negotiates a format the
  encoder accepts and inserts conversions (downmix, resample, pixel format)
  automatically.
* **Mid-stream changes.** If decoded frames change size or format, the graph
  is rebuilt with a tail matching the already-open encoder.
* **Timestamps.** CFR video (or a chain containing `fps`) is encoded with a
  1/fps time base, VFR keeps its exact timestamps. Audio runs in 1/sample_rate
  and stays sample-contiguous across container rounding (Matroska stores
  milliseconds), like the `ffmpeg` CLI. Encoders that reject the time base
  (mpeg4...) are retried with 1/fps.
* **Fixed frame-size audio encoders** (aac, opus...) get exactly-sized frames.
* **Subtitles** are decoded and re-encoded (text→text, bitmap→bitmap), keeping
  the decoder's ASS header for text encoders.
* **Rotation** metadata is kept on transcoded video (decoders don't rotate pixels).
* **Cancellation** stops reading, flushes, writes a valid trailer, then deletes
  the output unless `keep_partial`. Errors also delete the output unless
  `keep_partial`.

## Validation

`convcli validate job.json` (or `validate_job()`) reports every problem, each
with a severity, the stream and the field it concerns:

```text
error    #0 options.crff        encoder libx264 has no option 'crff' (did you mean 'crf'?)
error    #1                     stream #0: cannot open encoder libopus: Invalid argument (libopus: Invalid channel layout 5.1(side) for specified mapping family -1.)
warning  #0 encoder             vorbis is experimental
info     #0 action              mpegts accepts this stream (dry run)
Result: 2 error(s), 1 warning(s), dry run failed - the job would fail
```

* **Static pass** (fast, `--static`, meant for live feedback while editing):
  paths (input readable, output folder exists, not the input, overwrite),
  container ↔ codec for copies and encoders, encoder/filter/muxer names with
  "did you mean" suggestions, option names (plus hints for ffmpeg CLI syntax
  such as `q` or `b:v`), encoder and muxer option values (set on a scratch
  context, on the object FFmpeg gives them to), subtitle kinds, filter
  classes and media types, and filter option values (the chain is parsed in a
  scratch graph).
* **Dry run** (when the static pass found no error): each stream is set up
  alone with `engine_dry_run()` (decoder, filters, encoder with the real
  option values, muxer header written to a null sink), then the whole job with
  the muxer options. "maybe" warnings become *accepted* (info) or errors.
* FFmpeg's own explanation is captured from its log (`logcap`) and appended
  to error messages, in validation and in `run` alike.

## App icon

Two arrows turning around a play triangle, on an indigo-to-teal tile.

* Sources: [assets/icons/ffconv.svg](assets/icons/ffconv.svg), and
  [ffconv-small.svg](assets/icons/ffconv-small.svg) (thicker strokes, larger
  triangle) for the 16–24 px sizes.
* Generated, and committed: `assets/icons/png/ffconv-{16…256}.png` and
  `assets/icons/ffconv.ico`. After editing an SVG, regenerate them from the
  MSYS2 MINGW64 shell (needs `rsvg-convert`, from librsvg):
  `python3 tools/make_icons.py --preview build/icon_preview.png`
* Windows: the `.ico` is compiled into `ffconv.exe` with version info
  (`src/ui/ffconv.rc.in`), so Explorer and shortcuts show it.
* GTK: the SVG and PNGs are compiled into the program as a GResource
  (`src/ui/ffconv.gresource.xml`); every window uses the icon named after the
  application id, `io.github.ffconv.FFConv`.

## User interface (M5)

* **Input**: *Open…* or drop a file on the window. It is probed on a worker
  thread; the summary shows format, duration, size and stream count.
* **Streams**: one row per input stream with its description, an action
  (Copy, Convert, Drop) and, for *Convert*, an encoder list (possible ones
  first, type to search); hardware encoders and unconfirmed ("?") choices are
  marked. Defaults for a new file: copy when the container is known to accept
  the stream, else the preferred software encoder, else drop.
* **Output**: every container, those that can hold this file's streams first
  (common first). The input's own container is preselected when it keeps
  everything, else Matroska. The file name follows the container's extension
  until you choose another one.
* **Incompatible choices are greyed out, not removed**: an action or encoder
  the container cannot store, and a container that cannot store the streams
  as currently chosen, are drawn greyed with a ⚠ and the reason as tooltip
  ("avi cannot store opus"). They stay selectable, and changing one choice
  never changes another: switching container keeps every stream's action and
  encoder as they are. Validation lists what does not fit, and *Convert*
  stays disabled until the job is valid.
* **Live validation**: every change re-runs the static checks (250 ms after
  the last change); errors disable *Convert*.
* **Convert** opens the progress window, which runs the dry run and then the
  conversion on a worker thread: progress, fps, speed, size, time left,
  *Cancel*, the FFmpeg log, and at the end *Show in folder* or the problems
  found.

The UI can be driven without a user through `FFCONV_TEST_*` environment
variables (open a file, pick a container and actions, convert, cancel, save
window snapshots as PNG); see the top of [src/ui/main.c](src/ui/main.c).

## Option editor (M6)

*Options* next to a converted stream (or next to the container) opens an
editor generated from FFmpeg's own option descriptions, so it works for every
encoder and muxer without hard-coding any:

* Sections: **Common** (bitrate, rate control, GOP, B-frames, sample rate,
  channel layout, threads… and **Quality**), the encoder's or muxer's **own
  options**, then the **other general options**. A search box filters them.
* Widgets follow the option type: spin buttons for bounded numbers, text
  fields for unbounded ones (so `2M`, `96k` work), dropdowns with a
  "Default (…)" entry for booleans and named values, checkboxes for flags,
  text fields for the rest. A reset button appears on every changed option.
* Defaults are the encoder's own (read from a context created for it:
  libx264 shows no fixed bitrate, not FFmpeg's generic 200 kb/s).
* When an encoder option and a generic option share a name (x264's `profile`),
  only the one FFmpeg applies is shown (private for encoders, generic for
  muxers).
* Values are checked as you type, on a scratch context: a value FFmpeg
  refuses turns red, with FFmpeg's reason as a tooltip.
* **Quality** is the ffmpeg CLI's `-q`: it sets `global_quality` (q × 118) and
  the `qscale` flag.
* Only changed options are stored, per encoder (switching encoder and back
  keeps them) and per container; the buttons show how many are set.

Checked end to end through the dialogs' widgets: libx264 (`crf`, `preset`,
`tune`, `bf`, `profile`: all visible in x264's settings in the output), aac
(`b=96k` → 97 kb/s, `aac_coder`), libopus (`b=64k` → 59 kb/s in constrained
VBR, `frame_duration=40` → 40 ms packets), libvpx-vp9 (`crf=45`/`b=0` →
1561 kb/s against 6317 kb/s at crf 20, `deadline`, `cpu-used`, `row-mt`),
MP4 `movflags=faststart` (moov before mdat), libvorbis Quality (q 0 / 2 / 10
→ 56 / 71 / 364 kb/s, the same as `ffmpeg -q:a`).

## Equivalent ffmpeg command

The main window shows the `ffmpeg` command that runs the same conversion,
updated with every change, with a *Copy* button and a choice of shell for the
quoting (PowerShell, cmd, Bash). `convcli command job.json [--shell …]`
prints it too; `job_to_ffmpeg_command()` produces it.

```text
ffmpeg -y -i in.mkv -map 0:0 -c:v:0 libx265 -crf:v:0 28 -filter:v:0 scale=w=1280:h=-2,fps=fps=25
       -map 0:1 -c:a:0 aac -b:a:0 192k -map 0:2 -c:s:0 mov_text -movflags +faststart out.mp4
```

* Each stream is `-map 0:<input index>` followed by its options, with output
  stream specifiers counted per type in the job's order (`v:0`, `a:1`…).
* Quality (`global_quality` + the `qscale` flag) is written as `-q`.
* Metadata overrides become `-metadata` / `-metadata:s:<spec>`; turning off
  metadata or chapter copying becomes `-map_metadata -1` / `-map_chapters -1`.
* Quoting: Bash uses `'…'`; PowerShell `'…'` (it also quotes commas, which
  would otherwise split an argument into an array); cmd `"…"` with the
  Windows argument rules. The cmd form is for the interactive prompt: in a
  `.bat` file, `%` must be doubled.

Checked: the commands for `m3_acceptance` and `drawtext_escaping` (filter
text with `: , [ ] ; ' %`), run by the real `ffmpeg` from Bash, PowerShell and
cmd, give frames identical to the engine's (framemd5 of video and audio, and
the same subtitles); the `command_*` tests repeat this for Bash.

Not reproduced by the command: the engine's handling of variable-frame-rate
input (it keeps the timestamps; the ffmpeg CLI's default may duplicate or
drop frames for some containers — add `-fps_mode passthrough` to match) and
its removal of stale Matroska statistics tags on re-encoded streams.

## How "what is possible" is decided

* **Container ↔ codec**: `avformat_query_codec()`. A result of *unknown* is
  reported as `maybe` when the muxer has a default codec of that media type,
  and `no` otherwise (some muxers, e.g. image2, accept anything under relaxed
  compliance, which would otherwise make every pair look possible). `maybe`
  pairs will be settled by the dry-run validation in M4.
* **Stream copy**: the same check on the input codec. Attachments only go to Matroska.
* **Transcode**: needs a decoder in the build. Subtitles only convert within
  the same kind (text→text, bitmap→bitmap).
* **Default encoder** (`job-template`, later the UI): the first software
  encoder in a preference list (x264, x265, SVT-AV1, VP9, aom…; aac, opus…)
  that the container accepts.
* **Muxer keys**: FFmpeg muxer names are not unique (`matroska` is both .mkv
  and .mka). The muxer returned by `av_guess_format(name)` keeps the name; the
  others get `name:ext` (e.g. `matroska:mka`).
* **Filters with dynamic pads** (select, split, decimate, amix...) are created
  once with default options to count their real default pads. Hardware filters
  are not created (libplacebo would start Vulkan, ~1 s) and are classified
  from their static pads.
* **Hardware filters** are recognised by `AVFILTER_FLAG_HWDEVICE` and naming
  convention (`_cuda`, `_vaapi`, `_qsv`, `_vulkan`...), since FFmpeg has no
  public API exposing a filter's accepted pixel formats.

## Verified against the `ffmpeg` CLI (same FFmpeg 8.1 build)

* muxers 183 = 183, encoders 232 = 232, filters 531 = 531 (identical names)
* option names identical to `ffmpeg -h` for libx264, libsvtav1, aac, mp4,
  matroska, hls, scale (incl. swscale/framesync child options), overlay,
  loudnorm, drawtext, amix
* **M3 acceptance**: MKV (H.264 1080p / AC3 5.1 / SRT) → MP4 (HEVC 720p 25 fps
  via `scale` + `fps` / AAC / mov_text). Decoded video and audio are
  bit-identical to `ffmpeg -vf scale=1280:-2,fps=25 -c:v libx265 -c:a aac
  -c:s mov_text` (framemd5 of every frame, including timestamps); subtitle
  text and timing identical.
* Also tested: 5.1 → MP3 (automatic downmix), 44.1 kHz → Opus (resample),
  FLAC, PCM/WAV, Vorbis; WebM (SVT-AV1 / Opus / WebVTT); mid-stream
  resolution change; VFR input (gaps preserved); MPEG-4/AVI; copy-only
  remux; `drawtext` text containing `: , [ ] ; ' %`; Unicode output path;
  cancellation with and without `keep_partial`; 18 error cases.
* **M4**: the 10 jobs in `tests/jobs` pass validation (dry run included); the
  19 jobs in `tests/jobs/invalid` fail with the expected message (Opus in
  AVI, text→bitmap subtitles, bad option name/value, libopus + 5.1(side),
  `q` shorthand, Ogg + H.264 "maybe" rejected by the dry run...). MPEG-TS +
  H.264 "maybe" is confirmed by the dry run. Dry runs create no files.

## Known limitations

* UI: filters cannot be edited yet (M7). Trim, metadata editing and presets
  are M8.
* UI: `ffconv.exe` only runs where the MSYS2 `mingw64/bin` DLLs are found (no
  installer or bundled DLLs yet).
* UI: the file dialogs, drag and drop and *Show in folder* were not exercised
  by the automated tests (they need a person or OS-level automation).
* Constraints that FFmpeg only checks when an encoder opens (libopus rejects
  5.1(side): add `aformat=channel_layouts=5.1`) are found by the dry run, not
  by the static pass, so live feedback in the UI will not show them.
* Filter option values evaluated at configuration time (e.g. `fps=fast`) are
  likewise only caught by the dry run.
* Muxers that write their own files (image2, hls, segment...) are not dry-run.
* Log messages from codecs' internal threads are not captured; libraries that
  print directly to the console (x264/x265/SVT-AV1) are neither captured nor
  silenced by `--quiet`.
* Software only: hardware decoding/filtering/encoding is M9 (hardware
  *encoders* fed with software frames may already work).
* Audio muxers with cover-art support (mp3, flac) report that they can "keep"
  a video stream; in practice only a single picture is stored.
* Any file FFmpeg can open is accepted; a text file probes as ANSI art
  (`tty` demuxer), like `ffprobe` does.
* No memory-leak check has been run yet (Dr. Memory does not start on this
  Windows build; use Valgrind/ASan on Linux).

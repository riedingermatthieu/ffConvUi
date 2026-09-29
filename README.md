# ffconv: FFmpeg converter (M1–M5)

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
| `src/cli/convcli.c` | M1–M4 | Test front end. |
| `src/ui/*` | M5 | GTK 4 application: main window, stream rows, progress window, FFmpeg log view. |

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

`-DCONV_GUI_TESTS=ON` adds three UI tests (`ctest -L gui`); they open windows.

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
  such as `q` or `b:v`), subtitle kinds, filter classes and media types, and
  filter option values (the chain is parsed in a scratch graph).
* **Dry run** (when the static pass found no error): each stream is set up
  alone with `engine_dry_run()` (decoder, filters, encoder with the real
  option values, muxer header written to a null sink), then the whole job with
  the muxer options. "maybe" warnings become *accepted* (info) or errors.
* FFmpeg's own explanation is captured from its log (`logcap`) and appended
  to error messages, in validation and in `run` alike.

## User interface (M5)

* **Input**: *Open…* or drop a file on the window. It is probed on a worker
  thread; the summary shows format, duration, size and stream count.
* **Streams**: one row per input stream with its description and the actions
  the selected container allows (Copy, Convert, Drop). *Convert* shows an
  encoder list (best first, type to search); hardware encoders and
  unconfirmed ("?") choices are marked. Defaults: copy when the container is
  known to accept the stream, else the preferred software encoder, else drop.
* **Output**: containers that can hold at least one stream (common first,
  "keeps N of M streams" when some would be dropped). The input's own
  container is preselected when it keeps everything, else Matroska. The file
  name follows the container's extension until you choose another one.
* **Live validation**: every change re-runs the static checks (250 ms after
  the last change); errors disable *Convert*.
* **Convert** opens the progress window, which runs the dry run and then the
  conversion on a worker thread: progress, fps, speed, size, time left,
  *Cancel*, the FFmpeg log, and at the end *Show in folder* or the problems
  found.

The UI can be driven without a user through `FFCONV_TEST_*` environment
variables (open a file, pick a container and actions, convert, cancel, save
window snapshots as PNG); see the top of [src/ui/main.c](src/ui/main.c).

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

* UI: encoder/muxer options (M6) and filters (M7) cannot be edited yet;
  conversions use each encoder's defaults. Trim, metadata editing and presets
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

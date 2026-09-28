# ffconv: FFmpeg converter core (M1–M3)

A C11 library (`convcore`) that uses the FFmpeg API to describe an input file,
list every conversion the linked FFmpeg build can do with it, and run a
conversion, plus a headless CLI (`convcli`) to exercise it. The GTK UI comes
later and will use the same library.

| Module | Milestone | What it does |
|---|---|---|
| `src/core/probe.*` | M1 | `MediaInfo` snapshot of an input: container, streams, codec/profile, video geometry/color/HDR/rotation, audio layout, subtitle kind, chapters, tags. Owns its memory, holds no FFmpeg contexts. |
| `src/core/caps.*` | M2 | Capability catalog: every muxer, encoder (with supported pixel/sample formats, rates, layouts) and filter (classified simple/hw/convert/multi-in/multi-out/source/sink). Narrowing queries: containers for a file, per-stream copy/transcode choices for a container, filters for a media type, usable hardware devices. |
| `src/core/avopt_schema.*` | M2 | Converts `AVOption` metadata (type, default, range, named constants, flags, aliases, child classes) into a toolkit-neutral schema from which option editors can be generated. |
| `src/core/job.*`, `json.*` | M3 | `ConvJob`: the user's choices as plain data (container, per-stream copy/transcode/drop, encoder options, filter chains, metadata), loaded from / saved to JSON. Small built-in JSON parser, locale-independent. |
| `src/core/engine.*` | M3 | Runs a job: demux → decode → filter → encode → mux, or stream copy. Progress callback, cancellation from another thread. |
| `src/cli/convcli.c` | M1–M3 | Test front end. |

## Build (Windows, MSYS2 MINGW64)

Requirements: CMake ≥ 3.20, a C11 compiler, pkg-config, and **FFmpeg ≥ 7.1**
development files (tested with FFmpeg 8.1).

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
                   mingw-w64-x86_64-pkgconf mingw-w64-x86_64-ffmpeg
export PATH=/c/msys64/mingw64/bin:$PATH
cmake -S . -B build -G Ninja [-DCONV_TEST_MEDIA=/path/to/file.mkv]
cmake --build build
cd build && ctest
```

With `CONV_TEST_MEDIA` set, the tests also probe that file and convert it end
to end to MP4, MKV and WebM. The VS Code workspace has an "MSYS2 MINGW64"
terminal profile (default) with this environment.

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

## Known limitations

* Some encoder constraints are only checked when the encoder opens, e.g.
  libopus rejects 5.1(side) (add `aformat=channel_layouts=5.1`), and the
  `ffmpeg` CLI shorthand `-q:a` is not an option name (it sets
  `global_quality` + the `qscale` flag). M4 validation will catch these before
  running.
* Error messages for some failures ("cannot write the mp4 header: Invalid
  argument") are generic; the specific reason is in FFmpeg's log line
  printed just before. The UI will capture that log (M5).
* Libraries that print directly to the console (SVT-AV1's banner, x265) are
  not silenced by `--quiet`.
* Software only: hardware decoding/filtering/encoding is M9 (hardware
  *encoders* fed with software frames may already work).
* Audio muxers with cover-art support (mp3, flac) report that they can "keep"
  a video stream; in practice only a single picture is stored.
* Any file FFmpeg can open is accepted; a text file probes as ANSI art
  (`tty` demuxer), like `ffprobe` does.
* No memory-leak check has been run yet (Dr. Memory does not start on this
  Windows build; use Valgrind/ASan on Linux).

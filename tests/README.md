# Tests

* `media/`: small synthetic input files (generated with the `ffmpeg` CLI, see below).
* `jobs/`: conversion jobs run by CTest (`job_<name>`); `jobs/invalid/`: jobs
  that must fail validation. Paths are relative to
  the project root: inputs in `tests/media/`, outputs in `build/`.
* `run_job.cmake`: end-to-end test (`e2e_mp4/mkv/webm`): `job-template` → `run` → `probe`.

Run everything from the MSYS2 MINGW64 shell:

```sh
cmake -S . -B build -G Ninja && cmake --build build && (cd build && ctest)
```

| Job | Checks |
|---|---|
| `m3_acceptance` | MKV (H.264 1080p / AC3 5.1 / SRT) → MP4 (HEVC 720p 25 fps via scale+fps / AAC / mov_text) |
| `resolution_change` | 640×360 → 1280×720 mid-stream: filter graph rebuilt, output size kept |
| `vfr` | variable frame rate input: timestamps and the 0.47 s gap preserved |
| `mpeg4_avi` | mpeg4 + MP3 in AVI (encoder that needs a 1/fps time base) |
| `copy_mov` | stream copy only |
| `drawtext_escaping` | filter option value containing `: , [ ] ; ' %` |
| `cancel`, `cancel_keep_partial` | cancellation after 1 s: output removed / kept and valid |
| `ts_copy_maybe` | H.264/AC3 copy into MPEG-TS: static "maybe", confirmed by the dry run |
| `experimental_vorbis` | FFmpeg's experimental native Vorbis encoder: valid, with a warning |

Every job above is also validated (`validate_<name>`, dry run included).

`jobs/invalid/` holds jobs that must fail validation (`invalid_<name>`, run
by `validate_expect.cmake`): the output must match the job's `"_expect"`
regular expression. They cover unknown encoders/filters/muxers/options (with
suggestions), CLI shorthands (`q`, `b:a`), container ↔ codec mismatches,
text→bitmap subtitles, filters of the wrong type or class, bad option values
(found by the dry run: `preset=warp`, `fps=fast`, libopus + 5.1(side)), a
"maybe" rejected by the dry run (H.264 in Ogg), a missing output folder and a
bad stream index.

## Regenerating the media

From `tests/media/`:

```sh
# test.mkv: H.264 720p, AAC 5.1 (eng), Opus stereo (fra), SRT (default), 2 chapters, title
printf ';FFMETADATA1\ntitle=Test clip\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=2500\ntitle=Intro\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=2500\nEND=5000\ntitle=Main\n' > meta.txt
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30000/1001:duration=5 \
       -f lavfi -i sine=f=440:sample_rate=48000:duration=5 \
       -f lavfi -i sine=f=880:sample_rate=44100:duration=5 -i subs.srt -i meta.txt \
       -filter_complex "[1:a]pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0[a51]" \
       -map 0:v -map "[a51]" -map 2:a -map 3 -map_metadata 4 -map_chapters 4 \
       -c:v libx264 -preset veryfast -c:a:0 aac -c:a:1 libopus -ac:a:1 2 -c:s srt \
       -metadata:s:a:0 language=eng -metadata:s:a:1 language=fra \
       -metadata:s:s:0 language=eng -disposition:s:0 default test.mkv

# m3_in.mkv: H.264 1080p 29.97, AC3 5.1(side) 384k, SRT
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30000/1001:duration=10 \
       -f lavfi -i sine=f=440:sample_rate=48000:duration=10 -i m3.srt \
       -filter_complex "[1:a]pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0[a]" \
       -map 0:v -map "[a]" -map 2 -c:v libx264 -preset veryfast -c:a ac3 -b:a 384k -c:s srt \
       -metadata:s:a language=eng -metadata:s:s language=eng m3_in.mkv

# a441.wav: 44.1 kHz stereo PCM
ffmpeg -f lavfi -i sine=f=300:sample_rate=44100:duration=6 -ac 2 -c:a pcm_s16le a441.wav

# reschange.ts: 2 s at 640x360 followed by 2 s at 1280x720
ffmpeg -f lavfi -i testsrc2=size=640x360:rate=25:duration=2 -c:v libx264 -preset ultrafast -f mpegts p1.ts
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=25:duration=2 -c:v libx264 -preset ultrafast \
       -output_ts_offset 2 -f mpegts p2.ts
cat p1.ts p2.ts > reschange.ts && rm p1.ts p2.ts

# vfr.mkv: 30 fps with a 0.5 s jump
ffmpeg -f lavfi -i "testsrc2=size=640x360:rate=30,setpts='N/30+if(gte(N\,20)\,0.5\,0)/TB'" \
       -t 3 -c:v libx264 -preset ultrafast -fps_mode vfr vfr.mkv
```

`subs.srt` and `m3.srt` are hand-written; `meta.txt` is the FFMETADATA file above.

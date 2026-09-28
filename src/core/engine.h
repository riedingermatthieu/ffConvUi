/*
 * engine.h - run a ConvJob: demux -> decode -> filter -> encode -> mux
 * (or stream copy), in software. Based on FFmpeg's doc/examples/transcode.c.
 *
 * engine_run() blocks; call it from a worker thread in a UI. It can be
 * cancelled from another thread through the `cancel` flag: the output is then
 * finalised (trailer written) and deleted unless job->keep_partial is set.
 */
#ifndef CONV_ENGINE_H
#define CONV_ENGINE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "job.h"

typedef struct EngineProgress {
    double  out_time;    /* seconds of output written so far */
    double  duration;    /* input duration in seconds, 0 if unknown */
    double  percent;     /* 0..100, -1 if unknown */
    int64_t frames;      /* video frames encoded (all transcoded video streams) */
    double  fps;         /* encoded video frames per second of wall time */
    double  speed;       /* out_time / elapsed wall time */
    int64_t out_bytes;   /* bytes written to the output file */
    double  elapsed;     /* wall-clock seconds */
    int     finished;    /* set on the last call */
} EngineProgress;

typedef struct EngineCallbacks {
    /* Called from the engine's thread, at most every ~250 ms, plus once at
     * the end with finished = 1. May be NULL. */
    void (*on_progress)(const EngineProgress *p, void *opaque);
    void  *opaque;
} EngineCallbacks;

typedef struct EngineStats {
    int64_t decode_errors;   /* corrupt packets/frames skipped */
    int64_t dropped_frames;  /* video frames dropped for non-increasing timestamps */
    int     graph_reinits;   /* filter graphs rebuilt after a format change */
} EngineStats;

/*
 * Returns 0 on success, AVERROR_EXIT if cancelled, another negative AVERROR on
 * failure with a message in err. `cancel` and `stats` may be NULL.
 */
int engine_run(const ConvJob *job, const EngineCallbacks *cb, atomic_int *cancel,
               EngineStats *stats, char *err, size_t errlen);

/*
 * Set up everything engine_run() would (input, decoders, filter graphs,
 * encoders with their options, muxer with its options and header) without
 * processing any media; the header goes to a null sink, no file is created.
 * *header_checked is set to 0 for muxers without a file (image2, hls,
 * segment...), whose header is not written. FFmpeg's log is not printed:
 * its last error is included in err.
 */
int engine_dry_run(const ConvJob *job, int *header_checked, char *err, size_t errlen);

#endif /* CONV_ENGINE_H */

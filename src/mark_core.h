/*
 * Original work (c) 2026 Tim Cox, MIT (see LICENSE-mark). Modified for Looper 606.
 *
 * Mark — RC-505-style 5-track live looper for the Ableton Move (schwung).
 *
 * Five independent stereo loop tracks, each cycling record -> play ->
 * overdub with its own stop/clear, level, pan, reverse and one-shot.
 * Tracks quantize to a shared measure grid (MIDI clock when running,
 * otherwise the first-recorded track defines the grid), like the RC-505's
 * LOOP SYNC ON + QUANTIZE MEASURE defaults. Single-level undo/redo of the
 * last record/overdub gesture (the RC's UNDO/REDO), implemented as an
 * incremental buffer swap so the render path never blocks.
 *
 * Timing model mirrors smack: 4/4, MIDI clock 24 ppqn (96 ticks/measure)
 * via on_midi, free-run fallback from host get_bpm(). Render path is
 * non-allocating; all buffers are allocated in mark_create() (with a
 * shrinking-capacity fallback if the device can't give us the full size).
 */
#ifndef MARK_CORE_H
#define MARK_CORE_H

#include <stdint.h>
#include "plugin_api_v1.h"

/* All four are overridable at build time so one engine serves several panels.
 * Move is the default and must stay bit-identical: 44100 / 5 tracks / stereo.
 * The Daisy Patch build passes -DMARK_SR=48000 -DMARK_TRACKS=4 -DMARK_CH=1. */
#ifndef MARK_SR
#define MARK_SR      44100
#endif

#ifndef MARK_TRACKS
#define MARK_TRACKS  6
#endif

/*
 * Channels held in TRACK STORAGE (t->buf and undo_buf). NOT the width of the
 * audio path — that is always stereo, so pan, the per-track delay and the FX
 * chain are unaffected by this.
 *
 * It is a separate knob because storage is where all the memory is. The delay
 * lines are 256 KB per track and fx_block is 2 KB total; the track buffers are
 * 11.5 MB each at 60 s / 48 kHz. On a panel whose tracks are fed by one mono
 * jack each, storing two identical channels doubles the only allocation that
 * matters, so MARK_CH=1 halves the module's footprint and buys twice the loop
 * length for the same SDRAM.
 *
 * Every access goes through the tbuf_* accessors below, which at MARK_CH==2
 * compile to exactly the expressions they replaced.
 */
#ifndef MARK_CH
#define MARK_CH      2
#endif

/*
 * Optional subsystems, both on by default.
 *
 * They are separable because each is the only reason the engine needs a
 * hosted OS. Turning both off leaves pure C with libc and no pthread, dlfcn,
 * dirent, sys/stat or file I/O — which is what a bare-metal target needs.
 *
 *   MARK_HOSTED_FX  loading audio_fx v2 plugins from disk (dlopen + dirent,
 *                   and the worker thread that keeps dlopen off the audio
 *                   path). With it off, no external module ever loads and
 *                   every track falls through to the built-in FX — the same
 *                   runtime state as a hosted build with nothing loaded, so
 *                   no other code changes.
 *
 *   MARK_SESSIONS   saving and loading loops as WAVs (stdio + mkdir/unlink,
 *                   and the worker thread that keeps that off the audio
 *                   path). With it off, save/load report failure.
 *
 * The Daisy Patch build sets both to 0. Its SD card could back MARK_SESSIONS
 * later through libDaisy's FatFS, which is why this is a flag and not a
 * deletion.
 */
#ifndef MARK_HOSTED_FX
#define MARK_HOSTED_FX 1
#endif

#ifndef MARK_SESSIONS
#define MARK_SESSIONS  1
#endif

#define MARK_NEEDS_THREADS (MARK_HOSTED_FX || MARK_SESSIONS)

/* Per-track capacity target, seconds. At MARK_CH=2, 60 s stereo int16 is
 * ~10.6 MB per track (44.1k) and 5 tracks + 1 undo buffer is ~63 MB.
 * At MARK_CH=1 and 4 tracks that same 60 s costs ~23 MB at 48 kHz.
 * mark_create() falls back to smaller capacities if allocation fails
 * (see mark_alloc_seconds). */
#ifndef MARK_MAX_SECONDS
#define MARK_MAX_SECONDS 120
#endif

/* Track states, exposed via the `tstates` getter in this order. */
typedef enum {
    MK_EMPTY = 0,
    MK_REC   = 1,
    MK_PLAY  = 2,
    MK_DUB   = 3,
    MK_STOP  = 4
} mk_tstate_t;

typedef struct mark mark_t;

mark_t *mark_create(const host_api_v1_t *host);
/* Production constructor: module_dir lets Mark discover sibling
 * modules/audio_fx entries. mark_create() remains the no-catalog test API. */
mark_t *mark_create_in_dir(const host_api_v1_t *host, const char *module_dir);
void    mark_destroy(mark_t *m);

/* Process one block, stereo interleaved int16. in and out may alias. */
/* Classic form: one stereo input every track records, one stereo mix every
 * track sums into. Buffers are interleaved, `frames` frames each. */
void mark_process(mark_t *m, const int16_t *in, int16_t *out, int frames);

/*
 * Per-track form, for a panel with a jack per track.
 *
 * in_ch[i] and out_ch[i] are MONO buffers of `frames` samples: track i
 * records in_ch[i] and appears alone on out_ch[i]. Both arrays must have
 * MARK_TRACKS entries and no NULL members. `frames` is clamped to
 * MARK_BLOCK_FRAMES, as in mark_process.
 *
 * Two behavioural differences follow from one jack per track, and are
 * deliberate rather than unimplemented:
 *   - PAN is not applied. A mono output has nowhere to pan to. The `pan`
 *     param still round-trips through state; it just does not affect audio.
 *   - MONITOR passes that track's own input, not a shared one.
 * Level, master gain, one-shot, reverse, undo and both FX paths behave
 * exactly as in mark_process.
 */
/* L606 PERFORM: attach the effects engine (perform.h) and pick its target: 0 = the whole
 * mix, 1..MARK_TRACKS = that track's output. */
struct perf;
void mark_set_perf(mark_t *m, struct perf *p);
void mark_set_perf_target(mark_t *m, int target);

void mark_process_multi(mark_t *m, const int16_t *const *in_ch,
                        int16_t *const *out_ch, int frames);

void mark_on_midi(mark_t *m, const uint8_t *msg, int len, int source);
void mark_set_param(mark_t *m, const char *key, const char *val);
int  mark_get_param(mark_t *m, const char *key, char *buf, int buf_len);
/* Peak overview of track ti (0-based) for a screen: see mark_core.c. */
int  mark_track_wave_win(mark_t *m, int ti, int bins, uint8_t *out, uint32_t lo, uint32_t hi, int *pos);
#define MK_SPD_N  7
#define MK_SPD_1X 3
int  mark_track_wave(mark_t *m, int ti, int bins, uint8_t *out, uint32_t *span_frames, int *pos);
/* Clip start of track ti as a bin of the overview (-1 = empty). */
int  mark_track_start_bin(mark_t *m, int ti, int bins);
/* L606 END view: whole = 1 spans the whole recorded buffer (trimmed part included). */
int  mark_track_wave_ex(mark_t *m, int ti, int bins, uint8_t *out, uint32_t *span_frames, int *pos, int whole);
int  mark_track_start_bin_ex(mark_t *m, int ti, int bins, int whole);
int  mark_track_end_bin(mark_t *m, int ti, int bins);

/* Persistence (L606): loop audio as <dir>/tN.wav + <dir>/loops.json, written and
 * read by the CALLER's background thread (never the audio thread). The audio
 * thread is never blocked: saving reads the buffers as they are (a torn write
 * bumps the revision and is simply saved again). */
/* Revision signature of track ti: changes whenever its loop (audio, length,
 * start, or buffer) changes. Cheap, lock-free; safe from any thread. */
uint64_t mark_persist_sig(mark_t *m, int ti);
/* Write the tracks in `mask` (bit i = track i+1) and then loops.json. 0 = ok. */
int  mark_persist_save(mark_t *m, const char *dir, unsigned mask);
/* Restore every track from <dir>. Returns the number of tracks restored, or -1
 * when <dir> holds no valid loops.json (nothing is touched then). */
int  mark_persist_load(mark_t *m, const char *dir);

#endif /* MARK_CORE_H */

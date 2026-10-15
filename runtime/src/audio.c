// SPDX-License-Identifier: AGPL-3.0-only
// Native audio for the Web Audio stand-in in js/audio.js: decoding and mixing.
//
// - Decoding runs on its own thread (decodeAudioData is asynchronous in browsers too): WAV is
//   converted to 16-bit PCM, M4A/AAC (the game's cries) is demuxed with minimp4 and decoded with
//   faad2, short MP3s are decoded whole. Long MP3s (music, 1-3 MB each) stay compressed and are
//   decoded while they play: a whole track as PCM would take 30-60 MB.
// - Mixing runs on the platform's audio thread (platform_*.c), 48 kHz stereo: up to MAX_VOICES
//   playing buffers, each with a start time on the output sample clock (AudioBufferSourceNode.start
//   (when, offset, duration)), looping, playback rate (linear interpolation) and left/right gain
//   (js/audio.js folds the gain/panner node chain into those).
// One mutex protects voices and buffer reference counts; the mixer holds it while it mixes one block.

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3_ex.h"

#include "host.h"

#define OUT_RATE 48000
#define MAX_VOICES 32
#define STREAM_MIN_BYTES (384 * 1024) // MP3s bigger than this play from the compressed data
#define WINDOW_FRAMES 2304            // decoded frames held per streaming voice

typedef struct abuf {
    int refs;
    int channels, rate;
    int64_t frames;
    int16_t *pcm;          // interleaved PCM, or NULL for a streamed MP3
    uint8_t *mp3;          // compressed MP3 (streamed)
    size_t mp3_size;
} abuf;

typedef struct {
    bool used;
    uint32_t id;
    abuf *b;
    int64_t start_at, stop_at; // output sample clock
    double pos, end;           // source frames
    bool loop;
    double loop_start, loop_end;
    double rate;               // playbackRate (and detune) times source/output rate
    float gl, gr;
    // streamed MP3: a window of decoded frames [win_base, win_base + win_frames)
    mp3dec_ex_t *mp3;
    int16_t *win;
    int64_t win_base;
    int win_frames;
} voice;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static voice voices[MAX_VOICES];
static uint32_t next_voice_id = 1;
static int64_t out_clock; // frames mixed so far
static uint32_t ended_ids[256];
static int ended_n;
static size_t pcm_bytes, mp3_bytes; // memory held by buffers (for the log)
static bool output_ok;

// ---------------------------------------------------------------- buffers

static void abuf_unref_locked(abuf *b) {
    if (!b || --b->refs > 0) return;
    if (b->pcm) pcm_bytes -= (size_t)b->frames * b->channels * 2;
    if (b->mp3) mp3_bytes -= b->mp3_size;
    free(b->pcm);
    free(b->mp3);
    free(b);
}

void audio_buffer_release(void *p) {
    pthread_mutex_lock(&lock);
    abuf_unref_locked(p);
    pthread_mutex_unlock(&lock);
}

void audio_buffer_info(void *p, int *channels, int *rate, double *frames) {
    abuf *b = p;
    *channels = b->channels, *rate = b->rate, *frames = (double)b->frames;
}

// ---------------------------------------------------------------- decoding

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16le(const uint8_t *p) { return p[0] | p[1] << 8; }

static abuf *decode_wav(const uint8_t *d, size_t n) {
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return NULL;
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    const uint8_t *data = NULL;
    size_t data_n = 0;
    for (size_t o = 12; o + 8 <= n;) {
        uint32_t sz = rd32le(d + o + 4);
        const uint8_t *c = d + o + 8;
        if (!memcmp(d + o, "fmt ", 4) && sz >= 16 && o + 8 + 16 <= n) {
            fmt = rd16le(c), ch = rd16le(c + 2), rate = (int)rd32le(c + 4), bits = rd16le(c + 14);
            if (fmt == 0xFFFE && sz >= 26) fmt = rd16le(c + 24); // WAVE_FORMAT_EXTENSIBLE: subformat
        } else if (!memcmp(d + o, "data", 4)) {
            data = c;
            data_n = sz <= n - (o + 8) ? sz : n - (o + 8);
        }
        o += 8 + sz + (sz & 1);
    }
    if (!data || ch < 1 || ch > 2 || rate <= 0 || !(fmt == 1 || fmt == 3)) return NULL;
    int bps = bits / 8;
    if (bps < 1 || bps > 4 || (fmt == 3 && bps != 4)) return NULL;
    int64_t frames = (int64_t)(data_n / (size_t)(bps * ch));
    abuf *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->pcm = malloc((size_t)(frames > 0 ? frames : 1) * ch * 2);
    if (!b->pcm) {
        free(b);
        return NULL;
    }
    for (int64_t i = 0; i < frames * ch; i++) {
        const uint8_t *s = data + i * bps;
        int v;
        if (fmt == 3) {
            union { uint32_t u; float f; } x = {rd32le(s)};
            float f = x.f < -1 ? -1 : x.f > 1 ? 1 : x.f;
            v = (int)(f * 32767);
        } else if (bps == 1) v = (s[0] - 128) << 8;
        else if (bps == 2) v = (int16_t)rd16le(s);
        else if (bps == 3) v = (int16_t)(s[1] | s[2] << 8);
        else v = (int16_t)rd16le(s + 2);
        b->pcm[i] = (int16_t)v;
    }
    b->channels = ch, b->rate = rate, b->frames = frames;
    return b;
}

// M4A/AAC: src/audio_m4a.c (minimp4 and minimp3 can't share a file).
void *audio_decode_m4a(const uint8_t *d, size_t n, int16_t **pcm, int *channels, int *rate, int64_t *frames);
static abuf *decode_m4a(const uint8_t *d, size_t n) {
    int16_t *pcm = NULL;
    int ch = 0, rate = 0;
    int64_t frames = 0;
    if (!audio_decode_m4a(d, n, &pcm, &ch, &rate, &frames)) return NULL;
    abuf *b = calloc(1, sizeof *b);
    if (!b) {
        free(pcm);
        return NULL;
    }
    b->pcm = pcm, b->channels = ch, b->rate = rate, b->frames = frames;
    return b;
}

// MP3: short ones decoded whole; long ones keep the (copied) compressed data for streaming.
static abuf *decode_mp3(uint8_t *d, size_t n, bool *took_data) {
    mp3dec_ex_t *dec = malloc(sizeof *dec);
    if (!dec) return NULL;
    abuf *b = NULL;
    if (mp3dec_ex_open_buf(dec, d, n, MP3D_SEEK_TO_SAMPLE) || !dec->samples || dec->info.channels < 1) {
        free(dec);
        return NULL;
    }
    b = calloc(1, sizeof *b);
    if (!b) goto out;
    b->channels = dec->info.channels > 2 ? 2 : dec->info.channels;
    b->rate = dec->info.hz;
    b->frames = (int64_t)(dec->samples / dec->info.channels);
    if (n >= STREAM_MIN_BYTES) {
        b->mp3 = d, b->mp3_size = n;
        *took_data = true;
    } else {
        b->pcm = malloc((size_t)dec->samples * sizeof(int16_t));
        if (!b->pcm) {
            free(b);
            b = NULL;
            goto out;
        }
        size_t got = mp3dec_ex_read(dec, b->pcm, (size_t)dec->samples);
        b->frames = (int64_t)(got / dec->info.channels);
    }
out:
    mp3dec_ex_close(dec);
    free(dec);
    return b;
}

typedef struct job {
    uint32_t id;
    uint8_t *data;
    size_t size;
    abuf *result;
    struct job *next;
} job;
static pthread_mutex_t jlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jcond = PTHREAD_COND_INITIALIZER;
static job *jin, *jin_tail, *jout;
static uint32_t next_job_id = 1;

static void *decode_thread(void *arg) {
    (void)arg;
    plat_thread_setup("pkr_audio_decode", 2);
    for (;;) {
        pthread_mutex_lock(&jlock);
        while (!jin) pthread_cond_wait(&jcond, &jlock);
        job *j = jin;
        jin = j->next;
        if (!jin) jin_tail = NULL;
        pthread_mutex_unlock(&jlock);

        bool took = false;
        abuf *b = NULL;
        if (j->size >= 12 && !memcmp(j->data, "RIFF", 4)) b = decode_wav(j->data, j->size);
        else if (j->size >= 12 && !memcmp(j->data + 4, "ftyp", 4)) b = decode_m4a(j->data, j->size);
        else b = decode_mp3(j->data, j->size, &took);
        if (!took) free(j->data);
        j->data = NULL;
        if (b) {
            b->refs = 1;
            pthread_mutex_lock(&lock);
            if (b->pcm) pcm_bytes += (size_t)b->frames * b->channels * 2;
            if (b->mp3) mp3_bytes += b->mp3_size;
            pthread_mutex_unlock(&lock);
        }
        j->result = b;
        pthread_mutex_lock(&jlock);
        j->next = jout;
        jout = j;
        pthread_mutex_unlock(&jlock);
    }
    return NULL;
}

// Queues `data` (copied) for decoding; returns the job id.
uint32_t audio_decode_async(const uint8_t *data, size_t size) {
    job *j = calloc(1, sizeof *j);
    if (!j) return 0;
    j->data = malloc(size ? size : 1);
    if (!j->data) {
        free(j);
        return 0;
    }
    memcpy(j->data, data, size);
    j->size = size;
    pthread_mutex_lock(&jlock);
    j->id = next_job_id++;
    if (jin_tail) jin_tail->next = j;
    else jin = j;
    jin_tail = j;
    pthread_cond_signal(&jcond);
    uint32_t id = j->id;
    pthread_mutex_unlock(&jlock);
    return id;
}

// Finished decodes: calls cb(id, buffer or NULL) for each (the callback owns the reference).
void audio_decode_poll(void (*cb)(void *ctx, uint32_t id, void *buffer), void *ctx) {
    pthread_mutex_lock(&jlock);
    job *done = jout;
    jout = NULL;
    pthread_mutex_unlock(&jlock);
    while (done) {
        job *j = done;
        done = j->next;
        cb(ctx, j->id, j->result);
        free(j);
    }
}

// ---------------------------------------------------------------- voices

static voice *find_voice(uint32_t id) {
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].used && voices[i].id == id) return &voices[i];
    return NULL;
}

static void voice_free_locked(voice *v, bool report) {
    if (report && ended_n < (int)(sizeof ended_ids / sizeof *ended_ids)) ended_ids[ended_n++] = v->id;
    if (v->mp3) {
        mp3dec_ex_close(v->mp3);
        free(v->mp3);
    }
    free(v->win);
    abuf_unref_locked(v->b);
    memset(v, 0, sizeof *v);
}

uint32_t audio_start(void *buffer, double when, double offset, double duration, bool loop, double loop_start,
                     double loop_end, double rate, float gl, float gr) {
    abuf *b = buffer;
    if (!b || b->frames <= 0) return 0;
    mp3dec_ex_t *mp3 = NULL;
    int16_t *win = NULL;
    if (!b->pcm) { // streamed: its own decoder (the scan for the seek index happens here, not in the mixer)
        mp3 = malloc(sizeof *mp3);
        win = malloc(WINDOW_FRAMES * b->channels * sizeof(int16_t));
        if (!mp3 || !win || mp3dec_ex_open_buf(mp3, b->mp3, b->mp3_size, MP3D_SEEK_TO_SAMPLE)) {
            free(mp3);
            free(win);
            return 0;
        }
    }
    pthread_mutex_lock(&lock);
    voice *v = NULL;
    for (int i = 0; i < MAX_VOICES && !v; i++)
        if (!voices[i].used) v = &voices[i];
    if (!v) { // all busy: replace the one that started first
        v = &voices[0];
        for (int i = 1; i < MAX_VOICES; i++)
            if (voices[i].start_at < v->start_at) v = &voices[i];
        voice_free_locked(v, true);
    }
    v->used = true;
    v->id = next_voice_id++;
    if (!v->id) v->id = next_voice_id++;
    v->b = b;
    b->refs++;
    int64_t at = (int64_t)llround(when * OUT_RATE);
    v->start_at = at > out_clock ? at : out_clock;
    v->stop_at = INT64_MAX;
    double total = (double)b->frames;
    v->pos = offset > 0 ? offset * b->rate : 0;
    if (v->pos > total) v->pos = total;
    v->end = duration >= 0 ? v->pos + duration * b->rate : total;
    if (v->end > total) v->end = total;
    v->loop = loop;
    v->loop_start = loop_start > 0 ? loop_start * b->rate : 0;
    v->loop_end = loop_end > 0 ? loop_end * b->rate : total;
    if (v->loop_end > total || v->loop_end <= v->loop_start) v->loop_end = total, v->loop_start = 0;
    v->rate = rate * b->rate / OUT_RATE;
    v->gl = gl, v->gr = gr;
    v->mp3 = mp3, v->win = win, v->win_base = 0, v->win_frames = 0;
    uint32_t id = v->id;
    pthread_mutex_unlock(&lock);
    return id;
}

void audio_update(uint32_t id, double rate, float gl, float gr, bool loop, double loop_start, double loop_end) {
    pthread_mutex_lock(&lock);
    voice *v = find_voice(id);
    if (v) {
        v->rate = rate * v->b->rate / OUT_RATE;
        v->gl = gl, v->gr = gr;
        v->loop = loop;
        double total = (double)v->b->frames;
        v->loop_start = loop_start > 0 ? loop_start * v->b->rate : 0;
        v->loop_end = loop_end > 0 ? loop_end * v->b->rate : total;
        if (v->loop_end > total || v->loop_end <= v->loop_start) v->loop_end = total, v->loop_start = 0;
    }
    pthread_mutex_unlock(&lock);
}

void audio_stop(uint32_t id, double when) {
    pthread_mutex_lock(&lock);
    voice *v = find_voice(id);
    if (v) {
        int64_t at = (int64_t)llround(when * OUT_RATE);
        if (at <= out_clock) voice_free_locked(v, true);
        else v->stop_at = at;
    }
    pthread_mutex_unlock(&lock);
}

double audio_time(void) {
    pthread_mutex_lock(&lock);
    int64_t c = out_clock;
    pthread_mutex_unlock(&lock);
    return (double)c / OUT_RATE;
}

// Voice ids that finished (played to the end, or stopped) since the last call.
int audio_ended(uint32_t *ids, int max) {
    pthread_mutex_lock(&lock);
    int n = ended_n < max ? ended_n : max;
    memcpy(ids, ended_ids, n * sizeof *ids);
    memmove(ended_ids, ended_ids + n, (ended_n - n) * sizeof *ids);
    ended_n -= n;
    pthread_mutex_unlock(&lock);
    return n;
}

void audio_memory(size_t *pcm, size_t *mp3, int *active) {
    pthread_mutex_lock(&lock);
    *pcm = pcm_bytes, *mp3 = mp3_bytes;
    int a = 0;
    for (int i = 0; i < MAX_VOICES; i++) a += voices[i].used;
    *active = a;
    pthread_mutex_unlock(&lock);
}

bool audio_output_ok(void) { return output_ok; }

// ---------------------------------------------------------------- mixing

// Makes frames idx and idx + 1 available in a streamed voice's window. False at the end.
static bool stream_fill(voice *v, int64_t idx) {
    int ch = v->b->channels;
    if (idx >= v->win_base && idx + 1 < v->win_base + v->win_frames) return true;
    int keep = 0;
    int64_t last = v->win_base + v->win_frames - 1;
    if (v->win_frames > 0 && idx == last) { // sequential: keep the last frame
        memcpy(v->win, v->win + (size_t)(v->win_frames - 1) * ch, ch * sizeof(int16_t));
        v->win_base = idx;
        keep = 1;
    } else if (v->win_frames > 0 && idx > last && idx - last < WINDOW_FRAMES) { // a little ahead: read on
        int64_t skip = idx - last - 1;
        while (skip > 0) {
            size_t n = (size_t)(skip < WINDOW_FRAMES ? skip : WINDOW_FRAMES);
            size_t got = mp3dec_ex_read(v->mp3, v->win, n * ch) / ch;
            if (got == 0) return false;
            skip -= (int64_t)got;
        }
        v->win_base = idx;
    } else { // jump (start, loop): seek
        if (mp3dec_ex_seek(v->mp3, (uint64_t)idx * v->mp3->info.channels)) return false;
        v->win_base = idx;
    }
    // mp3dec_ex_read returns the file's channel count; mono/stereo match b->channels (>2 never kept)
    size_t want = (size_t)(WINDOW_FRAMES - keep) * ch;
    size_t got = mp3dec_ex_read(v->mp3, v->win + (size_t)keep * ch, want);
    v->win_frames = keep + (int)(got / ch);
    return idx < v->win_base + v->win_frames;
}

static inline void frame_at(voice *v, int64_t idx, int *l, int *r) {
    const int16_t *p;
    int ch = v->b->channels;
    if (v->b->pcm) {
        if (idx >= v->b->frames) idx = v->b->frames - 1;
        p = v->b->pcm + (size_t)idx * ch;
    } else {
        int64_t k = idx - v->win_base;
        if (k >= v->win_frames) k = v->win_frames - 1;
        if (k < 0) k = 0;
        p = v->win + (size_t)k * ch;
    }
    *l = p[0];
    *r = ch > 1 ? p[1] : p[0];
}

// Fills `out` (interleaved stereo) with `frames` mixed frames. Called on the audio thread.
void audio_mix(int16_t *out, int frames) {
    static int32_t acc[4096 * 2];
    if (frames > 4096) frames = 4096;
    memset(acc, 0, (size_t)frames * 2 * sizeof *acc);
    pthread_mutex_lock(&lock);
    for (int vi = 0; vi < MAX_VOICES; vi++) {
        voice *v = &voices[vi];
        if (!v->used) continue;
        int i = 0;
        if (v->start_at > out_clock) {
            int64_t wait = v->start_at - out_clock;
            if (wait >= frames) continue;
            i = (int)wait;
        }
        bool done = false;
        for (; i < frames; i++) {
            if (out_clock + i >= v->stop_at) { done = true; break; }
            if (v->loop && v->pos >= v->loop_end) v->pos = v->loop_start + fmod(v->pos - v->loop_start, v->loop_end - v->loop_start);
            if (!v->loop && v->pos >= v->end) { done = true; break; }
            int64_t idx = (int64_t)v->pos;
            if (!v->b->pcm && !stream_fill(v, idx)) { done = true; break; }
            float frac = (float)(v->pos - (double)idx);
            int l0, r0, l1, r1;
            frame_at(v, idx, &l0, &r0);
            frame_at(v, idx + 1 < v->b->frames ? idx + 1 : idx, &l1, &r1);
            float l = l0 + (l1 - l0) * frac, r = r0 + (r1 - r0) * frac;
            acc[i * 2] += (int32_t)(l * v->gl);
            acc[i * 2 + 1] += (int32_t)(r * v->gr);
            v->pos += v->rate;
        }
        if (done) voice_free_locked(v, true);
    }
    out_clock += frames;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < frames * 2; i++) {
        int32_t s = acc[i];
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
}

// ---------------------------------------------------------------- JS natives
// AudioData: a decoded (or streamable) buffer, owned by an AudioBuffer in js/audio.js.

static JSClassID data_class;
static void data_finalizer(JSRuntime *rt, JSValue v) {
    void *b = JS_GetOpaque(v, data_class);
    if (b) audio_buffer_release(b);
}

static double argnum(JSContext *ctx, int argc, JSValueConst *argv, int i, double def) {
    double d = def;
    if (i < argc && !JS_IsUndefined(argv[i]) && !JS_IsNull(argv[i])) JS_ToFloat64(ctx, &d, argv[i]);
    return d;
}

// audioDecode(ArrayBuffer | TypedArray) -> job id (0 on failure)
static JSValue n_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t len = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t off, tlen, bpe, total;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &tlen, &bpe);
        if (JS_IsException(ab)) return JS_NewInt32(ctx, 0);
        p = JS_GetArrayBuffer(ctx, &total, ab);
        JS_FreeValue(ctx, ab);
        if (!p) return JS_NewInt32(ctx, 0);
        p += off;
        len = tlen;
    }
    return JS_NewUint32(ctx, audio_decode_async(p, len));
}

typedef struct { JSContext *ctx; JSValue arr; uint32_t n; } poll_ctx;
static void poll_cb(void *c, uint32_t id, void *buffer) {
    poll_ctx *pc = c;
    JSContext *ctx = pc->ctx;
    JSValue e = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, e, 0, JS_NewUint32(ctx, id));
    if (buffer) {
        int ch, rate;
        double frames;
        audio_buffer_info(buffer, &ch, &rate, &frames);
        JSValue o = JS_NewObjectClass(ctx, data_class);
        JS_SetOpaque(o, buffer);
        JS_SetPropertyUint32(ctx, e, 1, o);
        JS_SetPropertyUint32(ctx, e, 2, JS_NewInt32(ctx, ch));
        JS_SetPropertyUint32(ctx, e, 3, JS_NewInt32(ctx, rate));
        JS_SetPropertyUint32(ctx, e, 4, JS_NewFloat64(ctx, frames));
    } else {
        JS_SetPropertyUint32(ctx, e, 1, JS_NULL);
    }
    JS_SetPropertyUint32(ctx, pc->arr, pc->n++, e);
}

// audioPoll() -> [[jobId, AudioData|null, channels, rate, frames], ...]
static JSValue n_poll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    poll_ctx pc = {ctx, JS_NewArray(ctx), 0};
    audio_decode_poll(poll_cb, &pc);
    return pc.arr;
}

// audioStart(data, when, offset, duration (-1: to the end), loop, loopStart, loopEnd, rate, gainL, gainR) -> voice id
static JSValue n_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    void *b = argc > 0 ? JS_GetOpaque(argv[0], data_class) : NULL;
    if (!b) return JS_NewInt32(ctx, 0);
    uint32_t id = audio_start(b, argnum(ctx, argc, argv, 1, 0), argnum(ctx, argc, argv, 2, 0), argnum(ctx, argc, argv, 3, -1),
                              argc > 4 && JS_ToBool(ctx, argv[4]) > 0, argnum(ctx, argc, argv, 5, 0), argnum(ctx, argc, argv, 6, 0),
                              argnum(ctx, argc, argv, 7, 1), (float)argnum(ctx, argc, argv, 8, 1), (float)argnum(ctx, argc, argv, 9, 1));
    return JS_NewUint32(ctx, id);
}

// audioUpdate(id, rate, gainL, gainR, loop, loopStart, loopEnd)
static JSValue n_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    audio_update((uint32_t)argnum(ctx, argc, argv, 0, 0), argnum(ctx, argc, argv, 1, 1), (float)argnum(ctx, argc, argv, 2, 1),
                 (float)argnum(ctx, argc, argv, 3, 1), argc > 4 && JS_ToBool(ctx, argv[4]) > 0, argnum(ctx, argc, argv, 5, 0),
                 argnum(ctx, argc, argv, 6, 0));
    return JS_UNDEFINED;
}

// audioStop(id, when)
static JSValue n_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    audio_stop((uint32_t)argnum(ctx, argc, argv, 0, 0), argnum(ctx, argc, argv, 1, 0));
    return JS_UNDEFINED;
}

static JSValue n_time(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, audio_time());
}

// audioEnded() -> [voice ids that finished since the last call]
static JSValue n_ended(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    uint32_t ids[64];
    int n = audio_ended(ids, 64);
    JSValue a = JS_NewArray(ctx);
    for (int i = 0; i < n; i++) JS_SetPropertyUint32(ctx, a, i, JS_NewUint32(ctx, ids[i]));
    return a;
}

void audio_register(JSContext *ctx, JSValue native) {
    if (g_cfg.no_audio) return; // js/audio.js falls back to a silent Web Audio
    JS_NewClassID(JS_GetRuntime(ctx), &data_class);
    JSClassDef def = {.class_name = "AudioData", .finalizer = data_finalizer};
    JS_NewClass(JS_GetRuntime(ctx), data_class, &def);
    static const JSCFunctionListEntry fns[] = {
        JS_CFUNC_DEF("audioDecode", 1, n_decode), JS_CFUNC_DEF("audioPoll", 0, n_poll),
        JS_CFUNC_DEF("audioStart", 10, n_start), JS_CFUNC_DEF("audioUpdate", 7, n_update),
        JS_CFUNC_DEF("audioStop", 2, n_stop), JS_CFUNC_DEF("audioTime", 0, n_time),
        JS_CFUNC_DEF("audioEnded", 0, n_ended),
    };
    JS_SetPropertyFunctionList(ctx, native, fns, sizeof fns / sizeof *fns);
    JS_SetPropertyStr(ctx, native, "audioOutput", JS_NewBool(ctx, output_ok));
}

void audio_init(void) {
    // The decoders keep large structures on the stack (minimp3's scratch, faad's frame state).
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&t, &attr, decode_thread, NULL) == 0) pthread_detach(t);
    pthread_attr_destroy(&attr);
    output_ok = plat_audio_start(OUT_RATE, audio_mix);
    host_log("[audio] %s, %d Hz stereo", output_ok ? "output started" : "no audio output (sounds are mixed silently)", OUT_RATE);
}

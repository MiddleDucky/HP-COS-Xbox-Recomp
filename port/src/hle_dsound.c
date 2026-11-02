/**
 * DirectSound (XDK 5028) on a host software mixer, in place of the emulated
 * MCPX APU.
 *
 * The recompiled XDK DirectSound drives the APU's registers and waits on its
 * interrupts for stream packets to complete. The emulated APU does not keep
 * real time, so XMV playback starved its audio stream and the intro stalled.
 * Here the public DirectSound entry points are implemented on the host:
 * buffers and stream packets are read straight from guest memory, mixed on a
 * mixer thread paced by XAudio2, and stream packets complete as they play.
 *
 * Logic ported from the MIT-licensed renderer's src/xdk/dsound.cpp (MIT, Copyright (c)
 * 2026 TGP482, Joshhhuaaa), which targets XDK 5849. The 5028 entry points and
 * their argument counts were checked against this XBE: addresses from
 * XbSymbolDatabase, argument counts from each function's `ret n`.
 *
 * Every entry point keeps the generated XDK body as sub_X_gen, so
 * RECOMP_HLE_DSOUND=0 runs the original DirectSound on the emulated APU.
 * RECOMP_DSOUND_TRACE=1 logs the calls.
 *
 * Built into the recompile with --exclude-manual pointing at this file.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the register names (eax, esp, ...) that RECOMP_ICALL expands to */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"
void hpcos_music_report(void); /* hle_debug.c: the music manager and its threads, every 10 s */

#ifdef _MSC_VER
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")
#endif

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
void xbox_HeapFree(uint32_t xbox_va);

/* ── Guest addresses (XDK 5028 DSOUND in this XBE) ───────────────────────── */

#define STREAM_VTABLE   0x0018A260u  /* CDirectSoundStream, XMediaObject part */
#define STREAM_VTABLE2  0x0018A254u  /* its second base, at object + 4 */

#define S_OK_           0x00000000u
#define E_FAIL_         0x80004005u
#define DSERR_INVALID   0x88780032u
#define XMEDIAPACKET_STATUS_PENDING  0x8000000Au
#define XMEDIAPACKET_STATUS_FLUSHED  0x80004004u

/* ── Arguments and returns, __stdcall on the guest stack ─────────────────── */

#define ARG(i)  MEM32(g_esp + 4 + 4 * (i))

static void ret_stdcall(uint32_t result, int nargs)
{
    g_eax = result;
    g_esp += 4 + 4 * nargs;
}

static int g_hle = -1, g_trace;

static int hle_on(void)
{
    if (g_hle < 0) {
        const char *e = getenv("RECOMP_HLE_DSOUND");
        const char *t = getenv("RECOMP_DSOUND_TRACE");
        g_hle = !(e && *e == '0');
        g_trace = t && *t && *t != '0';
        fprintf(stderr, "[DSOUND] %s\n", g_hle ? "host mixer (HLE)" : "XDK DirectSound on the emulated APU");
    }
    return g_hle;
}

int app_AudioTrace(void);  /* app.cpp: F10 Debug > Audio trace, switched while running */
static volatile int g_trace_ui;  /* app_AudioTrace(), read by the mixer once a pass */
#define TRACE(...) do { if (g_trace || g_trace_ui) { fprintf(stderr, "[DSOUND] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ── Voices ─────────────────────────────────────────────────────────────── */

#define MIX_RATE        48000
/* Mixed per pass, ~5.3 ms. The music streams through a ~50 ms looping buffer that the title refills by polling
 * GetCurrentPosition; the console reads it sample by sample. 1024 frames (~21 ms) a pass moved the cursor and read
 * ahead in jumps that ate half of the title's refill margin, and a slow game frame then let the mixer lap the
 * writer: stale audio, heard as clicks. */
#define MIX_FRAMES      256
#define MAX_VOICES      256
#define MAX_PACKETS     64
#define MAX_DONE        1024
#define ADPCM_FRAMES    64   /* Xbox ADPCM plays 64 frames per 36-byte block, not IMA's 65 */

typedef struct {
    uint32_t data, size, completed_ptr, status_ptr, context;
} Packet;

typedef struct {
    uint32_t obj;
    int stream, refs;
    /* format */
    uint16_t tag, channels, bits, align;
    uint32_t rate, freq;
    /* buffer */
    uint32_t data, bytes, loop_start, loop_len, owned;
    int playing, looping, paused, active, ending;
    double pos;  /* frames */
    /* stream */
    uint32_t max_packets, callback, context;
    Packet packets[MAX_PACKETS];
    int npackets;
    /* levels */
    float volume, headroom;
    uint8_t nbins, bin[8];
    int32_t bin_mb[32];
    float gain[2][2];
    float gain6[6][6];  /* 5.1 output (render6): source channel -> speaker */
    int has_gain;
    int fading_out;  /* Stop: one more pass ramping to silence, then stopped */
    /* xboxrecomp: audio diagnostics ([AUDIO]): a looping buffer the title polls (GetCurrentPosition) is one it
     * refills as it plays (streamed music, ambience); a lap whose data has not changed since the last one was not
     * refilled in time and plays old audio again */
    DWORD last_poll_ms;
    uint32_t polls, laps, stale_laps, lap_hash, refills;  /* refills: laps whose data differed from the last one */
    uint32_t last_poll_pos;                               /* the cursor the last poll reported, bytes */
    /* ADPCM block cache */
    int64_t block;
    int16_t decoded[65 * 8];
} Voice;

typedef struct {
    uint32_t voice, callback, context, packet_context, status;
} Done;

static CRITICAL_SECTION g_lock;
static Voice *g_voices[MAX_VOICES];
static Done g_done[MAX_DONE];
static int g_ndone;
static int g_started;
static uint32_t g_ds;

static float db(int32_t hundredths)
{
    return hundredths <= -10000 ? 0.0f : powf(10.0f, hundredths / 2000.0f);
}

static uint32_t frames_of(const Voice *v, uint32_t bytes)
{
    if (v->tag == 0x69) return bytes / (36u * v->channels) * ADPCM_FRAMES;
    return bytes / (v->align ? v->align : 1);
}

static uint32_t bytes_of(const Voice *v, uint32_t frames)
{
    if (v->tag == 0x69) return frames / ADPCM_FRAMES * 36 * v->channels;
    return frames * v->align;
}

static const int k_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* One 36-byte-per-channel IMA block: 65 frames decoded, ADPCM_FRAMES played. */
static void decode_block(const uint8_t *b, int ch, int16_t *out)
{
    for (int c = 0; c < ch; c++) {
        int pred = (int16_t)(b[c * 4] | b[c * 4 + 1] << 8), idx = b[c * 4 + 2] > 88 ? 88 : b[c * 4 + 2];
        const uint8_t *d = b + ch * 4;
        out[c] = (int16_t)pred;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = k_step[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = clampi(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = clampi(idx + k_index[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = (int16_t)pred;
            }
    }
}

/* Sample of channel c at frame f of the memory at guest address data. */
static float sample(Voice *v, uint32_t data, uint32_t f, int c)
{
    if (c >= v->channels) c = v->channels - 1;
    if (v->tag == 0x69) {
        int64_t at = (int64_t)data + (int64_t)(f / ADPCM_FRAMES) * 36 * v->channels;
        if (at != v->block) {
            decode_block((const uint8_t *)XBOX_PTR((uint32_t)at), v->channels > 8 ? 8 : v->channels, v->decoded);
            v->block = at;
        }
        return v->decoded[(f % ADPCM_FRAMES) * v->channels + c] / 32768.0f;
    }
    if (v->bits == 8) return ((int)MEM8(data + f * v->align + c) - 128) / 128.0f;
    return (int16_t)MEM16(data + f * v->align + c * 2) / 32768.0f;
}

/* Packet done: the XDK writes its sizes and status, then queues the stream's
 * callback for DirectSoundDoWork to run on the title's thread. */
static void complete(Voice *v, const Packet *pk, uint32_t status, uint32_t used)
{
    if (pk->completed_ptr) MEM32(pk->completed_ptr) = status == 0 ? pk->size : used;
    if (pk->status_ptr) MEM32(pk->status_ptr) = status;
    if (v->callback) {
        if (g_ndone < MAX_DONE) {
            Done d = {v->obj, v->callback, v->context, pk->context, status};
            g_done[g_ndone++] = d;
        } else {
            fprintf(stderr, "[DSOUND] completion queue full, packet callback dropped\n");
        }
    } else if (pk->context) {
        static int warned;
        if (!warned++) fprintf(stderr, "[DSOUND] packet with a completion event: not signalled (unimplemented)\n");
    }
}

static void pop_packet(Voice *v)
{
    memmove(&v->packets[0], &v->packets[1], (size_t)(v->npackets - 1) * sizeof(Packet));
    v->npackets--;
}

/* Gain of voice channel c on host left and right, from its mix bins. */
static void levels(Voice *v, float g[2][2])
{
    static const uint8_t front[2] = {0, 1};
    const uint8_t *bin = v->nbins ? v->bin : front;
    uint32_t n = v->nbins ? v->nbins : 2, voices = (v->channels - 1) / 2 + 1;
    uint32_t per = n / voices < 2 ? 2 : n / voices, step = v->channels == 1 ? 1 : 2;
    int speakers = 0;
    memset(g, 0, 4 * sizeof(float));
    /* muted speaker bins do not count: such a voice is heard through its effect send */
    for (uint32_t s = 0; s < n; s++) speakers |= bin[s] < 10 && bin[s] != 3 && v->bin_mb[bin[s]] > -10000;
    for (uint32_t c = 0; c < 2; c++)
        for (uint32_t s = c; c < v->channels && s < per && s < n; s += step) {
            uint32_t b = bin[s];
            float x;
            if (speakers && (b == 3 || b > 9)) continue;  /* sub and effect sends */
            x = db(v->bin_mb[b]) * v->volume * v->headroom * (b == 2 ? 0.707f : 1.0f);
            /* even bins are left, odd right, 2 is the centre */
            if (!speakers || b == 2 || !(b & 1)) g[c][0] = g[c][0] > x ? g[c][0] : x;
            if (!speakers || b == 2 || (b & 1)) g[c][1] = g[c][1] > x ? g[c][1] : x;
        }
}

/* xboxrecomp: 5.1 output (F10 Speakers): the bins are the console's speakers in the order a 5.1 channel mask has them
 * (front left, front right, centre, LFE, back left, back right), so each goes to its own speaker instead of the
 * stereo fold-down above; a buffer of more than two channels (a title's own 5.1 mix) goes channel by channel */
static int g_oc = 2;  /* output channels, set when the mixer starts */
int app_Speakers(void);  /* app.cpp: 2 or 6 */

static void levels6(Voice *v, float g[6][6])
{
    static const uint8_t front[2] = {0, 1};
    const uint8_t *bin = v->nbins ? v->bin : front;
    uint32_t n = v->nbins ? v->nbins : 2;
    int speakers = 0;
    memset(g, 0, 36 * sizeof(float));
    /* bins 6..9 are the crosstalk speakers (front L R, back L R): those positions */
    static const uint8_t spk[10] = {0, 1, 2, 3, 4, 5, 0, 1, 4, 5};
    for (uint32_t s = 0; s < n; s++) speakers |= bin[s] < 10 && v->bin_mb[bin[s]] > -10000;
    if (v->channels > 2) {
        for (uint32_t c = 0; c < v->channels && c < 6; c++) {
            uint32_t b = v->nbins > c ? bin[c] : c;
            if (b < 10) g[c][spk[b]] = db(v->bin_mb[b]) * v->volume * v->headroom;
        }
        return;
    }
    {
        uint32_t voices = (v->channels - 1) / 2 + 1, per = n / voices < 2 ? 2 : n / voices, step = v->channels == 1 ? 1 : 2;
        for (uint32_t c = 0; c < 2; c++)
            for (uint32_t s = c; c < v->channels && s < per && s < n; s += step) {
                uint32_t b = bin[s];
                float x = db(v->bin_mb[b]) * v->volume * v->headroom;
                if (!speakers) {
                    /* heard only through its effect sends: on the front pair, as the stereo mix has it */
                    g[c][0] = g[c][0] > x ? g[c][0] : x;
                    g[c][1] = g[c][1] > x ? g[c][1] : x;
                } else if (b < 10) {
                    g[c][spk[b]] = g[c][spk[b]] > x ? g[c][spk[b]] : x;
                }
            }
    }
}

static void deactivate(Voice *v)
{
    if (v->active) v->active = v->paused = v->ending = 0;
}

/* ── Audio diagnostics ([AUDIO] in hpcos_run.log) ───────────────────────────
 * Always on; quiet unless something happens. Every 10 s, if any count is non-zero, one summary line:
 *   clicks   - sudden jumps in the finished mix (a spike in its second difference): what a crackle is
 *   stale    - laps of a polled looping buffer (streamed music, ambience) played with the same data as the lap
 *              before: the title did not refill it in time, old audio played again (a buzz if it repeats)
 *   late     - mixer passes that started more than two blocks (~10 ms) after the one before
 *   burst    - the most blocks rendered back to back after a late start (the play cursor jumps by that much)
 *   gaps, clipped - the output played everything queued; samples past full scale
 * plus a line for the first few clicks and stale laps (with the voices playing), and for each looping buffer's
 * play/stop and each pause/resume. */
static unsigned g_clicks, g_stale, g_late, g_burst_max, g_stream_dry;
static double g_late_max_ms;  /* the longest gap between two mixer passes in the report period */
static double g_jump_max;     /* the largest cursor move between two polls of a looping buffer, share of its loop */
static int g_click_lines, g_stale_lines;

static void audio_voices(char *buf, size_t n)  /* the looping / streamed voices playing now, for an event line */
{
    size_t k = 0;
    buf[0] = 0;
    for (int i = 0; i < MAX_VOICES && k + 48 < n; i++) {
        const Voice *v = g_voices[i];
        if (!v || v->paused || (!v->stream && !v->playing)) continue;
        if (!v->stream && !v->looping) continue;
        k += (size_t)snprintf(buf + k, n - k, " %08X(%s%u b, pos %.0f%s)", v->obj, v->stream ? "stream " : "loop ",
                              v->stream ? 0u : v->bytes, v->pos, v->polls ? ", polled" : "");
    }
}

static uint32_t loop_hash(const Voice *v)
{
    uint32_t start = v->loop_len ? v->loop_start : 0, len = v->loop_len ? v->loop_len : v->bytes, h = 2166136261u;
    const uint8_t *p = (const uint8_t *)XBOX_PTR(v->data + start);
    for (uint32_t i = 0; i < len; i += 4) h = (h ^ p[i]) * 16777619u;  /* every 4th byte: enough to see a refill */
    return h;
}

/* a polled looping buffer wrapped: compare this lap's data with the last one's */
static void audio_lap(Voice *v)
{
    uint32_t h;
    if (!v->data || !v->polls || GetTickCount() - v->last_poll_ms > 1000) return;  /* not one the title refills */
    h = loop_hash(v);
    if (v->laps && h != v->lap_hash) v->refills++;
    /* stale only for a buffer the title has been seen refilling: a fixed loop it merely polls (ambience) repeats by
     * design */
    if (v->laps++ && h == v->lap_hash && v->refills) {
        v->stale_laps++;
        g_stale++;
        if (g_stale_lines++ < 12) {
            char vs[512];
            audio_voices(vs, sizeof vs);
            fprintf(stderr, "[AUDIO] %lu ms stale lap: buffer %08X (%u bytes) looped with no new data since the "
                    "last lap (title last polled it %lu ms ago) | playing:%s\n", (unsigned long)GetTickCount(),
                    v->obj, v->bytes, (unsigned long)(GetTickCount() - v->last_poll_ms), vs);
        }
    }
    v->lap_hash = h;
}

/* xboxrecomp: 5.1 output (F10 Speakers = 5.1): render's mix with six outputs, apart from it so the stereo mix stays
 * the code it was (2026-10-24: folded into one, stereo crackled though no sum in it had changed) */
static void render6(float *out, uint32_t frames)
{
    memset(out, 0, frames * 6 * sizeof(float));
    EnterCriticalSection(&g_lock);
    for (int vi = 0; vi < MAX_VOICES; vi++) {
        Voice *v = g_voices[vi];
        float g[6][6], dg[6][6];
        float e0[6] = {0}, e1[6] = {0};  /* 5.1: a multichannel buffer's channels past the first two */
        int ne = 0;
        double step;
        if (!v || v->paused || (!v->stream && !v->playing)) continue;
        memset(g, 0, sizeof(g));
        levels6(v, g);
        if (v->fading_out) memset(g, 0, sizeof(g));
        /* xboxrecomp: a voice that starts ramps up over this pass (~5 ms) instead of starting at full level: the
         * console's voice processor smooths level steps, and a sound starting or stopping mid waveform at once
         * was heard as a click every time */
        if (!v->has_gain) memset(v->gain6, 0, sizeof(v->gain6));
        v->has_gain = 1;
        for (int c = 0; c < 6; c++)
            for (int side = 0; side < 6; side++) dg[c][side] = (g[c][side] - v->gain6[c][side]) / (float)frames;
        step = (double)(v->freq ? v->freq : v->rate) / MIX_RATE;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)v->pos;
            float a, b, a1, b1, t, x0, x1;
            if (v->stream) {
                while (v->npackets && f >= frames_of(v, v->packets[0].size)) {
                    Packet pk = v->packets[0];
                    pop_packet(v);
                    complete(v, &pk, 0, pk.size);
                    v->pos -= frames_of(v, pk.size);
                    f = (uint32_t)v->pos;
                }
                if (!v->npackets) {
                    if (v->active && !v->ending) {  /* [AUDIO]: a playing stream with no packet left: a gap */
                        static int underruns;
                        g_stream_dry++;
                        if (underruns++ < 12)
                            fprintf(stderr, "[AUDIO] %lu ms stream %08X ran dry (the title fed it too late)\n",
                                    (unsigned long)GetTickCount(), v->obj);
                    }
                    v->pos = 0;
                    if (v->ending) deactivate(v);
                    break;
                }
                a = sample(v, v->packets[0].data, f, 0);
                b = sample(v, v->packets[0].data, f, 1);
                if (f + 1 < frames_of(v, v->packets[0].size)) {
                    a1 = sample(v, v->packets[0].data, f + 1, 0);
                    b1 = sample(v, v->packets[0].data, f + 1, 1);
                } else if (v->npackets > 1) {
                    a1 = sample(v, v->packets[1].data, 0, 0);
                    b1 = sample(v, v->packets[1].data, 0, 1);
                } else {
                    a1 = a;
                    b1 = b;
                }
            } else {
                uint32_t end = v->looping && v->loop_len ? frames_of(v, v->loop_start + v->loop_len) : frames_of(v, v->bytes);
                uint32_t nx;
                if (f >= end) {
                    uint32_t start;
                    if (!v->looping || !end) {
                        v->playing = 0;
                        v->pos = 0;
                        break;
                    }
                    start = v->loop_len ? frames_of(v, v->loop_start) : 0;
                    v->pos -= end - start;
                    audio_lap(v);
                    f = (uint32_t)v->pos;
                }
                if (!v->data) break;
                nx = f + 1 < end ? f + 1 : v->looping && end ? (v->loop_len ? frames_of(v, v->loop_start) : 0) : f;
                if (v->channels > 2) {
                    /* 5.1: each channel to its own speaker */
                    ne = v->channels < 6 ? v->channels : 6;
                    a = sample(v, v->data, f, 0);
                    b = sample(v, v->data, f, 1);
                    a1 = sample(v, v->data, nx, 0);
                    b1 = sample(v, v->data, nx, 1);
                    for (int c = 2; c < ne; c++) e0[c] = sample(v, v->data, f, c), e1[c] = sample(v, v->data, nx, c);
                } else {
                    a = sample(v, v->data, f, 0);
                    b = sample(v, v->data, f, 1);
                    a1 = sample(v, v->data, nx, 0);
                    b1 = sample(v, v->data, nx, 1);
                }
            }
            t = (float)(v->pos - f);
            x0 = a + (a1 - a) * t;
            x1 = b + (b1 - b) * t;
            {
                float *o = out + i * 6, xe[6] = {x0, x1, 0, 0, 0, 0};
                for (int c = 2; c < ne; c++) xe[c] = e0[c] + (e1[c] - e0[c]) * t;
                for (int c = 0; c < 6; c++)
                    for (int side = 0; side < 6; side++) v->gain6[c][side] += dg[c][side];
                for (int k = 0; k < 6; k++) {
                    float acc = 0;
                    for (int c = 0; c < (ne > 2 ? ne : 2); c++) acc += xe[c] * v->gain6[c][k];
                    o[k] += acc;
                }
            }
            v->pos += step;
        }
        memcpy(v->gain6, g, sizeof(g));
        if (v->fading_out) {
            v->playing = 0;
            v->fading_out = 0;
            v->has_gain = 0;
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void render(float *out, uint32_t frames)
{
    if (g_oc == 6) {
        render6(out, frames);
        return;
    }
    memset(out, 0, frames * 2 * sizeof(float));
    EnterCriticalSection(&g_lock);
    for (int vi = 0; vi < MAX_VOICES; vi++) {
        Voice *v = g_voices[vi];
        float g[2][2], dg[2][2];
        double step;
        if (!v || v->paused || (!v->stream && !v->playing)) continue;
        levels(v, g);
        if (v->fading_out) memset(g, 0, sizeof(g));
        /* xboxrecomp: a voice that starts ramps up over this pass (~5 ms) instead of starting at full level: the
         * console's voice processor smooths level steps, and a sound starting or stopping mid waveform at once
         * was heard as a click every time */
        if (!v->has_gain) memset(v->gain, 0, sizeof(v->gain));
        v->has_gain = 1;
        for (int c = 0; c < 2; c++)
            for (int side = 0; side < 2; side++) dg[c][side] = (g[c][side] - v->gain[c][side]) / (float)frames;
        step = (double)(v->freq ? v->freq : v->rate) / MIX_RATE;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)v->pos;
            float a, b, a1, b1, t, x0, x1;
            if (v->stream) {
                while (v->npackets && f >= frames_of(v, v->packets[0].size)) {
                    Packet pk = v->packets[0];
                    pop_packet(v);
                    complete(v, &pk, 0, pk.size);
                    v->pos -= frames_of(v, pk.size);
                    f = (uint32_t)v->pos;
                }
                if (!v->npackets) {
                    if (v->active && !v->ending) {  /* [AUDIO]: a playing stream with no packet left: a gap */
                        static int underruns;
                        g_stream_dry++;
                        if (underruns++ < 12)
                            fprintf(stderr, "[AUDIO] %lu ms stream %08X ran dry (the title fed it too late)\n",
                                    (unsigned long)GetTickCount(), v->obj);
                    }
                    v->pos = 0;
                    if (v->ending) deactivate(v);
                    break;
                }
                a = sample(v, v->packets[0].data, f, 0);
                b = sample(v, v->packets[0].data, f, 1);
                if (f + 1 < frames_of(v, v->packets[0].size)) {
                    a1 = sample(v, v->packets[0].data, f + 1, 0);
                    b1 = sample(v, v->packets[0].data, f + 1, 1);
                } else if (v->npackets > 1) {
                    a1 = sample(v, v->packets[1].data, 0, 0);
                    b1 = sample(v, v->packets[1].data, 0, 1);
                } else {
                    a1 = a;
                    b1 = b;
                }
            } else {
                uint32_t end = v->looping && v->loop_len ? frames_of(v, v->loop_start + v->loop_len) : frames_of(v, v->bytes);
                uint32_t nx;
                if (f >= end) {
                    uint32_t start;
                    if (!v->looping || !end) {
                        v->playing = 0;
                        v->pos = 0;
                        break;
                    }
                    start = v->loop_len ? frames_of(v, v->loop_start) : 0;
                    v->pos -= end - start;
                    audio_lap(v);
                    f = (uint32_t)v->pos;
                }
                if (!v->data) break;
                a = sample(v, v->data, f, 0);
                b = sample(v, v->data, f, 1);
                nx = f + 1 < end ? f + 1 : v->looping && end ? (v->loop_len ? frames_of(v, v->loop_start) : 0) : f;
                a1 = sample(v, v->data, nx, 0);
                b1 = sample(v, v->data, nx, 1);
            }
            t = (float)(v->pos - f);
            x0 = a + (a1 - a) * t;
            x1 = b + (b1 - b) * t;
            for (int c = 0; c < 2; c++)
                for (int side = 0; side < 2; side++) v->gain[c][side] += dg[c][side];
            out[i * 2] += x0 * v->gain[0][0] + x1 * v->gain[1][0];
            out[i * 2 + 1] += x0 * v->gain[0][1] + x1 * v->gain[1][1];
            v->pos += step;
        }
        memcpy(v->gain, g, sizeof(g));
        if (v->fading_out) {
            v->playing = 0;
            v->fading_out = 0;
            v->has_gain = 0;
        }
    }
    LeaveCriticalSection(&g_lock);
}

/* ── Output: XAudio2, or a wall clock when there is no audio device ──────── */

/* Queued output: up to OUT_BUFS - 1 blocks of 256 frames in flight. Was always 15 (~80 ms ahead of what is heard):
 * a late wake-up of this thread then mixed several blocks at once, and the play cursor the title polls to refill its
 * short looping music/ambience buffers (GetCurrentPosition) jumped past the part it had not refilled yet - the old
 * lap's audio played again, heard as constant crackling on Windows. As HP:PS (which found the same effect):
 * RECOMP_AUDIO_LATENCY_MS sets it, default 32 ms (6 blocks). */
#define OUT_BUFS 16
static int g_out_bufs = OUT_BUFS;

static void init_latency(void)
{
    const char *e = getenv("RECOMP_AUDIO_LATENCY_MS");
    int ms = e && *e ? atoi(e) : 32;
    int n = ms * MIX_RATE / 1000 / MIX_FRAMES + 1;  /* blocks queued, plus the one being filled */
    g_out_bufs = n < 3 ? 3 : n > OUT_BUFS ? OUT_BUFS : n;
    fprintf(stderr, "[DSOUND] output queue %d x %d frames (~%d ms)\n", g_out_bufs - 1, MIX_FRAMES,
            (g_out_bufs - 1) * MIX_FRAMES * 1000 / MIX_RATE);
}

/* RECOMP_DSOUND_WAV=<file>: the mixed output as it goes to XAudio2, 16-bit stereo 48 kHz, for the first 3 minutes */
static FILE *g_wav;
static uint32_t g_wav_frames;

static void wav_write(const int16_t *pcm, uint32_t frames)
{
    static int opened;
    if (!opened) {
        const char *e = getenv("RECOMP_DSOUND_WAV");
        opened = 1;
        if (e && *e && (g_wav = fopen(e, "wb")) != NULL) {
            static const uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0,
                                            0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0, 4, 0, 16, 0, 'd', 'a', 't',
                                            'a', 0, 0, 0, 0};
            fwrite(hdr, 1, sizeof hdr, g_wav);
            fprintf(stderr, "[DSOUND] recording the mix to %s\n", e);
        }
    }
    if (!g_wav) return;
    fwrite(pcm, 4, frames, g_wav);
    g_wav_frames += frames;
    if (g_wav_frames % (MIX_RATE * 5) < frames || g_wav_frames >= MIX_RATE * 180) {  /* sizes kept current */
        uint32_t data = g_wav_frames * 4, riff = data + 36;
        long at = ftell(g_wav);
        fseek(g_wav, 4, SEEK_SET);
        fwrite(&riff, 4, 1, g_wav);
        fseek(g_wav, 40, SEEK_SET);
        fwrite(&data, 4, 1, g_wav);
        fseek(g_wav, at, SEEK_SET);
        fflush(g_wav);
        if (g_wav_frames >= MIX_RATE * 180) {
            fclose(g_wav);
            g_wav = NULL;
            fprintf(stderr, "[DSOUND] mix recording finished\n");
        }
    }
}

/* output health, reported every 10 s when anything went wrong */
static unsigned g_dry, g_clipped, g_mixed;
static DWORD g_report_ms;

/* [AUDIO] the Windows playback device's own mix format (shared mode): XAudio2 mixes at 48000 Hz and Windows converts
 * to it. 2026-10-30: level music never started with the device at 192000 Hz (or 96000) unless exclusive mode was
 * allowed (which put it back to 48000) - logged so every run shows the device's rate. */
static void log_device_format(void)
{
    static const GUID clsid_enum = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    static const GUID iid_enum = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
    static const GUID iid_client = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    WAVEFORMATEX *mf = NULL;
    REFERENCE_TIME def = 0, min = 0;
    if (SUCCEEDED(CoCreateInstance(&clsid_enum, NULL, CLSCTX_ALL, &iid_enum, (void **)&en)) &&
        SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) &&
        SUCCEEDED(IMMDevice_Activate(dev, &iid_client, CLSCTX_ALL, NULL, (void **)&ac)) &&
        SUCCEEDED(IAudioClient_GetMixFormat(ac, &mf))) {
        IAudioClient_GetDevicePeriod(ac, &def, &min);
        fprintf(stderr, "[AUDIO] Windows device mix format: %lu Hz, %u channels, %u-bit, period %.1f ms (min %.1f)%s\n",
                (unsigned long)mf->nSamplesPerSec, mf->nChannels, mf->wBitsPerSample, def / 10000.0, min / 10000.0,
                mf->nSamplesPerSec != MIX_RATE ? " - not 48000 Hz: Windows resamples our output" : "");
        CoTaskMemFree(mf);
    } else {
        fprintf(stderr, "[AUDIO] Windows device mix format: unknown\n");
    }
    if (ac) IAudioClient_Release(ac);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
}

static DWORD WINAPI mixer_thread(LPVOID unused)
{
    static float mix[MIX_FRAMES * 6];
    static int16_t pcm[OUT_BUFS][MIX_FRAMES * 6];
    IXAudio2 *xa2 = NULL;
    IXAudio2MasteringVoice *master = NULL;
    IXAudio2SourceVoice *src = NULL;
    WAVEFORMATEX wfx = {0}, *wfx_out = &wfx;
    LARGE_INTEGER freq, now, next;
    int nb = 0;
    (void)unused;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    log_device_format();
    init_latency();
    /* late wake ups of this thread are what empties the device queue; the CPU itself is mostly idle */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    g_report_ms = GetTickCount();
    g_oc = app_Speakers() == 6 ? 6 : 2;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)g_oc;
    wfx.nSamplesPerSec = MIX_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(2 * g_oc);
    wfx.nAvgBytesPerSec = MIX_RATE * 2 * (DWORD)g_oc;
    {
        /* 5.1: the speaker positions the bins stand for (front L R, centre, LFE, back L R) */
        static WAVEFORMATEXTENSIBLE wfe;
        static const GUID pcm_guid = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        wfe.Format = wfx;
        wfe.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfe.Format.cbSize = sizeof(wfe) - sizeof(WAVEFORMATEX);
        wfe.Samples.wValidBitsPerSample = 16;
        wfe.dwChannelMask = 0x3F;
        wfe.SubFormat = pcm_guid;
        if (g_oc == 6) wfx_out = &wfe.Format;
    }
    if (FAILED(XAudio2Create(&xa2, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(IXAudio2_CreateMasteringVoice(xa2, &master, (UINT32)g_oc, MIX_RATE, 0, NULL, NULL, 0)) ||
        FAILED(IXAudio2_CreateSourceVoice(xa2, &src, wfx_out, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL)) ||
        FAILED(IXAudio2SourceVoice_Start(src, 0, XAUDIO2_COMMIT_NOW))) {
        fprintf(stderr, "[DSOUND] XAudio2 unavailable: mixing on a wall clock, silently\n");
        src = NULL;
    } else {
        fprintf(stderr, "[DSOUND] XAudio2 output, %d Hz %s\n", MIX_RATE, g_oc == 6 ? "5.1" : "stereo");
    }

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    for (;;) {
        {   /* [AUDIO] timing: a pass starting long after the last, then the blocks rendered back to back after it */
            static LARGE_INTEGER last;
            static unsigned burst;
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            if (last.QuadPart) {
                double ms = (double)(t.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
                double block_ms = MIX_FRAMES * 1000.0 / MIX_RATE;
                if (ms > g_late_max_ms) g_late_max_ms = ms;
                if (ms > 2 * block_ms) {
                    g_late++;
                    burst = 0;
                } else if (ms < block_ms * 0.25) {
                    if (++burst > g_burst_max) g_burst_max = burst;
                } else {
                    burst = 0;
                }
            }
            last = t;
        }
        g_trace_ui = app_AudioTrace();
        render(mix, MIX_FRAMES);
        for (int i = 0; i < MIX_FRAMES * g_oc; i++) {
            float s = mix[i] < -1.0f ? -1.0f : mix[i] > 1.0f ? 1.0f : mix[i];
            if (s != mix[i]) g_clipped++;
            pcm[nb][i] = (int16_t)(s * 32767.0f);
        }
        {   /* [AUDIO] clicks: a spike in the mix's second difference, per channel (music's own transients rarely
             * jump a third of full scale from one sample to the next and straight back) */
            static float p1[2], p2[2];
            for (int i = 0; i < MIX_FRAMES; i++)
                for (int c = 0; c < 2; c++) {
                    float x = pcm[nb][i * g_oc + c] / 32767.0f, d2 = x - 2 * p1[c] + p2[c];  /* front pair */
                    if (fabsf(d2) > 0.66f) {
                        g_clicks++;
                        if (g_click_lines++ < 12) {
                            char vs[512];
                            EnterCriticalSection(&g_lock);
                            audio_voices(vs, sizeof vs);
                            LeaveCriticalSection(&g_lock);
                            fprintf(stderr, "[AUDIO] %lu ms click in the mix (jump %.2f) | playing:%s\n",
                                    (unsigned long)GetTickCount(), d2, vs);
                        }
                    }
                    p2[c] = p1[c];
                    p1[c] = x;
                }
        }
        if (g_oc == 2) wav_write(pcm[nb], MIX_FRAMES);  /* RECOMP_DSOUND_WAV: stereo only */
        if (src) {
            XAUDIO2_BUFFER xb = {0};
            XAUDIO2_VOICE_STATE st;
            /* xboxrecomp: paced by the clock, a block every 5.3 ms, not whenever the device queue has room. XAudio2
             * takes its buffers in 10 ms quanta, so "room" came two blocks at a time and a late wake-up let several
             * go back to back: the play cursor the title polls to refill its 50 ms streaming loops (gameplay music
             * and ambience) jumped up to two thirds of a lap at once, past what it had refilled - old audio, a faint
             * crackle in gameplay only. Early only when the queue runs low; the queue cap still holds. */
            /* 2026-10-22: pacing at exactly real time kept only ~1 block queued, and every 10 ms XAudio2 quantum
             * emptied it ("gaps" 70 per 10 s = the crackle). Now the queue is held at its full depth: a block is
             * rendered as soon as one has been played, so XAudio2's quanta take two blocks at a time (10 ms, a
             * fifth of a streaming loop) and the queue never runs dry. */
            {
                static int period_set;
                if (!period_set) { timeBeginPeriod(1); period_set = 1; }
                for (;;) {
                    IXAudio2SourceVoice_GetState(src, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                    if (st.BuffersQueued < (UINT32)(g_out_bufs - 1)) break;
                    Sleep(1);
                }
            }
            if (st.BuffersQueued == 0 && g_mixed > (unsigned)g_out_bufs) g_dry++;  /* the device played everything: a gap */
            g_mixed++;
            {
                DWORD now_ms = GetTickCount();
                if (now_ms - g_report_ms >= 10000) {
                    if (g_dry || g_clipped || g_clicks || g_stale || g_late || g_jump_max > 0.5 || g_stream_dry)
                        fprintf(stderr, "[AUDIO] %lu ms last %lus: clicks %u, stale laps %u, late passes %u (longest "
                                "gap %.1f ms, longest burst %u blocks), largest cursor jump between polls %.0f%% of a "
                                "loop, gaps %u, clipped samples %u, streams run dry %u\n",
                                (unsigned long)now_ms, (unsigned long)((now_ms - g_report_ms) / 1000), g_clicks,
                                g_stale, g_late, g_late_max_ms, g_burst_max, g_jump_max * 100.0, g_dry, g_clipped,
                                g_stream_dry);
                    hpcos_music_report();  /* hle_debug.c: [MUSICSTATE] */
                    g_dry = g_clipped = g_clicks = g_stale = g_late = g_burst_max = g_stream_dry = 0;
                    g_late_max_ms = 0;
                    g_jump_max = 0;
                    g_report_ms = now_ms;
                }
            }
            xb.AudioBytes = MIX_FRAMES * 2 * (UINT32)g_oc;
            xb.pAudioData = (const BYTE *)pcm[nb];
            IXAudio2SourceVoice_SubmitSourceBuffer(src, &xb, NULL);
            nb = (nb + 1) % OUT_BUFS;
        } else {
            next.QuadPart += freq.QuadPart * MIX_FRAMES / MIX_RATE;
            for (;;) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= next.QuadPart) break;
                Sleep(1);
            }
        }
    }
    return 0;
}

static void start(void)
{
    if (g_started) return;
    g_started = 1;
    CreateThread(NULL, 0, mixer_thread, NULL, 0, NULL);
}

/* ── Shared helpers ─────────────────────────────────────────────────────── */

static void read_format(Voice *v, uint32_t wfx)
{
    if (!wfx) return;
    v->tag = MEM16(wfx);
    v->channels = MEM16(wfx + 2) ? MEM16(wfx + 2) : 1;
    v->rate = MEM32(wfx + 4);
    v->align = MEM16(wfx + 12) ? MEM16(wfx + 12) : 1;
    v->bits = MEM16(wfx + 14);
    if (v->tag != 1 && v->tag != 0x69) fprintf(stderr, "[DSOUND] format tag 0x%X not supported\n", v->tag);
}

/* DSMIXBINS { count, pairs[] of { bin, volume } }: the voice's bins and volumes. */
static void set_bins(Voice *v, uint32_t mixbins)
{
    uint32_t n, pairs;
    v->nbins = 0;  /* none listed: front left and right */
    if (!mixbins) return;
    n = MEM32(mixbins);
    if (n > 8) n = 8;
    pairs = MEM32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) {
        v->bin[i] = MEM32(pairs + i * 8) & 31;
        v->bin_mb[v->bin[i]] = (int32_t)MEM32(pairs + i * 8 + 4);
    }
    v->nbins = (uint8_t)n;
}

static void set_bin_volumes(Voice *v, uint32_t mixbins)
{
    uint32_t n, pairs;
    if (!mixbins) return;
    n = MEM32(mixbins);
    if (n > 32) n = 32;
    pairs = MEM32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) v->bin_mb[MEM32(pairs + i * 8) & 31] = (int32_t)MEM32(pairs + i * 8 + 4);
}

/* 600 mB unless 3D, or a mix-in or effect-in buffer */
static float default_headroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : db(-600); }

static Voice *find(uint32_t obj)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i] && g_voices[i]->obj == obj) return g_voices[i];
    return NULL;
}

static Voice *new_voice(uint32_t obj_size)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (!g_voices[i]) {
            Voice *v = calloc(1, sizeof(Voice));
            v->obj = xbox_HeapAlloc(obj_size, 16);
            memset((void *)XBOX_PTR(v->obj), 0, obj_size);
            v->refs = 1;
            v->tag = 1;
            v->channels = 1;
            v->bits = 16;
            v->align = 2;
            v->rate = MIX_RATE;
            v->max_packets = 1;
            v->volume = v->headroom = 1;
            v->block = -1;
            g_voices[i] = v;
            return v;
        }
    fprintf(stderr, "[DSOUND] out of voices (%d)\n", MAX_VOICES);
    return NULL;
}

static void free_voice(Voice *v)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i] == v) g_voices[i] = NULL;
    xbox_HeapFree(v->obj);
    free(v);
}

static void init_lock(void)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1)) InitializeCriticalSection(&g_lock);
}

/* Completed packets' callbacks run as the XDK runs them: on the title's own
 * thread, from DirectSoundDoWork or the stream call that flushed them. The
 * callback is __stdcall (stream context, packet context, status). */
static void run_callbacks(uint32_t only_voice, int all)
{
    Done run[MAX_DONE];
    int n = 0, keep = 0;
    uint32_t saved_eax = g_eax;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ndone; i++) {
        if (all || g_done[i].voice == only_voice) run[n++] = g_done[i];
        else g_done[keep++] = g_done[i];
    }
    g_ndone = keep;
    LeaveCriticalSection(&g_lock);
    if (n) TRACE("  %d packet callback(s)%s", n, all ? " from DoWork" : " from flush");
    for (int i = 0; i < n; i++) {
        PUSH32(g_esp, run[i].status);
        PUSH32(g_esp, run[i].packet_context);
        PUSH32(g_esp, run[i].context);
        PUSH32(g_esp, 0);  /* return address */
        RECOMP_ICALL(run[i].callback);
    }
    g_eax = saved_eax;
}

#define FALLBACK(addr) do { if (!hle_on()) { sub_##addr##_gen(); return; } init_lock(); } while (0)

/* ── Device ─────────────────────────────────────────────────────────────── */

/* DirectSoundCreate(pguid, ppDirectSound, pUnkOuter) */
extern void sub_0014613F_gen(void);
void sub_0014613F(void)
{
    FALLBACK(0014613F);
    EnterCriticalSection(&g_lock);
    start();
    if (!g_ds) {
        g_ds = xbox_HeapAlloc(0x40, 16);
        memset((void *)XBOX_PTR(g_ds), 0, 0x40);
    }
    MEM32(ARG(1)) = g_ds;
    LeaveCriticalSection(&g_lock);
    TRACE("DirectSoundCreate -> %08X", g_ds);
    ret_stdcall(S_OK_, 3);
}

/* IDirectSound_Release(this) */
extern void sub_00144641_gen(void);
void sub_00144641(void) { FALLBACK(00144641); ret_stdcall(0, 1); }

/* IDirectSound_SynchPlayback(this) */
extern void sub_00145237_gen(void);
void sub_00145237(void) { FALLBACK(00145237); ret_stdcall(S_OK_, 1); }

/* IDirectSound_SetI3DL2Listener(this, pds3dl, dwApply): no room reverb here */
extern void sub_00145C3F_gen(void);
void sub_00145C3F(void) { FALLBACK(00145C3F); ret_stdcall(S_OK_, 3); }

/* IDirectSound_DownloadEffectsImage(this, pvImageBuffer, dwImageSize,
 * pImageLoc, ppImageDesc). Effects run on the console's DSP and are not
 * reproduced; the title gets a zeroed descriptor with no effects. */
extern void sub_00145210_gen(void);
void sub_00145210(void)
{
    FALLBACK(00145210);
    if (ARG(4)) {
        uint32_t desc = xbox_HeapAlloc(0x400, 16);
        memset((void *)XBOX_PTR(desc), 0, 0x400);
        MEM32(ARG(4)) = desc;
    }
    TRACE("DownloadEffectsImage(%u bytes): not reproduced", ARG(2));
    ret_stdcall(S_OK_, 5);
}

/* DirectSoundDoWork() */
extern void sub_00145390_gen(void);
void sub_00145390(void)
{
    FALLBACK(00145390);
    run_callbacks(0, 1);
    g_esp += 4;
    g_eax = 0;
}

/* ── Buffers ────────────────────────────────────────────────────────────── */

/* DSBUFFERDESC { dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin } */
static uint32_t new_buffer(uint32_t desc, uint32_t out)
{
    Voice *v;
    uint32_t flags = MEM32(desc + 4), bytes = MEM32(desc + 8);
    EnterCriticalSection(&g_lock);
    start();
    v = new_voice(0x40);
    if (!v) {
        LeaveCriticalSection(&g_lock);
        return E_FAIL_;
    }
    read_format(v, MEM32(desc + 12));
    set_bins(v, MEM32(desc + 16));
    v->headroom = default_headroom(flags);
    if (bytes) {
        v->data = v->owned = xbox_HeapAlloc(bytes, 16);
        v->bytes = v->loop_len = bytes;
    }
    MEM32(out) = v->obj;
    LeaveCriticalSection(&g_lock);
    TRACE("CreateSoundBuffer flags=%X bytes=%u tag=%X ch=%u rate=%u -> %08X",
          flags, bytes, v->tag, v->channels, v->rate, v->obj);
    return S_OK_;
}

/* DirectSoundCreateBuffer(pdsbd, ppBuffer) */
extern void sub_00146186_gen(void);
void sub_00146186(void) { FALLBACK(00146186); ret_stdcall(new_buffer(ARG(0), ARG(1)), 2); }

/* IDirectSound_CreateSoundBuffer(this, pdsbd, ppBuffer, pUnkOuter) */
extern void sub_00145F75_gen(void);
void sub_00145F75(void) { FALLBACK(00145F75); ret_stdcall(new_buffer(ARG(1), ARG(2)), 4); }

/* One buffer method: look the voice up under the lock, run the body. */
typedef uint32_t (*buf_fn)(Voice *v);

static void buffer_call(void (*gen)(void), buf_fn fn, const char *name, int nargs)
{
    Voice *v;
    uint32_t r;
    if (!hle_on()) { gen(); return; }
    init_lock();
    EnterCriticalSection(&g_lock);
    v = find(ARG(0));
    r = v ? fn(v) : E_FAIL_;
    LeaveCriticalSection(&g_lock);
    /* the calls a title makes every frame for every sound (status, cursor, volume, levels) are left out: logged, they were
     * ~100k lines a few minutes, and the log writes held up the title's music refills */
    if (strcmp(name, "GetStatus") && strcmp(name, "GetCurrentPosition") && strcmp(name, "SetVolume") &&
        strcmp(name, "SetMixBinVolumes"))
        TRACE("Buffer_%s(%08X, %08X, %08X) = %08X", name, ARG(0), nargs > 1 ? ARG(1) : 0, nargs > 2 ? ARG(2) : 0, r);
    ret_stdcall(r, nargs);
}

#define BUFFER(name) static uint32_t buf_##name(Voice *v)

BUFFER(Release)
{
    if (--v->refs > 0) return (uint32_t)v->refs;
    if (v->owned) xbox_HeapFree(v->owned);
    free_voice(v);
    return 0;
}

BUFFER(SetBufferData)
{
    if (ARG(1) == v->data && ARG(2) == v->bytes) return S_OK_;
    if (v->fading_out) v->playing = v->fading_out = 0;  /* new data: the old sound's fade ends here */
    if (v->owned && v->owned != ARG(1)) {
        xbox_HeapFree(v->owned);
        v->owned = 0;
    }
    v->data = ARG(1);
    if (!ARG(1) && ARG(2)) v->data = v->owned = xbox_HeapAlloc(ARG(2), 16);  /* a size with no data */
    v->bytes = v->loop_len = ARG(2);
    v->loop_start = 0;
    v->pos = 0;
    v->block = -1;
    return S_OK_;
}

/* Play(this, dwReserved1, dwReserved2, dwFlags): DSBPLAY_LOOPING = 1, DSBPLAY_FROMSTART = 2 */
BUFFER(Play)
{
    if (!v->playing || v->fading_out) v->has_gain = 0;  /* a start ramps up from silence */
    v->fading_out = 0;
    if (ARG(3) & 1)
        fprintf(stderr, "[AUDIO] %lu ms play looping buffer %08X (%u bytes, %u Hz x%u %s, %.0f ms a lap)%s\n",
                (unsigned long)GetTickCount(), v->obj, v->bytes, v->freq ? v->freq : v->rate, v->channels,
                v->tag == 0x69 ? "ADPCM" : v->bits == 8 ? "8-bit" : "16-bit",
                frames_of(v, v->bytes) * 1000.0 / (double)(v->freq ? v->freq : v->rate ? v->rate : MIX_RATE),
                v->playing ? " (already playing)" : "");
    if (g_trace || g_trace_ui) {
        char bins[160];
        int n = 0;
        for (uint32_t b = 0; b < v->nbins && n < (int)sizeof bins - 16; b++)
            n += snprintf(bins + n, sizeof bins - n, " %u:%d", v->bin[b], (int)v->bin_mb[v->bin[b]]);
        if (!v->nbins) snprintf(bins, sizeof bins, " (none)");
        TRACE("%lu ms play %08X data %08X %u bytes x%u flags %X%s, bins (bin:mB)%s", (unsigned long)GetTickCount(),
              v->obj, v->data, v->bytes, v->channels, ARG(3), v->playing ? " (already playing)" : "", bins);
    }
    v->playing = 1;
    v->looping = ARG(3) & 1;
    if (ARG(3) & 2) v->pos = 0;
    return S_OK_;
}

BUFFER(Stop)
{
    /* the console's voice processor ramps a voice down; cutting it mid waveform clicked */
    if (v->playing && v->looping)
        fprintf(stderr, "[AUDIO] %lu ms stop looping buffer %08X (%u laps, %u refilled, %u stale)\n",
                (unsigned long)GetTickCount(), v->obj, v->laps, v->refills, v->stale_laps);
    if (v->playing) v->fading_out = 1;
    return S_OK_;
}

BUFFER(SetLoopRegion)
{
    if (ARG(1) > v->bytes || ARG(2) > v->bytes - ARG(1)) return DSERR_INVALID;
    v->loop_start = ARG(1);
    v->loop_len = ARG(2) ? ARG(2) : v->bytes - ARG(1);
    return S_OK_;
}

/* DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4 */
BUFFER(GetStatus)
{
    int on = v->playing && !v->fading_out;
    MEM32(ARG(1)) = (on ? 1u : 0u) | (on && v->looping ? 4u : 0u);
    return S_OK_;
}

BUFFER(GetCurrentPosition)
{
    uint32_t p = bytes_of(v, (uint32_t)v->pos);
    if (v->polls && v->looping && v->bytes) {  /* [AUDIO]: how far the cursor moved since the title last looked */
        double d = (double)((p + v->bytes - v->last_poll_pos) % v->bytes) / (double)v->bytes;
        if (d > g_jump_max) g_jump_max = d;
    }
    v->last_poll_pos = p;
    v->polls++;
    v->last_poll_ms = GetTickCount();
    if (ARG(1)) MEM32(ARG(1)) = p;
    if (ARG(2)) MEM32(ARG(2)) = v->bytes ? (p + bytes_of(v, ADPCM_FRAMES * 4)) % v->bytes : 0;
    return S_OK_;
}

BUFFER(SetCurrentPosition)
{
    v->pos = frames_of(v, ARG(1));
    return S_OK_;
}

BUFFER(SetVolume)
{
    v->volume = db((int32_t)ARG(1));
    return S_OK_;
}

BUFFER(SetFrequency)
{
    v->freq = ARG(1);
    return S_OK_;
}

BUFFER(SetFilter) { (void)v; return S_OK_; }

BUFFER(SetMixBins)
{
    set_bins(v, ARG(1));
    return S_OK_;
}

BUFFER(SetMixBinVolumes)
{
    set_bin_volumes(v, ARG(1));
    return S_OK_;
}

/* ── Streams: the object carries the XDK's own vtables ──────────────────── */

/* DirectSoundCreateStream(pdssd, ppStream)
 * DSSTREAMDESC { dwFlags, dwMaxAttachedPackets, lpwfxFormat, lpfnCallback,
 *                lpvContext, lpMixBins } */
extern void sub_001461DD_gen(void);
void sub_001461DD(void)
{
    uint32_t desc, flags;
    Voice *v;
    FALLBACK(001461DD);
    desc = ARG(0);
    flags = MEM32(desc);
    EnterCriticalSection(&g_lock);
    start();
    v = new_voice(0x80);
    if (!v) {
        LeaveCriticalSection(&g_lock);
        ret_stdcall(E_FAIL_, 2);
        return;
    }
    MEM32(v->obj) = STREAM_VTABLE;
    MEM32(v->obj + 4) = STREAM_VTABLE2;
    v->stream = 1;
    v->max_packets = MEM32(desc + 4) ? MEM32(desc + 4) : 1;
    if (v->max_packets > MAX_PACKETS) v->max_packets = MAX_PACKETS;
    read_format(v, MEM32(desc + 8));
    v->callback = MEM32(desc + 12);
    v->context = MEM32(desc + 16);
    set_bins(v, MEM32(desc + 20));
    v->headroom = default_headroom(flags);
    MEM32(ARG(1)) = v->obj;
    LeaveCriticalSection(&g_lock);
    TRACE("CreateStream flags=%X packets=%u tag=%X ch=%u rate=%u cb=%08X -> %08X",
          flags, v->max_packets, v->tag, v->channels, v->rate, v->callback, v->obj);
    ret_stdcall(S_OK_, 2);
}

/* Flush: every queued packet completes as flushed, and the stream's pending
 * callbacks go to the calling thread before the call returns. */
static void flush_stream(Voice *v)
{
    for (int i = 0; i < v->npackets; i++) complete(v, &v->packets[i], XMEDIAPACKET_STATUS_FLUSHED, 0);
    v->npackets = 0;
    v->pos = 0;
    deactivate(v);
}

typedef uint32_t (*str_fn)(Voice *v, int *flushed);

static void stream_call(void (*gen)(void), str_fn fn, const char *name, int nargs)
{
    Voice *v;
    uint32_t r, obj = ARG(0);
    int flushed = 0;
    if (!hle_on()) { gen(); return; }
    init_lock();
    EnterCriticalSection(&g_lock);
    v = find(obj);
    r = v ? fn(v, &flushed) : E_FAIL_;
    LeaveCriticalSection(&g_lock);
    TRACE("Stream_%s(%08X, %08X) = %08X", name, obj, nargs > 1 ? ARG(1) : 0, r);
    ret_stdcall(r, nargs);
    if (flushed) run_callbacks(obj, 0);
}

#define STREAM(name) static uint32_t str_##name(Voice *v, int *flushed)

STREAM(AddRef)
{
    (void)flushed;
    return (uint32_t)++v->refs;
}

STREAM(Release)
{
    if (--v->refs > 0) return (uint32_t)v->refs;
    flush_stream(v);
    *flushed = 1;
    /* the voice goes, its callbacks still run: they only carry the guest
     * object's address, which stays allocated until they have */
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i] == v) g_voices[i] = NULL;
    free(v);
    return 0;
}

/* XMEDIAINFO { dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead } */
STREAM(GetInfo)
{
    uint32_t info = ARG(1);
    (void)flushed;
    MEM32(info) = 0x3;  /* fixed sample size, fixed packet alignment */
    MEM32(info + 4) = v->tag == 0x69 ? 36u * v->channels : v->align;
    MEM32(info + 8) = 0;
    MEM32(info + 12) = 0;
    return S_OK_;
}

/* XMO_STATUSF_ACCEPT_INPUT = 1 */
STREAM(GetStatus)
{
    (void)flushed;
    MEM32(ARG(1)) = (uint32_t)v->npackets < v->max_packets ? 1u : 0u;
    return S_OK_;
}

/* Process(this, pInputPacket, pOutputPacket)
 * XMEDIAPACKET { pvBuffer, dwMaxSize, pdwCompletedSize, pdwStatus, pContext, prtTimestamp } */
STREAM(Process)
{
    uint32_t in = ARG(1);
    Packet pk;
    (void)flushed;
    if (!in) return S_OK_;
    if (v->npackets >= MAX_PACKETS) return E_FAIL_;
    pk.data = MEM32(in);
    pk.size = MEM32(in + 4);
    pk.completed_ptr = MEM32(in + 8);
    pk.status_ptr = MEM32(in + 12);
    pk.context = MEM32(in + 16);
    if (pk.completed_ptr) MEM32(pk.completed_ptr) = 0;
    if (pk.status_ptr) MEM32(pk.status_ptr) = XMEDIAPACKET_STATUS_PENDING;
    v->packets[v->npackets++] = pk;
    TRACE("  packet %08X size=%u queued=%d", pk.data, pk.size, v->npackets);
    v->active = 1;
    v->ending = 0;
    return S_OK_;
}

STREAM(Discontinuity)
{
    if (v->npackets) {
        v->ending = 1;
    } else if (v->active) {
        flush_stream(v);
        *flushed = 1;
    }
    return S_OK_;
}

STREAM(Flush)
{
    flush_stream(v);
    *flushed = 1;
    return S_OK_;
}

/* FlushEx(this, rtTimeStamp (64-bit), dwFlags): DSSTREAMFLUSHEX_ASYNC = 1
 * flushes only an active stream */
STREAM(FlushEx)
{
    if ((ARG(3) & 1) && !v->active) return S_OK_;
    flush_stream(v);
    *flushed = 1;
    return S_OK_;
}

/* Pause(this, dwPause): DSSTREAMPAUSE_PAUSE = 1, DSSTREAMPAUSE_RESUME = 0 */
STREAM(Pause)
{
    (void)flushed;
    if ((ARG(1) == 1) != (v->paused != 0))
        fprintf(stderr, "[AUDIO] %lu ms stream %08X %s (%d packets queued)\n", (unsigned long)GetTickCount(), v->obj,
                ARG(1) == 1 ? "paused" : "resumed", v->npackets);
    v->paused = ARG(1) == 1;
    return S_OK_;
}

/* ── Entry points: literal definitions, so --exclude-manual finds them ──── */

/* IDirectSoundBuffer_* */
extern void sub_00144657_gen(void);
void sub_00144657(void) { buffer_call(sub_00144657_gen, buf_Release, "Release", 1); }
extern void sub_00145C5F_gen(void);
void sub_00145C5F(void) { buffer_call(sub_00145C5F_gen, buf_SetBufferData, "SetBufferData", 3); }
extern void sub_001452BF_gen(void);
void sub_001452BF(void) { buffer_call(sub_001452BF_gen, buf_Play, "Play", 4); }
extern void sub_001452E3_gen(void);
void sub_001452E3(void) { buffer_call(sub_001452E3_gen, buf_Stop, "Stop", 1); }
extern void sub_001452FB_gen(void);
void sub_001452FB(void) { buffer_call(sub_001452FB_gen, buf_SetLoopRegion, "SetLoopRegion", 3); }
extern void sub_0014531B_gen(void);
void sub_0014531B(void) { buffer_call(sub_0014531B_gen, buf_GetStatus, "GetStatus", 2); }
extern void sub_00145337_gen(void);
void sub_00145337(void) { buffer_call(sub_00145337_gen, buf_GetCurrentPosition, "GetCurrentPosition", 3); }
extern void sub_00145357_gen(void);
void sub_00145357(void) { buffer_call(sub_00145357_gen, buf_SetCurrentPosition, "SetCurrentPosition", 2); }
extern void sub_0014524F_gen(void);
void sub_0014524F(void) { buffer_call(sub_0014524F_gen, buf_SetVolume, "SetVolume", 2); }
extern void sub_0014569F_gen(void);
void sub_0014569F(void) { buffer_call(sub_0014569F_gen, buf_SetFrequency, "SetFrequency", 2); }
extern void sub_0014526B_gen(void);
void sub_0014526B(void) { buffer_call(sub_0014526B_gen, buf_SetFilter, "SetFilter", 2); }
extern void sub_00145287_gen(void);
void sub_00145287(void) { buffer_call(sub_00145287_gen, buf_SetMixBins, "SetMixBins", 2); }
extern void sub_001452A3_gen(void);
void sub_001452A3(void) { buffer_call(sub_001452A3_gen, buf_SetMixBinVolumes, "SetMixBinVolumes", 2); }

/* CDirectSoundStream_*, reached through the stream vtable */
extern void sub_00144DAD_gen(void);
void sub_00144DAD(void) { stream_call(sub_00144DAD_gen, str_AddRef, "AddRef", 1); }
extern void sub_00144DF4_gen(void);
void sub_00144DF4(void) { stream_call(sub_00144DF4_gen, str_Release, "Release", 1); }
extern void sub_00144E42_gen(void);
void sub_00144E42(void) { stream_call(sub_00144E42_gen, str_GetInfo, "GetInfo", 2); }
extern void sub_00144F41_gen(void);
void sub_00144F41(void) { stream_call(sub_00144F41_gen, str_GetStatus, "GetStatus", 2); }
extern void sub_00144F92_gen(void);
void sub_00144F92(void) { stream_call(sub_00144F92_gen, str_Process, "Process", 3); }
extern void sub_00144EA9_gen(void);
void sub_00144EA9(void) { stream_call(sub_00144EA9_gen, str_Discontinuity, "Discontinuity", 1); }
extern void sub_00144EF6_gen(void);
void sub_00144EF6(void) { stream_call(sub_00144EF6_gen, str_Flush, "Flush", 1); }
extern void sub_0014505A_gen(void);
void sub_0014505A(void) { stream_call(sub_0014505A_gen, str_FlushEx, "FlushEx", 4); }
extern void sub_00145009_gen(void);
void sub_00145009(void) { stream_call(sub_00145009_gen, str_Pause, "Pause", 2); }

/* IDirectSoundStream_Pause: a jmp to CDirectSoundStream_Pause */
extern void sub_00145373_gen(void);
void sub_00145373(void)
{
    FALLBACK(00145373);
    sub_00145009();
}

/* IDirectSoundStream_FlushEx: forwards its four arguments */
extern void sub_00145378_gen(void);
void sub_00145378(void)
{
    FALLBACK(00145378);
    sub_0014505A();
}

// chime.cpp: the chimes (chime.h). Each is a few struck notes, made into a 48 kHz WAV in memory and
// handed to PlaySound, which plays it on the default output without blocking. lifelog never records
// its own process, so a chime does not end up in the recordings.

#include <mmsystem.h>

static const u32 CHIME_RATE = 48000;
static const i32 CHIME_MAX = CHIME_RATE;  // 1 s, longer than any chime

// One note: its pitch (Hz), when it starts and how long it rings (s), and how loud against the rest.
struct ChimeNote {
    f32 freq, start, length, level;
};

struct ChimeShape {
    ChimeNote notes[3];
    i32 count;
};

// G5 784, B5 988, C6 1047, E6 1319, G4 392, D5 587
static const ChimeShape CHIME_SHAPES[CHIME_COUNT] = {
    {{{784, 0, 0.35f, 1}, {1047, 0.07f, 0.45f, 1}}, 2},                           // rising fourth: go ahead
    {{{1047, 0, 0.5f, 0.8f}, {1319, 0.06f, 0.6f, 1}}, 2},                         // bright rising third
    {{{784, 0, 0.4f, 0.8f}, {988, 0.07f, 0.4f, 0.8f}, {1319, 0.14f, 0.7f, 1}}, 3},  // three up
    {{{988, 0, 0.35f, 1}, {784, 0.11f, 0.5f, 1}}, 2},                             // falling: "hm?"
    {{{392, 0, 0.3f, 1}, {392, 0.16f, 0.45f, 1}}, 2},                             // low double knock
    {{{587, 0, 0.45f, 0.8f}}, 1},                                                 // one soft low note
};

const char* chime_label(Chime c) {
    static const char* labels[CHIME_COUNT] = {"Listening", "Done", "Clip saved", "Didn't catch", "Error", "Cancelled"};
    return c >= 0 && c < CHIME_COUNT ? labels[c] : "";
}

// The WAV PlaySound is playing from: it reads it while it plays, so it stays put until the next one.
struct ChimeWav {
    u8 header[WAV_HEADER];
    i16 samples[CHIME_MAX];
};
static ChimeWav g_chime_wav;
static f32 g_chime_mix[CHIME_MAX];

static void chime_play_as(Chime c, ChimeStyle style, u32 volume) {
    if (c < 0 || c >= CHIME_COUNT || style == CHIME_OFF || volume == 0) return;
    if (volume > 100) volume = 100;
    PlaySoundW(0, 0, 0);  // stop the last one before its samples are written over

    const ChimeShape* shape = &CHIME_SHAPES[c];
    i32 count = 0;
    for (i32 n = 0; n < shape->count; n++) {
        i32 end = (i32)((shape->notes[n].start + shape->notes[n].length) * CHIME_RATE);
        if (end > count) count = end;
    }
    count += CHIME_RATE / 50;  // 20 ms of tail
    if (count > CHIME_MAX) count = CHIME_MAX;
    memset(g_chime_mix, 0, sizeof(f32) * count);

    const f32 tau = 6.2831853f;
    for (i32 n = 0; n < shape->count; n++) {
        ChimeNote note = shape->notes[n];
        i32 from = (i32)(note.start * CHIME_RATE), length = (i32)(note.length * CHIME_RATE);
        for (i32 i = 0; i < length && from + i < count; i++) {
            f32 t = (f32)i / CHIME_RATE;
            f32 attack = t < 0.004f ? t / 0.004f : 1;
            f32 v;
            if (style == CHIME_GLASS) {
                // Bell-like: the note plus a quiet, quickly fading partial that is not a harmonic.
                v = expf(-t * 7) * (sinf(tau * note.freq * t) + 0.25f * expf(-t * 14) * sinf(tau * note.freq * 2.76f * t));
            } else {
                // A round pluck: the note plus two octaves up, gone almost at once.
                v = expf(-t * 11) * (sinf(tau * note.freq * t) + 0.18f * expf(-t * 25) * sinf(tau * note.freq * 4 * t));
            }
            g_chime_mix[from + i] += v * attack * note.level;
        }
    }

    f32 peak = 0;
    for (i32 i = 0; i < count; i++) peak = fabsf(g_chime_mix[i]) > peak ? fabsf(g_chime_mix[i]) : peak;
    // Loudness follows the slider more evenly than the raw amplitude would.
    f32 v01 = volume / 100.0f;
    f32 gain = (peak > 0 ? 0.55f / peak : 0) * v01 * sqrtf(v01);
    i32 fade = CHIME_RATE / 66;  // the last 15 ms, so it never ends on a click
    for (i32 i = 0; i < count; i++) {
        f32 x = g_chime_mix[i] * gain;
        if (i >= count - fade) x *= (f32)(count - 1 - i) / fade;
        g_chime_wav.samples[i] = (i16)(x * 32767);
    }

    wav_header(g_chime_wav.header, CHIME_RATE, (u32)count * 2);
    PlaySoundW((LPCWSTR)&g_chime_wav, 0, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void chime_play(App* app, Chime c) { chime_play_as(c, app->config.chimes.style, app->config.chimes.volume); }

void chime_set_style(App* app, ChimeStyle style) {
    if (app->config.chimes.style == style) return;
    app->config.chimes.style = style;
    LOG("chimes %s", chime_style_name(style));
    config_save_string("chimes", "style", chime_style_name(style));
}

void chime_set_volume(App* app, u32 volume) {
    if (volume > 100) volume = 100;
    if (app->config.chimes.volume == volume) return;
    app->config.chimes.volume = volume;
    LOG("chime volume %u%%", volume);
    config_save_u32("chimes", "volume", volume);
}

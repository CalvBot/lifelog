// chime.cpp: the chimes (chime.h). Each is a few notes on a small synthesised instrument, placed
// across left and right, kept low and warm, with a soft echo and a little room. It is made into a 48 kHz stereo
// WAV in memory and handed to PlaySound, which plays it on the default output without blocking.
// lifelog never records its own process, so a chime does not end up in the recordings.

#include <mmsystem.h>

static const u32 CHIME_RATE = 48000;
static const i32 CHIME_MAX = CHIME_RATE * 2;  // frames: 2 s, longer than any chime and its tail
static const f32 CHIME_TAIL = 0.75f;          // seconds of echo and room after the last note
static const f32 TAU = 6.2831853f;

// One note: its pitch (Hz), when it starts and how long it rings (s), how loud against the rest,
// and where it sits, from -1 (left) to 1 (right).
struct ChimeNote {
    f32 freq, start, length, level, pan;
};

struct ChimeShape {
    ChimeNote notes[6];
    i32 count;
};

// Low and warm, around middle C: A3 220, B♭3 233, C4 262, D4 294, E4 330, G4 392, B4 494, C5 523, E5 659
static const ChimeShape CHIME_SHAPES[CHIME_COUNT] = {
    // Listening: a quick rising arpeggio, left to right: go ahead.
    {{{294, 0, 0.6f, 0.8f, -0.3f}, {392, 0.07f, 0.6f, 0.8f, 0}, {494, 0.14f, 0.8f, 0.9f, 0.3f}}, 3},
    // Done: a major chord strummed into bloom, an octave on top.
    {{{262, 0, 1.0f, 0.8f, -0.3f}, {330, 0.035f, 1.0f, 0.7f, 0.05f}, {392, 0.07f, 1.1f, 0.7f, 0.3f}, {523, 0.15f, 1.0f, 0.35f, 0}}, 4},
    // Didn't catch: down and back up a little, like "hm?".
    {{{330, 0, 0.4f, 0.9f, 0.2f}, {262, 0.13f, 0.4f, 0.8f, -0.2f}, {294, 0.26f, 0.7f, 0.85f, 0.1f}}, 3},
    // Error: two low knocks, each a rub of two notes a semitone apart.
    {{{233, 0, 0.35f, 1, -0.2f}, {220, 0, 0.35f, 0.9f, 0.2f}, {233, 0.18f, 0.5f, 1, -0.2f}, {220, 0.18f, 0.5f, 0.9f, 0.2f}}, 4},
    // Cancelled: a soft fall of a fourth.
    {{{392, 0, 0.5f, 0.7f, 0.2f}, {294, 0.14f, 0.8f, 0.6f, -0.2f}}, 2},
};

const char* chime_label(Chime c) {
    static const char* labels[CHIME_COUNT] = {"Listening", "Done", "Didn't catch", "Error", "Cancelled"};
    return c >= 0 && c < CHIME_COUNT ? labels[c] : "";
}

// ---------------------------------------------------------------------------------------------
// Instruments: one note into one channel. `detune` nudges the pitch (1 is none), a little
// differently left and right, which makes the note wide and shimmering.

// An electric-piano bell: gentle FM at a whole-number ratio (so it stays in tune with itself), the
// brightness fading in the first moments, and a soft body an octave below.
static void glass_note(f32* out, i32 frames, f32 freq, f32 detune, f32 level) {
    f32 f = freq * detune;
    for (i32 i = 0; i < frames; i++) {
        f32 t = (f32)i / CHIME_RATE;
        f32 index = 1.1f * expf(-t * 6);
        f32 tone = sinf(TAU * f * t + index * sinf(TAU * f * t));
        f32 v = expf(-t * 2.6f) * tone + 0.45f * expf(-t * 3) * sinf(TAU * f * 0.5f * t);
        f32 attack = t < 0.005f ? t / 0.005f : 1;
        out[i] += v * attack * level;
    }
}

// A low marimba: the bar's main overtone (about 4x) kept short, a round octave below, and the soft
// knock of a felt mallet.
static void soft_note(f32* out, i32 frames, f32 freq, f32 detune, f32 level, u32 seed) {
    f32 f = freq * detune;
    for (i32 i = 0; i < frames; i++) {
        f32 t = (f32)i / CHIME_RATE;
        seed = seed * 1664525 + 1013904223;
        f32 noise = (f32)(seed >> 8) / (f32)(1 << 24) * 2 - 1;
        f32 v = expf(-t * 5) * sinf(TAU * f * t) + 0.12f * expf(-t * 30) * sinf(TAU * f * 3.93f * t) +
                0.5f * expf(-t * 5.5f) * sinf(TAU * f * 0.5f * t) + 0.04f * expf(-t * 600) * noise;
        f32 attack = t < 0.004f ? t / 0.004f : 1;
        out[i] += v * attack * level;
    }
}

// A saw wave without the harsh edges (PolyBLEP): `phase` goes 0..1, `step` is freq / rate.
static f32 saw(f32 phase, f32 step) {
    f32 v = 2 * phase - 1;
    if (phase < step) {
        f32 x = phase / step;
        v -= x + x - x * x - 1;
    } else if (phase > 1 - step) {
        f32 x = (phase - 1) / step;
        v -= x * x + x + x + 1;
    }
    return v;
}

// A synth pluck: two saws a few cents apart through a low-pass filter that opens a little and closes
// again, over a sine an octave below.
static void synth_note(f32* out, i32 frames, f32 freq, f32 detune, f32 level) {
    f32 f1 = freq * detune * 0.997f, f2 = freq * detune * 1.003f;
    f32 p1 = 0, p2 = 0.37f, low = 0, band = 0;
    for (i32 i = 0; i < frames; i++) {
        f32 t = (f32)i / CHIME_RATE;
        f32 s1 = f1 / CHIME_RATE, s2 = f2 / CHIME_RATE;
        f32 in = 0.5f * (saw(p1, s1) + saw(p2, s2));
        p1 += s1;
        p2 += s2;
        if (p1 >= 1) p1 -= 1;
        if (p2 >= 1) p2 -= 1;
        // A state-variable filter; its cutoff starts high and falls towards the note.
        f32 cutoff = freq * (1.2f + 2.5f * expf(-t * 7));
        if (cutoff > 2500) cutoff = 2500;
        f32 k = 2 * sinf(3.1415927f * cutoff / CHIME_RATE);
        low += k * band;
        f32 high = in - low - 0.7f * band;
        band += k * high;
        f32 attack = t < 0.008f ? t / 0.008f : 1;
        f32 sub = 0.5f * sinf(TAU * freq * 0.5f * t);
        out[i] += (low * 0.6f + sub) * attack * expf(-t * 4) * level;
    }
}

// ---------------------------------------------------------------------------------------------
// The room: a small reverb (four damped combs, then two all-passes, per channel) after a
// ping-pong echo.

static const i32 COMB_LEN[4] = {1214, 1293, 1390, 1476};
static const i32 PASS_LEN[2] = {605, 480};
static const i32 RIGHT_SPREAD = 25;  // the right channel's filters are a little longer: width

struct ChimeRoom {
    f32 comb[2][4][1600];
    f32 comb_store[2][4];
    i32 comb_at[2][4];
    f32 pass[2][2][700];
    i32 pass_at[2][2];
};

static f32 room_step(ChimeRoom* r, i32 ch, f32 x) {
    f32 out = 0;
    for (i32 c = 0; c < 4; c++) {
        i32 len = COMB_LEN[c] + ch * RIGHT_SPREAD;
        f32* buf = r->comb[ch][c];
        i32* at = &r->comb_at[ch][c];
        f32 y = buf[*at];
        r->comb_store[ch][c] = y * 0.55f + r->comb_store[ch][c] * 0.45f;  // damping: highs die first
        buf[*at] = x + r->comb_store[ch][c] * 0.78f;
        if (++*at >= len) *at = 0;
        out += y;
    }
    for (i32 p = 0; p < 2; p++) {
        i32 len = PASS_LEN[p] + ch * RIGHT_SPREAD;
        f32* buf = r->pass[ch][p];
        i32* at = &r->pass_at[ch][p];
        f32 held = buf[*at];
        buf[*at] = out + held * 0.5f;
        out = held - out;
        if (++*at >= len) *at = 0;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Playing

// The WAV PlaySound is playing from: it reads it while it plays, so it stays put until the next one.
struct ChimeWav {
    u8 header[WAV_HEADER];
    i16 samples[CHIME_MAX * 2];  // left, right, left, ...
};
static ChimeWav g_chime_wav;
static f32 g_chime_dry[2][CHIME_MAX];
static f32 g_chime_echo[2][CHIME_MAX];
static f32 g_chime_note[CHIME_MAX];  // one note of one channel, before it is faded and added
static ChimeRoom g_chime_room;

static void chime_play_as(Chime c, ChimeStyle style, u32 volume) {
    if (c < 0 || c >= CHIME_COUNT || style == CHIME_OFF || volume == 0) return;
    if (volume > 100) volume = 100;
    PlaySoundW(0, 0, 0);  // stop the last one before its samples are written over

    const ChimeShape* shape = &CHIME_SHAPES[c];
    f32 last = 0;
    for (i32 n = 0; n < shape->count; n++) {
        f32 end = shape->notes[n].start + shape->notes[n].length;
        if (end > last) last = end;
    }
    i32 count = (i32)((last + CHIME_TAIL) * CHIME_RATE);
    if (count > CHIME_MAX) count = CHIME_MAX;
    memset(g_chime_dry, 0, sizeof(g_chime_dry));
    memset(g_chime_echo, 0, sizeof(g_chime_echo));
    memset(&g_chime_room, 0, sizeof(g_chime_room));

    // The notes, each placed by equal-power panning and given a short fade so it never clicks off.
    for (i32 n = 0; n < shape->count; n++) {
        ChimeNote note = shape->notes[n];
        i32 from = (i32)(note.start * CHIME_RATE);
        i32 frames = (i32)(note.length * CHIME_RATE);
        if (from + frames > count) frames = count - from;
        if (frames <= 0) continue;
        f32 angle = (note.pan + 1) * 0.25f * 3.1415927f;
        f32 gains[2] = {cosf(angle), sinf(angle)};
        f32 detunes[2] = {0.9985f, 1.0015f};
        i32 fade = CHIME_RATE / 25;  // 40 ms
        if (fade > frames) fade = frames;
        for (i32 ch = 0; ch < 2; ch++) {
            f32* note_out = g_chime_note;
            memset(note_out, 0, sizeof(f32) * frames);
            f32 level = note.level * gains[ch];
            if (style == CHIME_GLASS) glass_note(note_out, frames, note.freq, detunes[ch], level);
            else if (style == CHIME_SOFT) soft_note(note_out, frames, note.freq, 1 + (detunes[ch] - 1) * 0.5f, level, 12345 + n);
            else synth_note(note_out, frames, note.freq, 1 + (detunes[ch] - 1) * 2, level);
            for (i32 i = frames - fade; i < frames; i++) note_out[i] *= (f32)(frames - 1 - i) / fade;
            f32* out = g_chime_dry[ch] + from;
            for (i32 i = 0; i < frames; i++) out[i] += note_out[i];
        }
    }

    // Echo: the first repeat on the left, the next on the right, and so on, each quieter.
    i32 delay = (i32)(0.13f * CHIME_RATE);
    for (i32 i = delay; i < count; i++) {
        f32 mono = 0.5f * (g_chime_dry[0][i - delay] + g_chime_dry[1][i - delay]);
        g_chime_echo[0][i] = 0.5f * mono + 0.3f * g_chime_echo[1][i - delay];
        g_chime_echo[1][i] = 0.3f * g_chime_echo[0][i - delay];
    }

    // Mix the dry notes, the echo and the room; take the top off (two gentle low-pass steps at
    // about 3 kHz) so nothing is shrill; then find the loudest point.
    f32 peak = 0;
    f32 lp[2][2] = {};
    f32 k = 1 - expf(-TAU * 3000.0f / CHIME_RATE);
    for (i32 i = 0; i < count; i++) {
        for (i32 ch = 0; ch < 2; ch++) {
            f32 dry = g_chime_dry[ch][i], echo = g_chime_echo[ch][i];
            f32 room = room_step(&g_chime_room, ch, (dry + echo) * 0.03f);
            f32 x = dry + 0.15f * echo + 0.3f * room;
            lp[ch][0] += k * (x - lp[ch][0]);
            lp[ch][1] += k * (lp[ch][0] - lp[ch][1]);
            x = lp[ch][1];
            g_chime_dry[ch][i] = x;
            if (fabsf(x) > peak) peak = fabsf(x);
        }
    }

    // Loudness follows the slider more evenly than the raw amplitude would.
    f32 v01 = volume / 100.0f;
    f32 gain = (peak > 0 ? 0.6f / peak : 0) * v01 * sqrtf(v01);
    i32 fade = CHIME_RATE / 6;  // the last 170 ms of the tail, so it never ends on a click
    for (i32 i = 0; i < count; i++) {
        f32 end = i >= count - fade ? (f32)(count - 1 - i) / fade : 1;
        for (i32 ch = 0; ch < 2; ch++) {
            f32 x = g_chime_dry[ch][i] * gain * end;
            if (x > 1) x = 1;
            if (x < -1) x = -1;
            g_chime_wav.samples[i * 2 + ch] = (i16)(x * 32767);
        }
    }

    wav_header(g_chime_wav.header, CHIME_RATE, (u32)count * 4, 2);
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

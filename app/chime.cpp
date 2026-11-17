// chime.cpp: the chimes (chime.h). Each is a few notes played on a recorded instrument, placed
// across left and right with a little room around them. The recordings (assets\chimes, built into
// the exe by lifelog.rc) are single notes from VCSL by Versilian Studios (CC0); each note of a
// chime is the nearest recording played faster or slower to reach its pitch. The chime is made into
// a 48 kHz stereo WAV in memory and handed to PlaySound, which plays it on the default output
// without blocking. lifelog never records its own process, so a chime does not end up in the
// recordings.

#include <mmsystem.h>

static const u32 CHIME_RATE = 48000;          // the recordings' rate too
static const i32 CHIME_MAX = CHIME_RATE * 3;  // frames: 3 s, longer than any chime and its tail
static const f32 CHIME_TAIL = 0.5f;           // seconds of room after the last note
static const f32 CHIME_RING = 1.8f;           // a note rings this many times its length before it fades

// One note: its pitch (Hz), when it starts and how long it lasts (s), how loud against the rest,
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
    // Clip saved: a run up two octaves, sweeping across.
    {{{262, 0, 0.45f, 0.8f, -0.5f},
      {330, 0.06f, 0.45f, 0.75f, -0.25f},
      {392, 0.12f, 0.45f, 0.75f, 0},
      {523, 0.18f, 0.55f, 0.7f, 0.25f},
      {659, 0.24f, 0.9f, 0.55f, 0.5f}},
     5},
    // Didn't catch: down and back up a little, like "hm?".
    {{{330, 0, 0.4f, 0.9f, 0.2f}, {262, 0.13f, 0.4f, 0.8f, -0.2f}, {294, 0.26f, 0.7f, 0.85f, 0.1f}}, 3},
    // Error: two low knocks, each a rub of two notes a semitone apart.
    {{{233, 0, 0.35f, 1, -0.2f}, {220, 0, 0.35f, 0.9f, 0.2f}, {233, 0.18f, 0.5f, 1, -0.2f}, {220, 0.18f, 0.5f, 0.9f, 0.2f}}, 4},
    // Cancelled: a soft fall of a fourth.
    {{{392, 0, 0.5f, 0.7f, 0.2f}, {294, 0.14f, 0.8f, 0.6f, -0.2f}}, 2},
};

const char* chime_label(Chime c) {
    static const char* labels[CHIME_COUNT] = {"Listening", "Done", "Clip saved", "Didn't catch", "Error", "Cancelled"};
    return c >= 0 && c < CHIME_COUNT ? labels[c] : "";
}

// ---------------------------------------------------------------------------------------------
// The recordings: two notes per instrument, as lifelog.rc numbers them (100 + style * 2 + n),
// each 48 kHz mono 16-bit.

static const i32 CHIME_ROOTS = 2;
static const f32 CHIME_ROOT_HZ[CHIME_OFF][CHIME_ROOTS] = {
    {293.66f, 440.00f},  // vibes: D4, A4
    {261.63f, 392.00f},  // marimba: C4, G4
    {293.66f, 392.00f},  // kalimba: D4, G4
    {261.63f, 440.00f},  // bells: C4, A4
};

struct ChimeSample {
    const i16* pcm;  // in the exe's resources; null if it could not be found
    i32 frames;
    f32 root;
};
static ChimeSample g_chime_samples[CHIME_OFF][CHIME_ROOTS];
static bool g_chime_loaded;

// The PCM in a WAV's "data" chunk.
static ChimeSample sample_from_wav(const u8* wav, u32 size, f32 root) {
    ChimeSample s = {};
    u32 at = 12;
    while (at + 8 <= size) {
        u32 len;
        memcpy(&len, wav + at + 4, 4);
        if (!memcmp(wav + at, "data", 4) && at + 8 + len <= size) {
            s.pcm = (const i16*)(wav + at + 8);
            s.frames = (i32)(len / 2);
            s.root = root;
            break;
        }
        at += 8 + len + (len & 1);
    }
    return s;
}

static void chime_load() {
    if (g_chime_loaded) return;
    g_chime_loaded = true;
    for (i32 style = 0; style < CHIME_OFF; style++) {
        for (i32 n = 0; n < CHIME_ROOTS; n++) {
            HRSRC res = FindResourceW(0, MAKEINTRESOURCEW(100 + style * CHIME_ROOTS + n), (LPCWSTR)RT_RCDATA);
            HGLOBAL data = res ? LoadResource(0, res) : 0;
            const u8* wav = data ? (const u8*)LockResource(data) : 0;
            if (!wav) {
                LOG("chimes: recording %d is missing from the exe", 100 + style * CHIME_ROOTS + n);
                continue;
            }
            g_chime_samples[style][n] = sample_from_wav(wav, SizeofResource(0, res), CHIME_ROOT_HZ[style][n]);
        }
    }
}

// The recording nearest in pitch to `freq`, so it is changed as little as possible.
static const ChimeSample* sample_for(ChimeStyle style, f32 freq) {
    const ChimeSample* best = 0;
    f32 best_dist = 0;
    for (i32 n = 0; n < CHIME_ROOTS; n++) {
        const ChimeSample* s = &g_chime_samples[style][n];
        if (!s->pcm) continue;
        f32 dist = fabsf(logf(freq / s->root));
        if (!best || dist < best_dist) {
            best = s;
            best_dist = dist;
        }
    }
    return best;
}

// A sample between two frames, by a cubic through the four around it: smoother than a straight line
// when the recording is played faster or slower.
static f32 sample_at(const ChimeSample* s, f64 pos) {
    i32 i = (i32)pos;
    f32 t = (f32)(pos - i);
    f32 p[4];
    for (i32 k = 0; k < 4; k++) {
        i32 at = i - 1 + k;
        p[k] = at >= 0 && at < s->frames ? s->pcm[at] / 32768.0f : 0;
    }
    f32 a = -0.5f * p[0] + 1.5f * p[1] - 1.5f * p[2] + 0.5f * p[3];
    f32 b = p[0] - 2.5f * p[1] + 2 * p[2] - 0.5f * p[3];
    f32 c = -0.5f * p[0] + 0.5f * p[2];
    return ((a * t + b) * t + c) * t + p[1];
}

// ---------------------------------------------------------------------------------------------
// The room: a small reverb (four damped combs, then two all-passes, per channel).

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
        buf[*at] = x + r->comb_store[ch][c] * 0.75f;
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
static f32 g_chime_mix[2][CHIME_MAX];
static ChimeRoom g_chime_room;

static void chime_play_as(Chime c, ChimeStyle style, u32 volume) {
    if (c < 0 || c >= CHIME_COUNT || style < 0 || style >= CHIME_OFF || volume == 0) return;
    if (volume > 100) volume = 100;
    chime_load();
    PlaySoundW(0, 0, 0);  // stop the last one before its samples are written over

    const ChimeShape* shape = &CHIME_SHAPES[c];
    f32 last = 0;
    for (i32 n = 0; n < shape->count; n++) {
        f32 end = shape->notes[n].start + shape->notes[n].length * CHIME_RING;
        if (end > last) last = end;
    }
    i32 count = (i32)((last + CHIME_TAIL) * CHIME_RATE);
    if (count > CHIME_MAX) count = CHIME_MAX;
    memset(g_chime_mix, 0, sizeof(g_chime_mix));
    memset(&g_chime_room, 0, sizeof(g_chime_room));

    // The notes, each placed by equal-power panning and faded at the end so it never clicks off.
    bool any = false;
    for (i32 n = 0; n < shape->count; n++) {
        ChimeNote note = shape->notes[n];
        const ChimeSample* s = sample_for(style, note.freq);
        if (!s) continue;
        f64 step = note.freq / s->root;
        i32 from = (i32)(note.start * CHIME_RATE);
        i32 frames = (i32)(note.length * CHIME_RING * CHIME_RATE);
        i32 has = (i32)((s->frames - 1) / step);
        if (frames > has) frames = has;
        if (from + frames > count) frames = count - from;
        if (frames <= 0) continue;
        any = true;
        f32 angle = (note.pan + 1) * 0.25f * 3.1415927f;
        f32 gains[2] = {cosf(angle) * note.level, sinf(angle) * note.level};
        i32 fade = CHIME_RATE / 8;  // 125 ms
        if (fade > frames) fade = frames;
        for (i32 i = 0; i < frames; i++) {
            f32 v = sample_at(s, i * step);
            if (i >= frames - fade) v *= (f32)(frames - 1 - i) / fade;
            g_chime_mix[0][from + i] += v * gains[0];
            g_chime_mix[1][from + i] += v * gains[1];
        }
    }
    if (!any) return;

    // Add the room, then find the loudest point.
    f32 peak = 0;
    for (i32 i = 0; i < count; i++) {
        for (i32 ch = 0; ch < 2; ch++) {
            f32 dry = g_chime_mix[ch][i];
            f32 x = dry + 0.12f * room_step(&g_chime_room, ch, dry * 0.05f);
            g_chime_mix[ch][i] = x;
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
            f32 x = g_chime_mix[ch][i] * gain * end;
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

// player.cpp: playing kept audio back (see clip.h). Each play() starts a thread that mixes the
// audio in pieces (clip.cpp) and feeds it to the default output through WASAPI. play_stop() or a
// newer play() tells the thread to end; the thread frees its own state.
//
// Playing on fetches 30 s pieces one ahead, so it does not stop to wait. The output buffer holds
// 2 s, which covers the time a piece takes to mix.

#include <mmdeviceapi.h>
#include <audioclient.h>

struct Player {
    App* app;
    i64 line_id;
    PlayMode mode;
    Names hidden;
    volatile LONG stop;
};

// A piece handed to the output: its samples, and where in all that was written it starts.
struct Piece {
    Mix mix;
    i64 base;
};

struct Output {
    IMMDeviceEnumerator* devices;
    IMMDevice* device;
    IAudioClient* client;
    IAudioRenderClient* render;
    UINT32 frames;  // the buffer's size
    bool started;
};

static bool output_open(Output* o, char* error, i32 cap) {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&o->devices);
    if (SUCCEEDED(hr)) hr = o->devices->GetDefaultAudioEndpoint(eRender, eConsole, &o->device);
    if (SUCCEEDED(hr)) hr = o->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void**)&o->client);
    if (SUCCEEDED(hr)) {
        // The clips' own format; Windows converts it to the device's.
        WAVEFORMATEX f = {};
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = 1;
        f.nSamplesPerSec = 16000;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 2;
        f.nAvgBytesPerSec = 32000;
        hr = o->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                   2 * 10000000LL, 0, &f, 0);
    }
    if (SUCCEEDED(hr)) hr = o->client->GetBufferSize(&o->frames);
    if (SUCCEEDED(hr)) hr = o->client->GetService(__uuidof(IAudioRenderClient), (void**)&o->render);
    if (FAILED(hr)) snprintf(error, cap, "no audio output (0x%08lx)", (unsigned long)hr);
    return SUCCEEDED(hr);
}

static void output_close(Output* o) {
    if (o->client && o->started) o->client->Stop();
    if (o->render) o->render->Release();
    if (o->client) o->client->Release();
    if (o->device) o->device->Release();
    if (o->devices) o->devices->Release();
    *o = {};
}

// Frames still waiting in the buffer; -1 if the device went away.
static i64 output_waiting(Output* o) {
    UINT32 padding = 0;
    if (FAILED(o->client->GetCurrentPadding(&padding))) return -1;
    return padding;
}

// Writes as much of `samples` as fits; returns how many went in, -1 if the device went away.
static i64 output_write(Output* o, const i16* samples, i64 count) {
    i64 waiting = output_waiting(o);
    if (waiting < 0) return -1;
    i64 room = (i64)o->frames - waiting;
    i64 n = count < room ? count : room;
    if (n <= 0) return 0;
    BYTE* data = 0;
    if (FAILED(o->render->GetBuffer((UINT32)n, &data))) return -1;
    memcpy(data, samples, (size_t)n * 2);
    o->render->ReleaseBuffer((UINT32)n, 0);
    return n;
}

// The recording time at `played` frames into what was written.
static f64 pieces_time(List<Piece>* pieces, i64 played) {
    if (!pieces->count) return 0;
    Piece* p = &pieces->items[0];
    for (i32 i = 0; i < pieces->count && pieces->items[i].base <= played; i++) p = &pieces->items[i];
    f64 pos = (f64)(played - p->base) / AUDIO_RATE;
    if (!p->mix.jumps.count) return 0;
    TimeJump j = p->mix.jumps.items[0];
    for (i32 i = 0; i < p->mix.jumps.count && p->mix.jumps.items[i].at <= pos; i++) j = p->mix.jumps.items[i];
    return j.ts + (pos - j.at);
}

static bool player_stopped(Player* pl) { return pl->stop || stopping(pl->app); }

// The thread's work: returns when it is done, told to stop, or failed (with `error`).
// The main thread: where playback is now. `a` is the Player (only compared), `b` the moment.
static void player_at_apply(App* app, Event* e) {
    if ((Player*)e->a != app->clips.player) return;
    memcpy(&app->clips.play_ts, &e->b, sizeof(f64));
    wake_window(app);
}

static void player_run(Player* pl, sqlite3* db, char* error, i32 cap) {
    App* app = pl->app;
    wchar_t adir[MAX_PATH];
    audio_dir(settings(app), adir);

    // The line it starts from.
    sqlite3_stmt* st = prepare(db, "SELECT speaker, start_ts, end_ts FROM segments WHERE id = ?1");
    if (!st) {
        snprintf(error, cap, "could not read the database");
        return;
    }
    sqlite3_bind_int64(st, 1, pl->line_id);
    Arena own = {};
    Str speaker = {};
    f64 start = 0, end = 0;
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) {
        speaker = col_str(st, 0, &own);
        start = sqlite3_column_double(st, 1);
        end = sqlite3_column_double(st, 2);
    }
    sqlite3_finalize(st);
    if (!found) {
        snprintf(error, cap, "that line is gone");
        arena_release(&own);
        return;
    }

    bool on = pl->mode == PLAY_ALL_ON || pl->mode == PLAY_STREAM_ON;
    Keep keep = {&pl->hidden, pl->mode == PLAY_STREAM_ON ? speaker : Str{}};
    f64 from = start - 0.3;
    List<Piece> pieces = {};
    Output out = {};

    // The first piece.
    Piece first = {};
    bool ok = false;
    if (pl->mode == PLAY_LINE) {
        ok = line_audio(db, adir, pl->line_id, &first.mix, error, cap);
    } else if (pl->mode == PLAY_ALL_AT_LINE) {
        f64 to = end + 0.4 < from + 3600 ? end + 0.4 : from + 3600;  // at most an hour
        ok = mix_audio(db, adir, from, to, &keep, &first.mix, error, cap);
        if (ok && !first.mix.count) {
            snprintf(error, cap, "no audio kept for then (audio is kept for a week)");
            ok = false;
        }
    } else {
        f64 next = 0;
        ok = mix_from(db, adir, from, 30.0, &keep, &first.mix, &next, error, cap);
        if (!ok && !error[0]) snprintf(error, cap, "no more audio");
        if (ok) from = next;
    }
    if (!ok || player_stopped(pl)) {
        mix_free(&first.mix);
        arena_release(&own);
        return;
    }
    list_push(&pieces, first);
    bool opened = output_open(&out, error, cap);

    i64 written = 0;    // frames handed to the output
    i32 current = 0;    // the piece being written
    i64 offset = 0;     // into it
    f64 told = 0;       // when the window was last told where it is
    while (opened && !player_stopped(pl)) {
        // Write what fits.
        bool lost = false;
        while (current < pieces.count) {
            Piece* p = &pieces.items[current];
            i64 n = output_write(&out, p->mix.samples + offset, p->mix.count - offset);
            if (n < 0) lost = true;
            if (n <= 0) break;
            offset += n;
            written += n;
            if (offset < p->mix.count) break;
            current++;
            offset = 0;
        }
        if (lost) {
            snprintf(error, cap, "the audio output went away");
            break;
        }
        if (!out.started) {
            out.client->Start();
            out.started = true;
        }
        // Keep one piece queued behind the one playing.
        if (on && pieces.count - current < 2) {
            Piece next = {};
            f64 after = 0;
            char e[256] = {};
            if (mix_from(db, adir, from, 30.0, &keep, &next.mix, &after, e, sizeof(e)) && !player_stopped(pl)) {
                Piece* last = &pieces.items[pieces.count - 1];
                next.base = last->base + last->mix.count;
                list_push(&pieces, next);
                if (after > from) from = after;
                else on = false;
            } else {
                mix_free(&next.mix);
                on = false;  // nothing further (an error after the first piece just ends it)
            }
        }
        i64 waiting = output_waiting(&out);
        if (waiting < 0) {
            snprintf(error, cap, "the audio output went away");
            break;
        }
        i64 played = written - waiting;
        if (!on && current == pieces.count && waiting == 0) break;  // all of it was heard
        // Pieces heard to the end are not needed any more, apart from their times.
        for (i32 i = 0; i + 1 < pieces.count && pieces.items[i + 1].base <= played; i++) {
            free(pieces.items[i].mix.samples);
            pieces.items[i].mix.samples = 0;
        }
        f64 now = now_ts();
        if (now - told >= 0.05) {
            told = now;
            f64 ts = pieces_time(&pieces, played);
            i64 bits;
            memcpy(&bits, &ts, sizeof(bits));
            event_post(app, player_at_apply, (i64)pl, bits, 0);
        }
        WaitForSingleObject(app->stop, 15);
    }
    output_close(&out);
    for (i32 i = 0; i < pieces.count; i++) mix_free(&pieces.items[i].mix);
    list_free(&pieces);
    arena_release(&own);
}

struct PlayerDone {
    Player* pl;
    char error[256];
};

// The main thread: the player thread has ended.
static void player_done(App* app, Job* job) {
    PlayerDone* d = (PlayerDone*)job->data;
    Player* pl = d->pl;
    if (app->clips.player == pl) {
        app->clips.player = 0;
        app->clips.playing_line = 0;
        app->clips.play_ts = 0;
        if (d->error[0] && !pl->stop) ui_say(app, "%s", d->error);
        wake_window(app);
    }
    list_free(&pl->hidden);
    free(pl);
}

static DWORD WINAPI player_thread(void* p) {
    Player* pl = (Player*)p;
    App* app = pl->app;
    SetThreadDescription(GetCurrentThread(), L"player");
    CoInitializeEx(0, COINIT_MULTITHREADED);
    Job* job = job_new(player_done);
    PlayerDone* d = PUSH(&job->arena, PlayerDone);
    d->pl = pl;
    sqlite3* db = db_open_path(settings(app), true);
    if (!db) snprintf(d->error, sizeof(d->error), "could not open the database");
    else player_run(pl, db, d->error, sizeof(d->error));
    sqlite3_close(db);
    if (d->error[0]) WARN("playback: %s", d->error);
    CoUninitialize();
    // From here the Player is the main thread's to free.
    job->data = d;
    job_post_wait(app, job);
    return 0;
}

void play(App* app, i64 line_id, PlayMode mode, Names* hidden) {
    play_stop(app);
    Player* pl = (Player*)calloc(1, sizeof(Player));
    CHECK(pl, "memory for the player");
    pl->app = app;
    pl->line_id = line_id;
    pl->mode = mode;
    for (i32 i = 0; hidden && i < hidden->count; i++) list_push(&pl->hidden, hidden->items[i]);
    app->clips.player = pl;
    app->clips.playing_line = line_id;
    app->clips.play_mode = mode;
    app->clips.play_ts = 0;
    HANDLE t = CreateThread(0, 0, player_thread, pl, 0, 0);
    CHECK(t, "the player thread to start (error %lu)", GetLastError());
    CloseHandle(t);
}

void play_stop(App* app) {
    // The thread ends on its own once it sees `stop`; player_done frees its state.
    Player* pl = (Player*)app->clips.player;
    if (pl) InterlockedExchange(&pl->stop, 1);
    app->clips.player = 0;
    app->clips.playing_line = 0;
    app->clips.play_ts = 0;
}

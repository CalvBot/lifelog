// capture.cpp: see capture.h. One thread per stream (the mic, or one app's process tree) takes
// 48 kHz stereo float from WASAPI, mixes it to mono, brings it down to 16 kHz and feeds it to its
// segmenter. Once a second the watcher thread posts what plays sound, and the main thread starts
// and stops the app streams from that (events.h). None of these threads take App.lock.

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <audioclientactivationparams.h>

#pragma comment(lib, "mmdevapi.lib")

static const u32 IN_RATE = 48000;
static const u32 CHANNELS = 2;
static const u32 WAIT_MS = 500;
static const f64 LINGER_SECS = 30;  // an app stream ends once its app has been silent this long

// ---------------------------------------------------------------------------------------------
// The segmenter: turns a continuous 16 kHz mono stream into short WAV files in the spool folder.
//
// It is only a cheap loudness gate so that silence is never written to disk; the worker runs a
// real voice detector before transcribing. A segment opens when the sound gets loud, and closes
// after a pause, or at the first short pause once it is long enough, or at a hard length limit.

static const i32 RATE = 16000;
static const i32 BLOCK = RATE / 50;            // 20 ms
static const i32 PREROLL_BLOCKS = 15;          // 300 ms kept from before it got loud
static const i32 CLOSE_AFTER_QUIET = 60;       // 1.2 s of quiet ends a segment
static const i32 SOFT_LIMIT = 20 * RATE;       // past this, any short pause ends it
static const i32 SOFT_QUIET = 15;              // 300 ms
static const i32 HARD_LIMIT = 30 * RATE;
static const i32 MIN_LOUD_BLOCKS = 10;         // under 200 ms of sound is a click, not speech

// What a stream is; written next to each WAV so the worker can label the text.
struct StreamMeta {
    Source source;
    char exe[64];
    u32 pid;
    char title[256];
    char device[128];
};

struct Segmenter {
    wchar_t spool[MAX_PATH];
    char name[64];  // in file names only
    f32 gate;
    f32 pending[BLOCK];
    i32 pending_n;
    f32 preroll[PREROLL_BLOCKS][BLOCK];  // a ring
    i32 preroll_first;
    i32 preroll_n;
    bool open;
    f64 start_ts;
    f32* samples;  // HARD_LIMIT of them
    i32 n;
    i32 quiet_run;
    i32 loud_blocks;
    StreamMeta meta;  // copied when a segment opens
    f64 loud_ts;
};

// `text` as a JSON string, or null when empty.
static Str json_text(Arena* a, const char* text) {
    if (!text[0]) return str("null");
    i64 len = (i64)strlen(text);
    char* out = PUSH_ARRAY(a, char, len * 6 + 3);
    i64 n = 0;
    out[n++] = '"';
    for (i64 i = 0; i < len; i++) {
        u8 c = (u8)text[i];
        if (c == '"' || c == '\\') out[n++] = '\\', out[n++] = (char)c;
        else if (c < 0x20) n += sprintf_s(out + n, 7, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n++] = '"';
    return {out, n};
}

static void seg_write(Segmenter* seg, f64 end_ts) {
    Arena* a = scratch();
    Mark m = mark(a);
    i32 err = SHCreateDirectoryExW(0, seg->spool, 0);
    if (err != ERROR_SUCCESS && err != ERROR_ALREADY_EXISTS) {
        // The data drive may be unplugged: nothing is written anywhere else.
        ERR("%s: could not make the spool folder %ls (error %d); a segment is lost", seg->name, seg->spool, err);
        rewind(m);
        return;
    }
    StreamMeta* meta = &seg->meta;
    Str pid = meta->pid ? str_fmt(a, "%u", meta->pid) : str("null");
    Str json = str_fmt(a, "{\"start_ts\":%.6f,\"end_ts\":%.6f,\"sample_rate\":%d,\"source\":\"%s\",\"exe\":%s,\"pid\":%s,\"title\":%s,\"device\":%s}",
                       seg->start_ts, end_ts, RATE, meta->source == SOURCE_MIC ? "mic" : "app", json_text(a, meta->exe).ptr,
                       pid.ptr, json_text(a, meta->title).ptr, json_text(a, meta->device).ptr);
    wchar_t stem[MAX_PATH], path[MAX_PATH], part[MAX_PATH];
    swprintf(stem, MAX_PATH, L"%s\\%014llu-%s", seg->spool, (u64)(seg->start_ts * 1000.0), wide(a, str(seg->name)));
    swprintf(path, MAX_PATH, L"%s.json", stem);
    // The worker picks up *.wav, so the sidecar goes first and the WAV appears by a rename.
    if (!write_file(path, json)) {
        ERR("%s: could not write %ls (error %lu)", seg->name, path, GetLastError());
        rewind(m);
        return;
    }
    u32 data_len = (u32)seg->n * 2;
    u8* wav = PUSH_ARRAY(a, u8, 44 + data_len);
    u8* p = wav;
    auto put = [&](const void* v, u32 len) { memcpy(p, v, len), p += len; };
    u32 riff = 36 + data_len, fmt_len = 16, rate = RATE, bytes_per_sec = RATE * 2;
    u16 pcm = 1, mono = 1, align = 2, bits = 16;
    put("RIFF", 4), put(&riff, 4), put("WAVEfmt ", 8), put(&fmt_len, 4), put(&pcm, 2), put(&mono, 2);
    put(&rate, 4), put(&bytes_per_sec, 4), put(&align, 2), put(&bits, 2), put("data", 4), put(&data_len, 4);
    for (i32 i = 0; i < seg->n; i++) {
        f32 s = seg->samples[i];
        s = s < -1 ? -1 : s > 1 ? 1 : s;
        i16 v = (i16)(s * 32767.0f);
        put(&v, 2);
    }
    swprintf(part, MAX_PATH, L"%s.wav.part", stem);
    swprintf(path, MAX_PATH, L"%s.wav", stem);
    HANDLE f = CreateFileW(part, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    DWORD wrote = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, wav, 44 + data_len, &wrote, 0) && wrote == 44 + data_len;
    if (f != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(f);
        CloseHandle(f);
    }
    ok = ok && MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) {
        ERR("%s: could not write %ls (error %lu)", seg->name, path, GetLastError());
        DeleteFileW(part);
        swprintf(path, MAX_PATH, L"%s.json", stem);
        DeleteFileW(path);
    }
    rewind(m);
}

static void seg_close(Segmenter* seg) {
    if (!seg->open) return;
    seg->open = false;
    if (seg->loud_blocks < MIN_LOUD_BLOCKS) return;
    // Keep at most 300 ms of the trailing quiet.
    i32 trim = seg->quiet_run > SOFT_QUIET ? (seg->quiet_run - SOFT_QUIET) * BLOCK : 0;
    seg->n -= trim;
    seg_write(seg, seg->start_ts + (f64)seg->n / RATE);
}

static void seg_block(Segmenter* seg, const f32* block, f64 block_ts, StreamMeta* meta) {
    f32 sum = 0;
    for (i32 i = 0; i < BLOCK; i++) sum += block[i] * block[i];
    bool loud = sqrtf(sum / BLOCK) > seg->gate;
    if (loud) seg->loud_ts = block_ts + (f64)BLOCK / RATE;
    if (!seg->open) {
        if (!loud) {
            i32 at = (seg->preroll_first + seg->preroll_n) % PREROLL_BLOCKS;
            if (seg->preroll_n == PREROLL_BLOCKS) seg->preroll_first = (seg->preroll_first + 1) % PREROLL_BLOCKS;
            else seg->preroll_n++;
            memcpy(seg->preroll[at], block, sizeof(f32) * BLOCK);
            return;
        }
        seg->n = 0;
        for (i32 i = 0; i < seg->preroll_n; i++) {
            memcpy(seg->samples + seg->n, seg->preroll[(seg->preroll_first + i) % PREROLL_BLOCKS], sizeof(f32) * BLOCK);
            seg->n += BLOCK;
        }
        memcpy(seg->samples + seg->n, block, sizeof(f32) * BLOCK);
        seg->n += BLOCK;
        seg->open = true;
        seg->start_ts = block_ts - (f64)(seg->preroll_n * BLOCK) / RATE;
        seg->quiet_run = 0;
        seg->loud_blocks = 1;
        seg->meta = *meta;
        seg->preroll_n = seg->preroll_first = 0;
        return;
    }
    memcpy(seg->samples + seg->n, block, sizeof(f32) * BLOCK);
    seg->n += BLOCK;
    if (loud) {
        seg->quiet_run = 0;
        seg->loud_blocks++;
    } else {
        seg->quiet_run++;
    }
    if (seg->quiet_run >= CLOSE_AFTER_QUIET || (seg->n >= SOFT_LIMIT && seg->quiet_run >= SOFT_QUIET) || seg->n >= HARD_LIMIT)
        seg_close(seg);
}

// Samples that end "now". `meta` is copied when a segment opens.
static void seg_push(Segmenter* seg, const f32* samples, i32 count, StreamMeta* meta) {
    i32 blocks = (seg->pending_n + count) / BLOCK;
    if (!blocks) {
        memcpy(seg->pending + seg->pending_n, samples, sizeof(f32) * count);
        seg->pending_n += count;
        return;
    }
    f64 now = now_ts();
    f32 block[BLOCK];
    i32 have = seg->pending_n, at = 0;
    memcpy(block, seg->pending, sizeof(f32) * have);
    for (i32 b = 0; b < blocks; b++) {
        memcpy(block + have, samples + at, sizeof(f32) * (BLOCK - have));
        at += BLOCK - have;
        have = 0;
        // Wall-clock time of the start of this block.
        seg_block(seg, block, now - (f64)((blocks - b) * BLOCK) / RATE, meta);
    }
    seg->pending_n = count - at;
    memcpy(seg->pending, samples + at, sizeof(f32) * seg->pending_n);
}

// The source sent nothing for `secs` (a silent app sends no packets at all).
static void seg_silence(Segmenter* seg, f64 secs, StreamMeta* meta) {
    if (!seg->open) {
        seg->preroll_n = seg->pending_n = 0;
        return;
    }
    static const f32 zeros[1600] = {};
    for (i32 left = (i32)(secs * RATE); left > 0; left -= COUNT(zeros))
        seg_push(seg, zeros, left < COUNT(zeros) ? left : COUNT(zeros), meta);
}

// Throws away everything not yet written (private mode, or the app was just excluded).
static void seg_discard(Segmenter* seg) {
    seg->open = false;
    seg->preroll_n = seg->pending_n = 0;
}

// ---------------------------------------------------------------------------------------------
// 48 kHz to 16 kHz: a windowed-sinc low-pass at ~7 kHz, then every third sample.

static const i32 TAPS = 63;

struct Downsampler {
    f32 taps[TAPS];
    f32 history[TAPS * 2];  // each sample twice, so the window is one straight run
    i32 pos;
    i32 phase;
};

static void down_init(Downsampler* d) {
    *d = {};
    const f32 PI = 3.14159265358979f;
    f32 cutoff = 7000.0f / IN_RATE, mid = (TAPS - 1) / 2.0f, sum = 0;
    for (i32 i = 0; i < TAPS; i++) {
        f32 x = i - mid;
        f32 sinc = x == 0 ? 2 * cutoff : sinf(2 * PI * cutoff * x) / (PI * x);
        f32 hann = 0.5f - 0.5f * cosf(2 * PI * i / (TAPS - 1));
        d->taps[i] = sinc * hann;
        sum += d->taps[i];
    }
    for (i32 i = 0; i < TAPS; i++) d->taps[i] /= sum;
}

static void down_push(Downsampler* d, f32 sample, List<f32>* out) {
    d->pos = (d->pos + 1) % TAPS;
    d->history[d->pos] = d->history[d->pos + TAPS] = sample;
    if (++d->phase < 3) return;
    d->phase = 0;
    const f32* h = d->history + d->pos + 1;  // oldest first
    f32 sum = 0;
    for (i32 i = 0; i < TAPS; i++) sum += h[i] * d->taps[i];
    list_push(out, sum);
}

// ---------------------------------------------------------------------------------------------
// WASAPI

// PKEY_Device_FriendlyName, spelt out so no GUID library is needed.
static const PROPERTYKEY FRIENDLY_NAME = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static IMMDeviceEnumerator* device_enumerator() {
    IMMDeviceEnumerator* en = 0;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr)) WARN("capture: no audio device enumerator (0x%08lx)", (unsigned long)hr);
    return en;
}

// The id of the Windows default mic (what gets recorded), into `out`; "" if there is none.
static void default_mic_id(IMMDeviceEnumerator* en, wchar_t* out, i32 cap) {
    out[0] = 0;
    IMMDevice* dev = 0;
    if (FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))) return;
    LPWSTR id = 0;
    if (SUCCEEDED(dev->GetId(&id))) {
        wcsncpy_s(out, cap, id, _TRUNCATE);
        CoTaskMemFree(id);
    }
    dev->Release();
}

// Process loopback is opened asynchronously, and Windows calls back on a COM object of ours.
// This is the one place a vtable is needed: COM requires it.
struct Activation : IActivateAudioInterfaceCompletionHandler {
    LONG refs;
    HANDLE done;
    HRESULT hr;
    IAudioClient* client;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAgileObject) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = 0;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs);
        if (n == 0) {
            if (client) client->Release();
            CloseHandle(done);
            delete this;
        }
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        IUnknown* got = 0;
        HRESULT activated = E_FAIL;
        hr = op->GetActivateResult(&activated, &got);
        if (SUCCEEDED(hr)) hr = activated;
        if (SUCCEEDED(hr) && got) hr = got->QueryInterface(__uuidof(IAudioClient), (void**)&client);
        if (got) got->Release();
        SetEvent(done);
        return S_OK;
    }
};

// An audio client recording the process tree under `pid`, or null.
static IAudioClient* loopback_client(u32 pid, HRESULT* hr) {
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv = {};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = (BYTE*)&params;

    Activation* act = new Activation();
    act->refs = 1;
    act->done = CreateEventW(0, TRUE, FALSE, 0);
    act->hr = E_PENDING;
    IActivateAudioInterfaceAsyncOperation* op = 0;
    *hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, act, &op);
    IAudioClient* client = 0;
    if (SUCCEEDED(*hr)) {
        if (WaitForSingleObject(act->done, 10000) == WAIT_OBJECT_0) {
            *hr = act->hr;
            client = act->client;
            act->client = 0;
        } else {
            *hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        }
        op->Release();
    }
    act->Release();  // if Windows still holds it, it is freed when Windows lets go
    return client;
}

enum RunEnd {
    RUN_STOP,
    RUN_RESTART,  // the default mic changed: record from the new one
    RUN_ERROR,
};

// The stream's file name: "mic", or the exe without ".exe" and the root pid.
static void stream_name(Stream* s, char out[64]) {
    if (s->source == SOURCE_MIC) {
        snprintf(out, 64, "mic");
        return;
    }
    Str exe = str(s->exe);
    if (str_ends(exe, str(".exe"))) exe.len -= 4;
    char stem[41];
    i32 n = 0;
    for (i64 i = 0; i < exe.len && n < 40; i++) {
        char c = exe.ptr[i];
        stem[n++] = (isalnum((u8)c) && (u8)c < 0x80) || c == '-' || c == '_' ? c : '_';
    }
    stem[n] = 0;
    snprintf(out, 64, "%s-%u", stem, s->pid);
}

static RunEnd stream_run(Stream* s, Segmenter* seg) {
    App* app = s->app;
    IMMDeviceEnumerator* en = 0;
    IMMDevice* dev = 0;
    IAudioClient* client = 0;
    IAudioCaptureClient* capture = 0;
    HANDLE event = 0;
    RunEnd end = RUN_ERROR;
    HRESULT hr = S_OK;
    const char* step = "";
    wchar_t mic_id[512] = {};
    f64 checked = now_ts();
    bool was_paused = false;
    List<f32> out = {};
    Downsampler down;
    down_init(&down);
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = (WORD)CHANNELS;
    format.nSamplesPerSec = IN_RATE;
    format.wBitsPerSample = 32;
    format.nBlockAlign = (WORD)(CHANNELS * 4);
    format.nAvgBytesPerSec = IN_RATE * CHANNELS * 4;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    REFERENCE_TIME buffer = 0;

#define TRY(call, what) \
    do { \
        step = what; \
        hr = (call); \
        if (FAILED(hr)) goto done; \
    } while (0)

    if (s->source == SOURCE_MIC) {
        en = device_enumerator();
        if (!en) {
            step = "the audio devices";
            goto done;
        }
        TRY(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev), "the default mic");
        LPWSTR id = 0;
        if (SUCCEEDED(dev->GetId(&id))) {
            wcsncpy_s(mic_id, id, _TRUNCATE);
            CoTaskMemFree(id);
        }
        IPropertyStore* props = 0;
        char name[128] = {};
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(FRIENDLY_NAME, &v)) && v.vt == VT_LPWSTR) {
                Mark m = mark(scratch());
                SET(name, narrow(scratch(), v.pwszVal));
                rewind(m);
            }
            PropVariantClear(&v);
            props->Release();
        }
        LOG("mic: %s", name[0] ? name : "default device");
        AcquireSRWLockExclusive(&s->lock);
        SET(s->device, str(name));
        ReleaseSRWLockExclusive(&s->lock);
        TRY(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void**)&client), "the mic's audio client");
        buffer = 2000000;  // 200 ms
    } else {
        step = "process loopback";
        client = loopback_client(s->pid, &hr);
        if (!client) goto done;
        flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }
    TRY(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer, 0, &format, 0), "the audio client to start");
    event = CreateEventW(0, FALSE, FALSE, 0);
    TRY(client->SetEventHandle(event), "an event for the audio");
    TRY(client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture), "the capture client");
    TRY(client->Start(), "the stream to start");

    while (!s->stop) {
        // Follow the default mic when it changes (a headset plugged in, or changed in Windows).
        if (en && now_ts() - checked >= 3) {
            checked = now_ts();
            wchar_t now_id[512];
            default_mic_id(en, now_id, COUNT(now_id));
            if (now_id[0] && wcscmp(now_id, mic_id) != 0) {
                LOG("the default mic changed; switching");
                end = RUN_RESTART;
                goto done;
            }
        }
        bool got_event = WaitForSingleObject(event, WAIT_MS) == WAIT_OBJECT_0;
        out.count = 0;
        for (;;) {
            UINT32 packet = 0;
            TRY(capture->GetNextPacketSize(&packet), "the next packet's size");
            if (!packet) break;
            BYTE* data = 0;
            UINT32 frames = 0;
            DWORD bflags = 0;
            TRY(capture->GetBuffer(&data, &frames, &bflags, 0, 0), "a packet");
            const f32* f = (const f32*)data;
            bool silent = (bflags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            for (UINT32 i = 0; i < frames; i++) {
                f32 mono = silent ? 0 : (f[i * 2] + f[i * 2 + 1]) / CHANNELS;
                down_push(&down, mono, &out);
            }
            TRY(capture->ReleaseBuffer(frames), "a packet to be released");
        }

        bool paused = atomic_get(&app->capture.paused) != 0;
        StreamMeta meta = {};
        meta.source = s->source;
        SET(meta.exe, str(s->exe));
        meta.pid = s->pid;
        AcquireSRWLockExclusive(&s->lock);
        SET(meta.title, str(s->title));
        SET(meta.device, str(s->device));
        s->loud_ts = seg->loud_ts;
        ReleaseSRWLockExclusive(&s->lock);

        if (paused) {
            if (!was_paused) seg_discard(seg);
            was_paused = true;
            continue;
        }
        was_paused = false;
        if (!out.count) {
            if (!got_event) seg_silence(seg, WAIT_MS / 1000.0, &meta);
            continue;
        }
        seg_push(seg, out.items, out.count, &meta);
    }
    end = RUN_STOP;
#undef TRY

done:
    if (end == RUN_ERROR) WARN("%s: capture stopped at %s (0x%08lx); trying again in 5 s", seg->name, step, (unsigned long)hr);
    if (client) client->Stop();
    if (capture) capture->Release();
    if (client) client->Release();
    if (event) CloseHandle(event);
    if (dev) dev->Release();
    if (en) en->Release();
    list_free(&out);
    return end;
}

static DWORD WINAPI stream_main(void* p) {
    Stream* s = (Stream*)p;
    CoInitializeEx(0, COINIT_MULTITHREADED);
    Segmenter* seg = (Segmenter*)calloc(1, sizeof(Segmenter));
    CHECK(seg, "memory for a segmenter");
    seg->samples = (f32*)malloc(sizeof(f32) * HARD_LIMIT);
    CHECK(seg->samples, "memory for %d samples", HARD_LIMIT);
    stream_name(s, seg->name);
    wcscpy_s(seg->spool, s->spool);
    seg->gate = s->gate;

    while (!s->stop) {
        RunEnd end = stream_run(s, seg);
        if (end == RUN_STOP) break;
        seg_close(seg);
        // An app that has exited is ended by the watcher; the mic may have been unplugged, so
        // it keeps trying.
        for (i32 i = 0; end == RUN_ERROR && i < 50 && !s->stop; i++) Sleep(100);
    }
    if (s->discard) seg_discard(seg);
    seg_close(seg);
    LOG("%s: stream closed", seg->name);
    free(seg->samples);
    free(seg);
    CoUninitialize();
    return 0;
}

// Starts `s`'s thread. The main thread, with the lock.
static void stream_start(App* app, Stream* s) {
    s->app = app;
    s->lock = SRWLOCK_INIT;
    spool_dir(&app->config, s->spool);
    s->gate = app->config.capture.gate_rms;
    s->started_ts = s->playing_ts = now_ts();
    s->thread = CreateThread(0, 0, stream_main, s, 0, 0);
    CHECK(s->thread, "a capture thread to start (error %lu)", GetLastError());
    char name[64], label[80];
    stream_name(s, name);
    snprintf(label, sizeof(label), "capture-%s", name);
    Mark m = mark(scratch());
    SetThreadDescription(s->thread, wide(scratch(), str(label)));
    rewind(m);
}

// Tells the stream at `i` to stop and moves it to `ending`. With the lock.
static void stream_end(App* app, i32 i, bool discard) {
    Stream* s = app->capture.streams[i];
    if (discard) InterlockedExchange(&s->discard, 1);
    InterlockedExchange(&s->stop, 1);
    list_remove(&app->capture.streams, i);
    list_push(&app->capture.ending, s);
}

// Frees the streams whose threads have ended. With the lock.
static void streams_reap(App* app) {
    List<Stream*>* l = &app->capture.ending;
    for (i32 i = 0; i < l->count; i++) {
        Stream* s = l->items[i];
        if (WaitForSingleObject(s->thread, 0) != WAIT_OBJECT_0) continue;
        CloseHandle(s->thread);
        free(s);
        list_remove(l, i--);
    }
}

// ---------------------------------------------------------------------------------------------
// The watcher

struct Pids {
    u32* items;
    i32 count;
};

// Processes with a sound session playing now, on any output device.
static Pids playing_pids(IMMDeviceEnumerator* en, Arena* a) {
    List<u32> list = {};
    IMMDeviceCollection* devices = 0;
    if (en && SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        UINT n = 0;
        devices->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice* dev = 0;
            IAudioSessionManager2* mgr = 0;
            IAudioSessionEnumerator* sessions = 0;
            if (SUCCEEDED(devices->Item(i, &dev)) &&
                SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, 0, (void**)&mgr)) &&
                SUCCEEDED(mgr->GetSessionEnumerator(&sessions))) {
                int count = 0;
                sessions->GetCount(&count);
                for (int j = 0; j < count; j++) {
                    IAudioSessionControl* ctl = 0;
                    IAudioSessionControl2* ctl2 = 0;
                    AudioSessionState state = AudioSessionStateInactive;
                    DWORD pid = 0;
                    if (SUCCEEDED(sessions->GetSession(j, &ctl)) && SUCCEEDED(ctl->GetState(&state)) &&
                        state == AudioSessionStateActive &&
                        SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctl2)) &&
                        SUCCEEDED(ctl2->GetProcessId(&pid))) {
                        bool have = false;
                        for (i32 k = 0; k < list.count && !have; k++) have = list.items[k] == pid;
                        if (!have) list_push(&list, (u32)pid);
                    }
                    if (ctl2) ctl2->Release();
                    if (ctl) ctl->Release();
                }
            }
            if (sessions) sessions->Release();
            if (mgr) mgr->Release();
            if (dev) dev->Release();
        }
        devices->Release();
    }
    Pids out = {PUSH_ARRAY(a, u32, list.count), list.count};
    if (list.count) memcpy(out.items, list.items, sizeof(u32) * list.count);
    list_free(&list);
    return out;
}

// What the watcher saw this second, for the main thread to act on.
struct WatchFacts {
    Procs procs;
    Wins wins;
    Pids playing;   // with a sound session playing; not 0 ("system sounds") or lifelog
    char (*paths)[MAX_PATH];  // each playing pid's exe path, "" when it could not be read
    f64 now;
};

// The main thread, with the lock: starts and stops app streams from what the watcher saw.
static void watch_apply(App* app, Job* job) {
    if (stopping(app)) return;  // capture_stop has ended the streams
    WatchFacts* f = (WatchFacts*)job->data;
    Capture* c = &app->capture;
    bool changed = false;
    bool record_apps = app->config.capture.apps;
    for (i32 i = 0; record_apps && i < f->playing.count; i++) {
        u32 pid = f->playing.items[i];
        Proc* proc = proc_find(&f->procs, pid);
        if (!proc) continue;
        Str exe = str(proc->exe);
        AppInfo* info = app_get(app, exe);
        // Its folder may decide: the path is needed before the first sound is kept. Read every
        // time: the same exe may now run from another folder (and so fall under another folder
        // rule, or carry another name).
        if (f->paths[i][0]) app_learn_path(app, exe, str(f->paths[i]));
        if (app_decision(app, info) == DECISION_EXCLUDED) continue;
        u32 root = proc_root(&f->procs, pid);
        Stream* found = 0;
        for (i32 k = 0; k < c->streams.count && !found; k++)
            if (c->streams[k]->source == SOURCE_APP && c->streams[k]->pid == root) found = c->streams[k];
        if (found) {
            found->playing_ts = f->now;
            continue;
        }
        Stream* s = (Stream*)calloc(1, sizeof(Stream));
        CHECK(s, "memory for a stream");
        s->source = SOURCE_APP;
        SET(s->exe, exe);
        s->pid = root;
        SET(s->title, title_for(exe, &f->procs, &f->wins));
        list_push(&c->streams, s);
        stream_start(app, s);
        // Its exe may have been rebuilt with a new icon or name since it was last read.
        app_get(app, exe)->icon_recheck = true;
        // Removed from the lists earlier: it plays again, so it is new again.
        app_unremove(app, exe);
        LOG("capturing %s (pid %u): %s", s->exe, root, s->title[0] ? s->title : "no window");
        changed = true;
    }
    for (i32 i = 0; i < c->streams.count; i++) {
        Stream* s = c->streams[i];
        if (s->source != SOURCE_APP) continue;
        Str exe = str(s->exe);
        AppInfo* info = app_find(app, exe);
        if ((info && app_decision(app, info) == DECISION_EXCLUDED) || !record_apps) {
            // Just excluded: stop now and drop the sentence in progress.
            LOG("%s (pid %u): no longer recorded", s->exe, s->pid);
            stream_end(app, i--, true);
            changed = true;
            continue;
        }
        // A reused pid would be a different program.
        Proc* proc = proc_find(&f->procs, s->pid);
        bool alive = proc && str_eq(str(proc->exe), exe);
        if (!alive || f->now - s->playing_ts >= LINGER_SECS) {
            LOG("%s (pid %u) went quiet", s->exe, s->pid);
            stream_end(app, i--, false);
            changed = true;
            continue;
        }
        Str title = title_for(exe, &f->procs, &f->wins);
        AcquireSRWLockExclusive(&s->lock);
        SET(s->title, title);
        ReleaseSRWLockExclusive(&s->lock);
    }
    // An app is "playing" while it has a stream.
    for (i32 i = 0; i < app->apps.list.count; i++) {
        AppInfo* a = &app->apps.list[i];
        bool on = false;
        for (i32 k = 0; k < c->streams.count && !on; k++) on = c->streams[k]->source == SOURCE_APP && strcmp(c->streams[k]->exe, a->exe) == 0;
        changed |= on != a->playing;
        a->playing = on;
    }
    streams_reap(app);
    if (changed) wake_window(app);
}

// Once a second: what runs, what has windows and what plays sound, gathered without the lock
// and posted to the main thread.
static DWORD WINAPI capture_watch_main(void* p) {
    App* app = (App*)p;
    CoInitializeEx(0, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = device_enumerator();
    u32 own_pid = GetCurrentProcessId();

    do {
        Job* job = job_new(watch_apply);
        Arena* a = &job->arena;
        WatchFacts* f = PUSH(a, WatchFacts);
        f->procs = processes(a);
        f->wins = windows_now(a);
        Pids all = playing_pids(en, a);
        f->playing.items = PUSH_ARRAY(a, u32, all.count);
        f->paths = (char(*)[MAX_PATH])arena_push(a, (u64)MAX_PATH * (all.count ? all.count : 1), 1);
        for (i32 i = 0; i < all.count; i++) {
            u32 pid = all.items[i];
            if (pid == 0 || pid == own_pid) continue;
            Proc* proc = proc_find(&f->procs, pid);
            // lifelog's own window plays recordings back: never record those again.
            if (!proc || is_own_exe(str(proc->exe))) continue;
            i32 n = f->playing.count++;
            f->playing.items[n] = pid;
            if (!proc_path(pid, f->paths[n], MAX_PATH)) f->paths[n][0] = 0;
        }
        f->now = now_ts();
        job->data = f;
        job_post(app, job);
    } while (sleep_unless_stopping(app, 1000));

    if (en) en->Release();
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------------------------

void capture_start(App* app) {
    if (app->config.capture.mic) {
        Stream* s = (Stream*)calloc(1, sizeof(Stream));
        CHECK(s, "memory for a stream");
        s->source = SOURCE_MIC;
        list_push(&app->capture.streams, s);
        stream_start(app, s);
    }
    app->capture.watcher = CreateThread(0, 0, capture_watch_main, app, 0, 0);
    CHECK(app->capture.watcher, "the capture watcher to start (error %lu)", GetLastError());
    SetThreadDescription(app->capture.watcher, L"capture-watch");
}

void capture_set_paused(App* app, bool paused) {
    if ((atomic_get(&app->capture.paused) != 0) == paused) return;
    atomic_set(&app->capture.paused, paused);
    LOG("%s", paused ? "private mode: recording nothing" : "recording again");
    wake_window(app);
}

void capture_stop(App* app) {
    CHECK(stopping(app), "capture_stop only once the program is ending (App.stop set)");
    Capture* c = &app->capture;
    while (c->streams.count) stream_end(app, 0, false);
    HANDLE watcher = c->watcher;
    c->watcher = 0;
    // Let go meanwhile, so the other threads are not held up for the seconds this can take.
    app_unlock(app);
    if (watcher) {
        WaitForSingleObject(watcher, 5000);
        CloseHandle(watcher);
    }
    DWORD until = GetTickCount() + 10000;
    for (i32 i = 0; i < c->ending.count; i++) {
        DWORD left = until - GetTickCount();
        WaitForSingleObject(c->ending.items[i]->thread, (i32)left > 0 ? left : 0);
    }
    app_lock(app);
    streams_reap(app);
    if (c->ending.count) WARN("capture: %d streams did not finish in time", c->ending.count);
}

// voice.cpp: see voice.h.

static const i32 VOICE_RATE = 16000;
static const i32 VOICE_SHORTEST = VOICE_RATE / 4;  // under 250 ms is not a command
static const i32 VOICE_LONGEST = 12 * VOICE_RATE;  // of a longer sentence, only its last 12 s
static const f64 VOICE_WAIT_SECS = 6;              // after the name alone

void voice_heard(App* app, const f32* samples, i32 n, f32 gain) {
    Voice* v = &app->voice;
    if (!v->wake || !atomic_get(&v->listen) || atomic_get(&v->state) != VOICE_READY) return;
    if (n < VOICE_SHORTEST) return;
    if (n > VOICE_LONGEST) {
        samples += n - VOICE_LONGEST;
        n = VOICE_LONGEST;
    }
    f32* copy = (f32*)malloc(sizeof(f32) * n);
    if (!copy) return;
    for (i32 i = 0; i < n; i++) {
        f32 s = samples[i] * gain;
        copy[i] = s < -1 ? -1 : s > 1 ? 1 : s;
    }
    AcquireSRWLockExclusive(&v->lock);
    if (v->count == COUNT(v->queue)) {  // behind: the oldest goes
        free(v->queue[v->head].samples);
        v->head = (v->head + 1) % COUNT(v->queue);
        v->count--;
    }
    v->queue[(v->head + v->count) % COUNT(v->queue)] = {copy, n};
    v->count++;
    ReleaseSRWLockExclusive(&v->lock);
    SetEvent(v->wake);
}

static bool voice_take(Voice* v, VoiceSentence* out) {
    AcquireSRWLockExclusive(&v->lock);
    bool got = v->count > 0;
    if (got) {
        *out = v->queue[v->head];
        v->head = (v->head + 1) % COUNT(v->queue);
        v->count--;
    }
    ReleaseSRWLockExclusive(&v->lock);
    return got;
}

static void voice_drop_all(Voice* v) {
    VoiceSentence s;
    while (voice_take(v, &s)) free(s.samples);
}

void voice_set_listen(App* app, bool on) {
    app->config.voice.listen = on;
    config_save_bool("voice", "listen", on);
    atomic_set(&app->voice.listen, on);
    LOG("voice commands %s", on ? "on" : "off");
    if (app->voice.wake) SetEvent(app->voice.wake);
}

static void voice_publish_wake(App* app) {
    Voice* v = &app->voice;
    AcquireSRWLockExclusive(&v->wake_lock);
    SET(v->wake_word, str(app->config.voice.wake));
    SET(v->model, str(app->config.voice.model));
    ReleaseSRWLockExclusive(&v->wake_lock);
}

bool voice_set_model(App* app, Str model) {
    if (!voice_model(model)) return false;
    SET(app->config.voice.model, model);
    config_save_string("voice", "model", app->config.voice.model);
    voice_publish_wake(app);
    LOG("voice commands: listening with %s", app->config.voice.model);
    if (app->voice.wake) SetEvent(app->voice.wake);
    return true;
}

bool voice_set_wake(App* app, Str word) {
    word = str_trim(word);
    if (!word.len) word = str("lifelog");
    i32 letters = 0;
    bool ok = word.len < (i64)sizeof(app->config.voice.wake);
    for (i64 i = 0; i < word.len && ok; i++) {
        u8 c = (u8)word.ptr[i];
        if (isalnum(c)) letters++;
        else ok = c == ' ' || c == '-' || c == '\'';
    }
    if (!ok || letters < 3) {
        ui_say(app, "The wake word needs at least 3 letters, and only letters, digits and spaces");
        return false;
    }
    SET(app->config.voice.wake, word);
    config_save_string("voice", "wake_word", app->config.voice.wake);
    voice_publish_wake(app);
    LOG("voice commands: the wake word is \"%s\"", app->config.voice.wake);
    return true;
}

// ---------------------------------------------------------------------------------------------
// What was said, as words: lower case, letters and digits ("Lifelog, sum up the last 15 minutes." is
// lifelog sum up the last 15 minutes). An apostrophe joins ("today's" is todays).

struct Words {
    Str w[48];
    i32 n;
};

static void words_of(Arena* a, Str text, Words* out) {
    out->n = 0;
    char* buf = PUSH_ARRAY(a, char, text.len + 1);
    i64 len = 0;
    for (i64 i = 0; i <= text.len; i++) {
        char c = i < text.len ? text.ptr[i] : ' ';
        bool keep = isalnum((u8)c) != 0;
        if (keep) {
            buf[len++] = (char)tolower((u8)c);
            continue;
        }
        if (c == '\'' && len) continue;
        if (len && out->n < COUNT(out->w)) out->w[out->n++] = str_copy(a, {buf, len});
        len = 0;
    }
}

static bool word_is(Str w, const char* const* list, i32 count) {
    for (i32 i = 0; i < count; i++)
        if (str_eq(w, str(list[i]))) return true;
    return false;
}
template <typename... T>
static bool word_in(Str w, T... list) {
    const char* const l[] = {list...};
    return word_is(w, l, (i32)sizeof...(list));
}

static bool has(const Words* ws, const char* w) {
    for (i32 i = 0; i < ws->n; i++)
        if (str_eq(ws->w[i], str(w))) return true;
    return false;
}

// Where the word starting with `prefix` is, or -1.
static i32 find_prefix(const Words* ws, const char* prefix) {
    for (i32 i = 0; i < ws->n; i++)
        if (str_starts(ws->w[i], str(prefix))) return i;
    return -1;
}

static bool has_pair(const Words* ws, const char* a, const char* b) {
    for (i32 i = 0; i + 1 < ws->n; i++)
        if (str_eq(ws->w[i], str(a)) && str_eq(ws->w[i + 1], str(b))) return true;
    return false;
}

// Letters and digits only, lower case: the wake word as it is matched ("Hey Jarvis" is heyjarvis).
static void wake_key(const char* word, char out[64]) {
    i32 n = 0;
    for (const char* c = word; *c && n < 63; c++)
        if (isalnum((u8)*c)) out[n++] = (char)tolower((u8)*c);
    out[n] = 0;
}

// Whether `a` and `b` differ by at most one letter added, dropped or changed.
static bool one_off(Str a, Str b) {
    if (a.len < b.len) {
        Str t = a;
        a = b;
        b = t;
    }
    if (a.len - b.len > 1) return false;
    i64 i = 0;
    while (i < b.len && a.ptr[i] == b.ptr[i]) i++;
    if (i == b.len) return true;
    Str ra = str_sub(a, i + 1), rb = str_sub(b, a.len == b.len ? i + 1 : i);
    return str_eq(ra, rb);
}

// How many words the wake word (`key`, from wake_key) takes at `i`, or 0. Whisper may split it
// ("life log") or get one letter wrong ("lifeleg"), so one to three words are joined and a word of
// six letters or more may be one off.
static i32 wake_at(const Words* ws, i32 i, const char* key) {
    Str k = str(key);
    if (!k.len) return 0;
    if (str_eq(k, str("lifelog"))) {  // the ways Whisper writes the default
        Str w = ws->w[i];
        if (word_in(w, "lifelogs", "lifelock", "lifelocks", "livelog", "lifeblog")) return 1;
        if (i + 1 < ws->n && word_in(w, "life", "live", "lie") && word_in(ws->w[i + 1], "log", "logs", "lock", "locks", "blog"))
            return 2;
    }
    char joined[128];
    i32 len = 0;
    for (i32 n = 1; n <= 3 && i + n - 1 < ws->n; n++) {
        Str w = ws->w[i + n - 1];
        if (len + w.len >= (i64)sizeof(joined)) break;
        memcpy(joined + len, w.ptr, w.len);
        len += (i32)w.len;
        Str got = {joined, len};
        if (str_eq(got, k) || (k.len >= 6 && one_off(got, k))) return n;
        if (len > k.len + 1) break;
    }
    return 0;
}

// The words after `from` and up to `to`, with the polite and filler ones dropped.
static void words_between(const Words* ws, i32 from, i32 to, Words* out) {
    out->n = 0;
    for (i32 i = from; i < to; i++) {
        Str w = ws->w[i];
        if (word_in(w, "please", "can", "could", "would", "you", "will", "hey", "ok", "okay", "um", "uh", "just", "now")) continue;
        out->w[out->n++] = w;
    }
}

// ---------------------------------------------------------------------------------------------
// Commands

enum VoiceDo : i32 {
    DO_NOTHING,    // not a command
    DO_API,        // through the API: `method`, `path`, `query`
    DO_CANCEL,
    DO_CHECK_AI,
    DO_SUMMARISE,  // a few sentences about the last `query` ("30m"), from the summary AI
    DO_REMIND,     // a reminder saying `text` at `due` (0: no time was said)
    DO_CUSTOM,     // a custom command's script: `custom` (commands.h)
};

struct VoiceCommand {
    VoiceDo what;
    const char* method;
    char path[32];
    char query[256];
    f64 due;          // DO_REMIND: unix seconds
    char text[128];   // DO_REMIND: what it is about
    Chime chime;      // when it worked
    bool private_ok;  // carried out in private mode too
    char said[128];   // what it does, for the log and Settings: "summarise the last hour"
    char shown[128];  // what the banner says when it worked: "Alarm set: 4 minutes"
    CmdMatch custom;  // DO_CUSTOM
};

static void command(VoiceCommand* c, const char* method, const char* path, const char* said) {
    c->what = DO_API;
    c->method = method;
    snprintf(c->path, sizeof(c->path), "%s", path);
    snprintf(c->said, sizeof(c->said), "%s", said);
    c->chime = CHIME_DONE;
}

// A number said as words or digits, at `*i` (moved past it): "fifteen", "twenty five", "15", "an".
// 0 if there is none.
static i32 number_at(const Words* ws, i32* i) {
    static const char* ones[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                 "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
    static const char* tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
    if (*i >= ws->n) return 0;
    Str w = ws->w[*i];
    if (isdigit((u8)w.ptr[0])) {
        i32 n = 0;
        for (i64 k = 0; k < w.len && isdigit((u8)w.ptr[k]); k++) n = n * 10 + (w.ptr[k] - '0');
        // "15m" or "2h", as Whisper sometimes writes it, leaves the unit for unit_at.
        (*i)++;
        return n > 999 ? 999 : n;
    }
    if (word_in(w, "a", "an")) {
        (*i)++;
        return 1;
    }
    if (word_in(w, "couple")) {
        (*i)++;
        if (*i < ws->n && str_eq(ws->w[*i], str("of"))) (*i)++;
        return 2;
    }
    if (word_in(w, "few")) {
        (*i)++;
        return 3;
    }
    for (i32 k = 1; k < COUNT(ones); k++)
        if (str_eq(w, str(ones[k]))) {
            (*i)++;
            return k;
        }
    for (i32 k = 2; k < COUNT(tens); k++)
        if (str_eq(w, str(tens[k]))) {
            (*i)++;
            i32 n = k * 10;
            if (*i < ws->n)
                for (i32 j = 1; j < 10; j++)
                    if (str_eq(ws->w[*i], str(ones[j]))) {
                        (*i)++;
                        n += j;
                        break;
                    }
            return n;
        }
    return 0;
}

// The unit at `i`: 'm', 'h' or 'd', or 0.
static char unit_of(Str w) {
    if (word_in(w, "minute", "minutes", "min", "mins")) return 'm';
    if (word_in(w, "hour", "hours", "hr", "hrs")) return 'h';
    if (word_in(w, "day", "days")) return 'd';
    return 0;
}

// The stretch said for a summary, as clip_range takes it: "15m", "2h", "today". Default 15m.
static void clip_range_said(const Words* ws, char* out, i32 cap) {
    snprintf(out, cap, "15m");
    if (has(ws, "today") || has(ws, "todays")) {
        snprintf(out, cap, "today");
        return;
    }
    if (has_pair(ws, "half", "an") || has_pair(ws, "half", "hour")) {
        snprintf(out, cap, "30m");
        return;
    }
    for (i32 i = 0; i < ws->n; i++) {
        // "15m" and "2h" in one word.
        Str w = ws->w[i];
        if (w.len >= 2 && isdigit((u8)w.ptr[0]) && strchr("mhd", w.ptr[w.len - 1])) {
            bool digits = true;
            for (i64 k = 0; k < w.len - 1; k++) digits &= isdigit((u8)w.ptr[k]) != 0;
            if (digits) {
                snprintf(out, cap, "%.*s", (int)w.len, w.ptr);
                return;
            }
        }
        char unit = unit_of(w);
        if (!unit) continue;
        i32 n = 0;
        // The number before the unit: one word ("fifteen", "15") or two ("twenty five").
        for (i32 back = 2; back >= 1 && !n; back--) {
            i32 from = i - back;
            if (from < 0) continue;
            i32 j = from;
            i32 got = number_at(ws, &j);
            if (got && j == i) n = got;
        }
        if (!n) n = 1;  // "the last hour"
        bool and_a_half = i + 3 < ws->n && str_eq(ws->w[i + 1], str("and")) && str_eq(ws->w[i + 2], str("a")) &&
                          str_eq(ws->w[i + 3], str("half"));
        if (and_a_half && unit == 'h') snprintf(out, cap, "%dm", n * 60 + 30);
        else snprintf(out, cap, "%d%c", n, unit);
        return;
    }
}

// The app named in `ws` from word `from` on: "spotify", "visual studio code".
static void app_said(const Words* ws, i32 from, i32 to, char* out, i32 cap) {
    out[0] = 0;
    i32 len = 0;
    for (i32 i = from; i < to && i < ws->n; i++) {
        Str w = ws->w[i];
        if (word_in(w, "the", "app", "application", "program", "game", "as", "a", "mark", "from", "to", "recording", "list", "not",
                    "no", "isnt", "is", "its", "an")) continue;
        len += snprintf(out + len, cap - len, "%s%.*s", len ? " " : "", (int)w.len, w.ptr);
        if (len >= cap) break;
    }
}

// "15m" as words, for "the last ...": "15 minutes", "hour", "today".
static void range_words(const char* range, char* out, i32 cap) {
    i32 n = atoi(range);
    char unit = range[0] ? range[strlen(range) - 1] : 0;
    if (!n || !strchr("mhd", unit)) {
        snprintf(out, cap, "%s", range);
        return;
    }
    const char* name = unit == 'm' ? "minute" : unit == 'h' ? "hour" : "day";
    if (n == 1) snprintf(out, cap, "%s", name);
    else snprintf(out, cap, "%d %ss", n, name);
}

// A number said as a word or digits at `*i` (moved past it), but not "a" or "an": for a clock time.
static i32 clock_number_at(const Words* ws, i32* i) {
    if (*i >= ws->n || word_in(ws->w[*i], "a", "an", "couple", "few")) return 0;
    return number_at(ws, i);
}

// A reminder or alarm: "remind me in 10 minutes to check the oven", "remind me to call Sam at 5
// pm", "set an alarm for 7:30", "set a timer for 20 minutes", "wake me up tomorrow at 8". A time
// without am or pm is the next one: "at 5" is 05:00 or 17:00, whichever comes first, and a time
// that has passed is tomorrow's.
static void reminder_said(const Words* ws, f64 now, VoiceCommand* c) {
    c->what = DO_REMIND;
    bool used[COUNT(ws->w)] = {};
    f64 after = 0;
    i32 hour = -1, minute = 0;
    char half = 0;
    bool tomorrow = false;
    for (i32 i = 0; i < ws->n; i++) {
        Str w = ws->w[i];
        if (word_in(w, "tomorrow", "tomorrows")) {
            tomorrow = used[i] = true;
            continue;
        }
        bool pm = word_in(w, "tonight", "evening", "afternoon"), am = word_in(w, "morning");
        if (pm || am) {
            half = pm ? 'p' : 'a';
            used[i] = true;
            for (i32 k = i - 1; k >= 0 && word_in(ws->w[k], "in", "the", "this"); k--) used[k] = true;
            continue;
        }
        bool in = word_in(w, "in", "for", "after");
        bool at = word_in(w, "at", "for", "by", "until", "till", "til");
        if (!in && !at) continue;
        i32 j = i + 1;
        if (j < ws->n && word_in(ws->w[j], "about", "around")) j++;
        // A length: "in 10 minutes", "for an hour and a half", "in half an hour".
        if (in && after == 0) {
            i32 k = j;
            if (k + 2 < ws->n && str_eq(ws->w[k], str("half")) && word_in(ws->w[k + 1], "an", "a") && unit_of(ws->w[k + 2]) == 'h') {
                after = 1800;
                for (i32 x = i; x <= k + 2; x++) used[x] = true;
                i = k + 2;
                continue;
            }
            i32 n = number_at(ws, &k);
            char u = k < ws->n ? unit_of(ws->w[k]) : 0;
            if (n && u) {
                f64 unit = u == 'm' ? 60 : u == 'h' ? 3600 : 86400;
                after = n * unit;
                k++;
                if (k + 2 < ws->n && str_eq(ws->w[k], str("and")) && str_eq(ws->w[k + 1], str("a")) && str_eq(ws->w[k + 2], str("half"))) {
                    after += unit / 2;
                    k += 3;
                }
                for (i32 x = i; x < k; x++) used[x] = true;
                i = k - 1;
                continue;
            }
        }
        // A clock time: "at 5", "at 7 30 pm", "for 5pm", "at seven thirty", "at noon".
        if (at && hour < 0 && j < ws->n) {
            i32 k = j;
            Str t = ws->w[k];
            i32 h = -1, m = 0;
            if (word_in(t, "noon", "midday")) {
                h = 12;
                half = 'p';
                k++;
            } else if (word_in(t, "midnight")) {
                h = 0;
                half = 'a';
                k++;
            } else {
                i32 n = clock_number_at(ws, &k);
                if (!n) continue;
                if (n >= 100) {
                    h = n / 100;
                    m = n % 100;
                } else {
                    h = n;
                }
                if (str_ends(t, str("pm"))) half = 'p';
                else if (str_ends(t, str("am"))) half = 'a';
                if (n < 100 && !half && k < ws->n) {
                    i32 k2 = k;
                    if (word_in(ws->w[k2], "oh", "o")) k2++;
                    i32 got = clock_number_at(ws, &k2);
                    if (got > 0 && got < 60 && !(k2 < ws->n && unit_of(ws->w[k2]))) {
                        m = got;
                        k = k2;
                    }
                }
            }
            if (k < ws->n && word_in(ws->w[k], "oclock")) k++;
            if (k < ws->n && word_in(ws->w[k], "am", "pm")) half = ws->w[k++].ptr[0];
            else if (k + 1 < ws->n && word_in(ws->w[k], "a", "p") && str_eq(ws->w[k + 1], str("m"))) {
                half = ws->w[k].ptr[0];
                k += 2;
            }
            if (h > 23 || m > 59) continue;
            hour = h;
            minute = m;
            for (i32 x = i; x < k; x++) used[x] = true;
            i = k - 1;
        }
    }

    LocalTime t = local_time(now);
    i32 day = t.day + (tomorrow ? 1 : 0);
    if (after > 0) {
        c->due = now + after;
    } else if (hour >= 0) {
        if (half == 'p' && hour < 12) hour += 12;
        if (half == 'a' && hour == 12) hour = 0;
        c->due = local_ts(t.year, t.month, day, hour, minute);
        if (!half && hour >= 1 && hour < 12 && c->due <= now) c->due = local_ts(t.year, t.month, day, hour + 12, minute);
        if (c->due <= now) c->due = local_ts(t.year, t.month, day + 1, hour, minute);
    } else if (tomorrow) {
        c->due = local_ts(t.year, t.month, day, 9, 0);
    }

    // What it is about: the words after "to", "that" or "about", less the time.
    i32 from = -1;
    for (i32 i = 1; i < ws->n && from < 0; i++)
        if (!used[i] && word_in(ws->w[i], "to", "that", "about")) from = i + 1;
    i32 len = 0;
    for (i32 i = from; from >= 0 && i < ws->n; i++) {
        if (used[i]) continue;
        len += snprintf(c->text + len, sizeof(c->text) - len, "%s%.*s", len ? " " : "", (int)ws->w[i].len, ws->w[i].ptr);
        if (len >= (i32)sizeof(c->text)) break;
    }
    if (!c->text[0]) snprintf(c->text, sizeof(c->text), "%s", has(ws, "timer") ? "Timer" : has(ws, "alarm") || has(ws, "wake") ? "Alarm" : "Reminder");
    c->text[0] = (char)toupper((u8)c->text[0]);

    if (!c->due) {
        snprintf(c->said, sizeof(c->said), "reminder: %s (no time said)", c->text);
        return;
    }
    // The banner: "Timer set: 20 minutes", "Reminder set: 17:00 · Call Sam".
    const char* kind = has(ws, "timer") ? "Timer" : has(ws, "alarm") || has(ws, "wake") ? "Alarm" : "Reminder";
    char when[48];
    if (after > 0) {
        i32 mins = (i32)(after / 60), days = mins / 1440, hours = mins % 1440 / 60;
        mins %= 60;
        i32 wl = 0;
        if (days) wl += snprintf(when + wl, sizeof(when) - wl, "%d day%s ", days, days == 1 ? "" : "s");
        if (hours) wl += snprintf(when + wl, sizeof(when) - wl, "%d hour%s ", hours, hours == 1 ? "" : "s");
        if (mins) wl += snprintf(when + wl, sizeof(when) - wl, "%d minute%s ", mins, mins == 1 ? "" : "s");
        if (wl) when[wl - 1] = 0;
    }
    LocalTime d = local_time(c->due);
    char when_day[16] = "";
    if (d.day != t.day) snprintf(when_day, sizeof(when_day), "%s ", d.day == local_time(now + 86400).day ? "tomorrow" : "on that day");
    snprintf(c->said, sizeof(c->said), "reminder %sat %02d:%02d: %s", when_day, d.hour, d.minute, c->text);
    if (after <= 0) snprintf(when, sizeof(when), "%s%02d:%02d", d.day != t.day ? (d.day == local_time(now + 86400).day ? "tomorrow " : "") : "", d.hour, d.minute);
    bool own_text = strcmp(c->text, "Timer") && strcmp(c->text, "Alarm") && strcmp(c->text, "Reminder");
    snprintf(c->shown, sizeof(c->shown), "%s set: %s%s%s", kind, when, own_text ? " · " : "", own_text ? c->text : "");
}

// What the words after the wake name ask for, of the built-in commands.
static void parse_builtin(const Words* ws, VoiceCommand* c) {
    *c = {};
    if (!ws->n) return;
    Str first = ws->w[0];
    Arena* a = scratch();

    if (word_in(first, "cancel", "nevermind", "nothing", "stop") && ws->n == 1) {
        c->what = DO_CANCEL;
        c->private_ok = true;
        return;
    }
    if (has_pair(ws, "never", "mind")) {
        c->what = DO_CANCEL;
        c->private_ok = true;
        return;
    }
    if (word_in(first, "remind", "wake") || has(ws, "reminder") || has(ws, "alarm") || has(ws, "timer")) {
        reminder_said(ws, now_ts(), c);
        return;
    }
    if ((has(ws, "check") || has(ws, "test")) && (has(ws, "ai") || has_pair(ws, "a", "i"))) {
        c->what = DO_CHECK_AI;
        snprintf(c->said, sizeof(c->said), "check the AI");
        return;
    }
    // "pause transcription" / "continue transcription" (back to auto or always, as it was).
    if (find_prefix(ws, "transcri") >= 0) {
        const char* mode = 0;
        if (has(ws, "always")) mode = "always";
        else if (has(ws, "auto") || has(ws, "automatic")) mode = "auto";
        else if (has(ws, "pause") || has(ws, "paused") || has(ws, "off") || has(ws, "stop") || has(ws, "hold")) mode = "paused";
        else if (has(ws, "continue") || has(ws, "resume") || has(ws, "start") || has(ws, "on") || has(ws, "back") || has(ws, "unpause")) mode = "continue";
        else return;
        command(c, "POST", "/transcription", "");
        snprintf(c->query, sizeof(c->query), "mode=%s", mode);
        if (strcmp(mode, "paused") == 0) snprintf(c->said, sizeof(c->said), "pause transcription");
        else if (strcmp(mode, "continue") == 0) snprintf(c->said, sizeof(c->said), "continue transcription");
        else snprintf(c->said, sizeof(c->said), "transcription %s", mode);
        if (strcmp(mode, "paused") == 0) snprintf(c->shown, sizeof(c->shown), "Transcription paused");
        else if (strcmp(mode, "continue") == 0) snprintf(c->shown, sizeof(c->shown), "Transcription back on");
        else snprintf(c->shown, sizeof(c->shown), "Transcription: %s", mode);
        return;
    }
    // "summarise the last half hour", "sum up the last 10 minutes", "recap".
    if (find_prefix(ws, "summar") >= 0 || has_pair(ws, "sum", "up") || has(ws, "recap")) {
        c->what = DO_SUMMARISE;
        clip_range_said(ws, c->query, sizeof(c->query));
        char words[32];
        range_words(c->query, words, sizeof(words));
        snprintf(c->said, sizeof(c->said), "summarise the last %s", words);
        if (strcmp(c->query, "today") == 0) snprintf(c->said, sizeof(c->said), "summarise today");
        return;
    }
    // No built-in "clip": that phrase is left for a custom command (OBS, a replay buffer).

    // Private mode. Resume comes first: "stop private mode" is a resume.
    bool private_word = has(ws, "private") || has(ws, "privacy");
    bool off = has(ws, "off") || has(ws, "stop") || has(ws, "end") || has(ws, "leave") || has(ws, "exit") || has(ws, "disable");
    if (word_in(first, "resume", "unpause", "continue") || has_pair(ws, "start", "recording") ||
        has_pair(ws, "keep", "recording") || (has(ws, "record") && has(ws, "again")) || (private_word && off)) {
        command(c, "POST", "/resume", "resume recording");
        snprintf(c->shown, sizeof(c->shown), "Recording again");
        c->private_ok = true;
        return;
    }
    if (word_in(first, "pause") || private_word || has_pair(ws, "stop", "recording")) {
        command(c, "POST", "/pause", "private mode on");
        snprintf(c->shown, sizeof(c->shown), "Private mode on");
        return;
    }

    // "open spotify", "launch valorant", "start visual studio code".
    if (word_in(first, "open", "launch", "start", "run", "play") && ws->n > 1) {
        char app[128];
        app_said(ws, 1, ws->n, app, sizeof(app));
        if (!app[0]) return;
        command(c, "POST", "/launch", "");
        snprintf(c->query, sizeof(c->query), "app=%s", url_enc(a, str(app)).ptr);
        snprintf(c->said, sizeof(c->said), "open %s", app);
        return;
    }
}

static bool custom_said(const Words* raw, bool leading, VoiceCommand* c) {
    CmdMatch m;
    if (!commands_match(raw->w, raw->n, leading, &m)) return false;
    *c = {};
    c->what = DO_CUSTOM;
    c->custom = m;
    SET(c->said, str(m.label));
    return true;
}

// What the words after the wake name ask for. `ws` has the polite words dropped; `raw` is all of
// them, for the custom commands, whose slots may need a "you" ("play You Belong With Me"). A custom
// phrase that starts the sentence goes before the built-in commands, one later in it after them.
static void parse_command(const Words* ws, const Words* raw, VoiceCommand* c) {
    parse_builtin(ws, c);
    if (c->what == DO_CANCEL || !raw->n) return;
    if (custom_said(raw, true, c)) return;
    if (c->what == DO_NOTHING) custom_said(raw, false, c);
}

// What a sentence is: for us when it starts with the name (after a "hey" or "ok"), or comes
// within the wait after the name alone (`armed`). The name said in the middle of a sentence ("I
// was working on lifelog") is someone talking about it, not to it.
struct Understood {
    bool for_us;
    bool name_only;  // the name and nothing else: chime and wait for the command
    VoiceCommand c;  // DO_NOTHING when the rest is not a command
};

// `wake`: the wake word as typed in Settings ("lifelog", "Jarvis").
static void voice_understand(Str text, const char* wake, bool armed, Understood* u) {
    *u = {};
    Arena* a = scratch();
    Words ws;
    words_of(a, text, &ws);
    char key[64];
    wake_key(wake, key);
    // The word itself may start with "hey"; if not, a "hey" or "ok" before it is fine.
    i32 at = 0;
    i32 wake_n = ws.n ? wake_at(&ws, 0, key) : 0;
    if (!wake_n) {
        while (at < ws.n && word_in(ws.w[at], "hey", "hi", "ok", "okay", "so", "um", "uh", "oh", "yo", "and")) at++;
        wake_n = at < ws.n ? wake_at(&ws, at, key) : 0;
    }
    if (!wake_n && !armed) return;
    u->for_us = true;
    Words rest, raw = {};
    i32 from = wake_n ? at + wake_n : 0;
    words_between(&ws, from, ws.n, &rest);
    for (i32 i = from; i < ws.n; i++) raw.w[raw.n++] = ws.w[i];
    u->name_only = wake_n && rest.n == 0;
    parse_command(&rest, &raw, &u->c);
}

// ---------------------------------------------------------------------------------------------
// Results, on the main thread

struct VoiceDone {
    Chime chime;
    bool keep;  // a command: Settings shows it
    char heard[160];
    char result[160];
    char shown[128];  // the banner, when it worked; "" for none
};

static void voice_done_apply(App* app, Event* e) {
    VoiceDone* d = event_record<VoiceDone>(e);
    chime_play(app, d->chime);
    if (d->shown[0]) {
        char who[64];
        SET(who, str(app->config.voice.wake[0] ? app->config.voice.wake : "lifelog"));
        who[0] = (char)toupper((u8)who[0]);
        banner_show(who, d->shown);
    }
    if (!d->keep) return;
    Voice* v = &app->voice;
    SET(v->heard, str(d->heard));
    SET(v->result, str(d->result));
    v->heard_ts = now_ts();
    wake_window(app);
}

static void voice_say(App* app, Chime chime, const char* heard, const char* result, const char* shown = 0) {
    VoiceDone d = {};
    if (shown) SET(d.shown, str(shown));
    d.chime = chime;
    d.keep = heard != 0;
    if (heard) SET(d.heard, str(heard));
    if (result) SET(d.result, str(result));
    event_send(app, voice_done_apply, d);
}

static volatile LONG g_voice_checking;  // a check of the AI is running

static DWORD WINAPI voice_check_ai_main(void* p) {
    App* app = (App*)p;
    char said[512] = {};
    bool ok = summary_check_ai(app, said, sizeof(said));
    // The first line is enough for Settings: "loaded qwen3-14b in 12s".
    char* nl = strchr(said, '\n');
    if (nl) *nl = 0;
    LOG("voice: check the AI: %s", said);
    char shown[128];
    snprintf(shown, sizeof(shown), "AI works: %.100s", said);
    voice_say(app, ok ? CHIME_DONE : CHIME_ERROR, "check the AI", said, ok ? shown : 0);
    atomic_set(&g_voice_checking, 0);
    return 0;
}

// The main thread: the summary may be writing, and then it has the AI already.
static void voice_check_ai_apply(App* app, Event*) {
    if (app->summary.writing) {
        voice_say(app, CHIME_ERROR, "check the AI", "the summary is being written: it is running");
        return;
    }
    if (InterlockedCompareExchange(&g_voice_checking, 1, 0) != 0) return;
    Voice* v = &app->voice;
    SET(v->heard, str("check the AI"));
    SET(v->result, str("checking..."));
    v->heard_ts = now_ts();
    wake_window(app);
    start_thread(app, "voice-check-ai", voice_check_ai_main);
}

// ---------------------------------------------------------------------------------------------
// The voice thread

// The API's answer as a line for Settings: its "error", or what it did.
static void answer_text(Arena* a, const VoiceCommand* c, Answer* r, char* out, i32 cap) {
    Str body = buf_take(&r->body, a);
    Json j = {};
    char error[64];
    json_parse(a, body, &j, error, sizeof(error));
    Str err = json_text(json_get(&j, "error"));
    if (r->code != 200) {
        snprintf(out, cap, "%.*s", (int)err.len, err.len ? err.ptr : "it did not work");
        return;
    }
    Str result = json_text(json_get(&j, "result"));
    Str path = json_text(json_get(&j, "path"));
    if (result.len) snprintf(out, cap, "%.*s", (int)result.len, result.ptr);
    else if (path.len) snprintf(out, cap, "%d lines on the clipboard", (i32)json_get(&j, "lines")->num);
    else snprintf(out, cap, "done");
    (void)c;
}

// Summarising the last stretch, on its own thread (the AI can take a while to start).
struct VoiceSummarise {
    f64 from, to;
    char said[128];
    char title[64];   // "The last 30 minutes"
};
static volatile LONG g_voice_summarising;
static VoiceSummarise g_summarise;
static Arena g_summarise_arena;
static AppLabels g_summarise_labels;
// The newest this many characters are summarised (about 6,000 words for the AI's 12,288 tokens).
static const i64 SUMMARISE_MAX_CHARS = 24000;

static DWORD WINAPI voice_summarise_main(void* p) {
    App* app = (App*)p;
    Arena* a = &g_summarise_arena;
    VoiceSummarise* s = &g_summarise;
    char error[512] = {};
    i32 lines = 0;
    Str text = clip_text(&g_summarise_labels, a, s->from, s->to, &lines, error, sizeof(error));
    bool ok = !error[0];
    if (ok && !lines) {
        snprintf(error, sizeof(error), "nothing was heard then");
        ok = false;
    }
    if (ok && text.len > SUMMARISE_MAX_CHARS) {
        Str tail = {text.ptr + text.len - SUMMARISE_MAX_CHARS, SUMMARISE_MAX_CHARS};
        const char* nl = (const char*)memchr(tail.ptr, '\n', (size_t)tail.len);
        if (nl) tail = {nl + 1, tail.len - (nl + 1 - tail.ptr)};
        text = tail;
    }
    Str out = {};
    ok = ok && summary_quick(app, a, text, &out, error, sizeof(error));
    if (ok) {
        if (!clipboard_set_text(out)) WARN("voice: summarise: the clipboard is busy");
        ToastJob t = {};
        SET(t.tag, str("voice-summary"));
        SET(t.title, str(s->title));
        set(t.body, sizeof(t.body), out);
        notify_show(app, &t);
    }
    LOG("voice: %s: %s", s->said, ok ? "done" : error);
    char result[160];
    if (ok) snprintf(result, sizeof(result), "on the clipboard: %.*s", (int)(out.len < 120 ? out.len : 120), out.ptr);
    else snprintf(result, sizeof(result), "%s", error);
    char shown[128];
    snprintf(shown, sizeof(shown), "Summary copied: %s", s->title);
    voice_say(app, ok ? CHIME_DONE : CHIME_ERROR, s->said, result, ok ? shown : 0);
    arena_release(a);
    atomic_set(&g_voice_summarising, 0);
    return 0;
}

// The main thread: the app names for the transcript, then the thread.
static void voice_summarise_apply(App* app, Event* e) {
    VoiceSummarise* s = event_record<VoiceSummarise>(e);
    if (InterlockedCompareExchange(&g_voice_summarising, 1, 0) != 0) {
        voice_say(app, CHIME_ERROR, s->said, "already summarising");
        return;
    }
    g_summarise = *s;
    g_summarise_arena = {};
    g_summarise_labels = app_labels(app, &g_summarise_arena);
    Voice* v = &app->voice;
    SET(v->heard, str(s->said));
    SET(v->result, str("summarising..."));
    v->heard_ts = now_ts();
    wake_window(app);
    start_thread(app, "voice-summarise", voice_summarise_main);
}

// A custom command's script ended (on its thread).
static void voice_custom_done(App* app, const CmdMatch* m, bool ok, const char* message, const char* banner) {
    const char* result = message[0] ? message : ok ? "done" : "failed";
    const char* shown = banner[0] ? banner : message;
    voice_say(app, ok ? CHIME_DONE : CHIME_ERROR, m->label, result, ok && shown[0] ? shown : 0);
}

static void voice_do(App* app, const VoiceCommand* c, const char* heard) {
    Arena* a = scratch();
    Mark m = mark(a);
    bool paused = atomic_get(&app->capture.paused) != 0;
    if (c->what == DO_CANCEL) {
        voice_say(app, CHIME_CANCELLED, 0, 0);
    } else if (paused && !c->private_ok) {
        // Nothing about what was said is kept in private mode.
        voice_say(app, CHIME_ERROR, "(in private mode)", "only \"resume\" works in private mode");
    } else if (c->what == DO_CHECK_AI) {
        LOG("voice: check the AI");
        event_send(app, voice_check_ai_apply);
    } else if (c->what == DO_SUMMARISE) {
        VoiceSummarise s = {};
        s.to = now_ts();
        char label[64];
        bool ok = clip_range(str(c->query), s.to, &s.from, label, sizeof(label));
        SET(s.said, str(c->said));
        char words[32];
        range_words(c->query, words, sizeof(words));
        if (strcmp(c->query, "today") == 0) SET(s.title, str("Today"));
        else snprintf(s.title, sizeof(s.title), "The last %s", words);
        LOG("voice: \"%s\": %s", heard, c->said);
        if (ok) event_send(app, voice_summarise_apply, s);
        else voice_say(app, CHIME_ERROR, c->said, "could not tell how long");
    } else if (c->what == DO_REMIND) {
        char error[160] = {};
        bool ok = c->due > 0;
        if (!ok) snprintf(error, sizeof(error), "say when: \"in 10 minutes\" or \"at 5 pm\"");
        ok = ok && reminder_add(app, str(c->text), str(heard), c->due, error, sizeof(error));
        LOG("voice: \"%s\": %s%s%s", heard, c->said, ok ? "" : ": ", ok ? "" : error);
        voice_say(app, ok ? CHIME_DONE : CHIME_ERROR, c->said, ok ? "set" : error, ok ? c->shown : 0);
    } else if (c->what == DO_API) {
        Answer r = {};
        api_ask(app, str(c->method), str(c->path), str(c->query), &r);
        if (r.code == 200 && r.after) event_send(app, api_after_apply, r.after);
        char result[160];
        answer_text(a, c, &r, result, sizeof(result));
        buf_free(&r.body);
        bool ok = r.code == 200;
        LOG("voice: \"%s\": %s: %s", heard, c->said, result);
        // "started Spotify" from /launch: "Opening Spotify".
        char shown[128];
        if (strncmp(result, "started ", 8) == 0) snprintf(shown, sizeof(shown), "Opening %s", result + 8);
        else SET(shown, str(c->shown));
        voice_say(app, ok ? c->chime : CHIME_ERROR, c->said, result, ok && shown[0] ? shown : 0);
    } else if (c->what == DO_CUSTOM) {
        LOG("voice: \"%s\": %s", heard, c->said);
        commands_run(app, &c->custom, voice_custom_done);
    }
    rewind(m);
}

// Loads ggml-<model>.bin for the processor. Null, with the state set, if it could not.
static whisper_context* voice_open(App* app, const char* ai_dir, const char* model) {
    wchar_t bins[MAX_PATH], dll[MAX_PATH], bin[MAX_PATH];
    whisper_bin_dir(ai_dir, bins);
    swprintf(dll, MAX_PATH, L"%ls\\whisper.dll", bins);
    voice_model_path(ai_dir, model, bin);
    if (!exists(dll) || !exists(bin)) {
        atomic_set(&app->voice.state, VOICE_NEEDS_SETUP);
        return 0;
    }
    atomic_set(&app->voice.state, VOICE_LOADING);
    char error[256] = {};
    if (!whisper_load(bins, error, sizeof(error))) {
        WARN("voice commands: %s", error);
        atomic_set(&app->voice.state, VOICE_FAILED);
        return 0;
    }
    whisper_context_params cp = p_whisper_context_default_params();
    cp.use_gpu = false;  // the GPU is the transcriber's and the games'
    cp.flash_attn = false;
    cp.dtw_token_timestamps = false;
    Arena* a = scratch();
    Mark m = mark(a);
    f64 began = now_ts();
    whisper_context* ctx = p_whisper_init_from_file_with_params(narrow(a, bin).ptr, cp);
    rewind(m);
    if (!ctx) {
        WARN("voice commands: could not load ggml-%s.bin", model);
        atomic_set(&app->voice.state, VOICE_FAILED);
        return 0;
    }
    LOG("voice commands: listening (%s loaded in %.1f s)", model, now_ts() - began);
    atomic_set(&app->voice.state, VOICE_READY);
    return ctx;
}

// What was said, or empty if it was not speech.
static Str voice_text(Arena* a, whisper_context* ctx, const VoiceSentence* s, const char* wake) {
    const i32 pad = VOICE_RATE * 3 / 10;
    i32 total = s->n + 2 * pad;
    f32* samples = PUSH_ARRAY(a, f32, total);  // zeroed: the silence around it
    memcpy(samples + pad, s->samples, sizeof(f32) * s->n);
    whisper_full_params wp = p_whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.n_threads = 4;
    wp.language = "en";
    wp.no_context = true;
    wp.n_max_text_ctx = 0;
    wp.single_segment = true;
    wp.no_timestamps = true;
    wp.print_progress = false;
    wp.print_realtime = false;
    wp.print_timestamps = false;
    wp.print_special = false;
    // So the name comes out as one word (with no prompt "lifelog" is often "Lifeleg"); a prompt of
    // just the name makes it leave a first "Lifelog" out, as if already said.
    char prompt[128];
    snprintf(prompt, sizeof(prompt), "Commands for %s, the app.", wake);
    wp.initial_prompt = prompt;
    if (p_whisper_full(ctx, wp, samples, total) != 0) return {"", 0};
    Buf b = {};
    i32 n = p_whisper_full_n_segments(ctx);
    for (i32 i = 0; i < n; i++) {
        if (p_whisper_full_get_segment_no_speech_prob(ctx, i) > 0.6f) continue;
        buf_cstr(&b, p_whisper_full_get_segment_text(ctx, i));
        buf_cstr(&b, " ");
    }
    return buf_take(&b, a);
}

static DWORD WINAPI voice_main(void* p) {
    App* app = (App*)p;
    Voice* v = &app->voice;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(0, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // for ShellExecute
    WorkerOptions o = {};
    LONG version = -1;
    char loaded_dir[MAX_PATH] = {};
    char loaded_model[32] = {};
    whisper_context* ctx = 0;
    f64 armed_until = 0;  // the name was said alone: the next sentence is the command
    f64 tried = 0;
    char failed_dir[MAX_PATH] = {};  // the model would not load from there: not tried again
    char failed_model[32] = {};
    while (!stopping(app)) {
        DWORD wait = 10000;
        if (armed_until) {
            f64 left = armed_until - now_ts();
            wait = left > 0 ? (DWORD)(left * 1000) + 1 : 0;
        }
        HANDLE waits[2] = {app->stop, v->wake};
        if (WaitForMultipleObjects(2, waits, FALSE, wait) == WAIT_OBJECT_0) break;
        worker_options_take(app, &o, &version);
        bool listen = atomic_get(&v->listen) != 0;
        char model[32];
        AcquireSRWLockShared(&v->wake_lock);
        SET(model, str(v->model));
        ReleaseSRWLockShared(&v->wake_lock);
        bool changed = strcmp(loaded_dir, o.ai_dir) != 0 || strcmp(loaded_model, model) != 0;
        if (ctx && (!listen || changed)) {
            p_whisper_free(ctx);
            ctx = 0;
            tried = 0;  // the new one is loaded at once
        }
        if (!listen) {
            atomic_set(&v->state, VOICE_OFF);
            voice_drop_all(v);
            armed_until = 0;
            continue;
        }
        // Not there yet (set up from Settings, or the folder moved): look again every 10 s.
        bool failed_here = strcmp(failed_dir, o.ai_dir) == 0 && strcmp(failed_model, model) == 0;
        if (!ctx && o.ai_dir[0] && now_ts() - tried >= 9 && !failed_here) {
            tried = now_ts();
            ctx = voice_open(app, o.ai_dir, model);
            if (ctx) {
                SET(loaded_dir, str(o.ai_dir));
                SET(loaded_model, str(model));
            } else if (atomic_get(&v->state) == VOICE_FAILED) {
                SET(failed_dir, str(o.ai_dir));
                SET(failed_model, str(model));
            }
        }
        if (!ctx) {
            voice_drop_all(v);
            continue;
        }
        if (armed_until && now_ts() >= armed_until) {
            armed_until = 0;
            voice_say(app, CHIME_CANCELLED, 0, 0);
        }
        VoiceSentence s;
        while (voice_take(v, &s)) {
            Arena* a = scratch();
            Mark m = mark(a);
            char wake[64];
            AcquireSRWLockShared(&v->wake_lock);
            SET(wake, str(v->wake_word));
            ReleaseSRWLockShared(&v->wake_lock);
            Str text = voice_text(a, ctx, &s, wake);
            free(s.samples);
            bool armed = armed_until && now_ts() < armed_until;
            Understood u;
            voice_understand(text, wake, armed, &u);
            if (!u.for_us) {  // not for us: dropped, never logged
                rewind(m);
                continue;
            }
            Str heard = str_trim(text);
            char said[160];
            snprintf(said, sizeof(said), "%.*s", (int)(heard.len < 159 ? heard.len : 159), heard.ptr);
            if (u.c.what != DO_NOTHING) {
                armed_until = 0;
                voice_do(app, &u.c, said);
            } else if (u.name_only) {
                armed_until = now_ts() + VOICE_WAIT_SECS;
                voice_say(app, CHIME_LISTENING, 0, 0);
            } else {
                armed_until = 0;
                bool paused = atomic_get(&app->capture.paused) != 0;
                if (!paused) LOG("voice: \"%s\": not a command", said);
                voice_say(app, CHIME_DIDNT_CATCH, paused ? 0 : said, paused ? 0 : "not a command");
            }
            rewind(m);
        }
    }
    if (ctx) p_whisper_free(ctx);
    voice_drop_all(v);
    CoUninitialize();
    return 0;
}

void voice_start(App* app) {
    Voice* v = &app->voice;
    v->wake = CreateEventW(0, FALSE, FALSE, 0);
    CHECK(v->wake, "the voice thread's event (error %lu)", GetLastError());
    voice_publish_wake(app);
    atomic_set(&v->listen, app->config.voice.listen);
    atomic_set(&v->state, app->config.voice.listen ? VOICE_LOADING : VOICE_OFF);
    start_thread(app, "voice", voice_main);
    SetEvent(v->wake);
}

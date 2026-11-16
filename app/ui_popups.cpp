// ui_popups.cpp: the small windows by the tray: lifelog's own menu ("tray") and putting off the
// daily summary (the delay box). Both close once used, on Esc, or when you click elsewhere
// (main.cpp closes the popup when it loses focus).

// "Copy the last N min": clip_copy is slow (it reads the database, and mixing audio takes a
// while), so it runs on the pool. Kept outside PopupState because the popup may close first.
// The main thread's.
struct CopyState {
    bool busy;
    bool done;  // `result` is waiting to be shown
    ClipKind kind;
    ClipResult result;
};
static CopyState g_copy;

// What a finished copy says.
static const char* copy_message(ClipResult* r, ClipKind kind) {
    if (!r->ok) return tmp("%s", r->error);
    if (kind == CLIP_TEXT) return tmp("Copied %d lines", r->lines);
    if (kind == CLIP_FILE) return tmp("Copied %d lines as a file", r->lines);
    return tmp("Copied %s of audio", duration_text(r->seconds));
}

static void copy_done(App* app, ClipResult* r, ClipKind kind, i64) {
    g_copy.result = *r;
    g_copy.busy = false;
    g_copy.done = true;
    // With the menu gone, the window says it instead.
    if (!app->ui.popup_state) {
        Mark m = mark(scratch());
        ui_say(app, "%s", copy_message(r, kind));
        rewind(m);
        g_copy.done = false;
    }
    wake_window(app);
}

struct PopupState {
    bool ready;      // set up on the first frame
    // The tray menu.
    f32 amount;      // "Copy the last [15]"
    bool hours;      // min or h
    f64 changed_ts;  // when the amount last changed; the preview follows 200 ms later
    bool preview_loaded;
    u64 preview_asked;  // the request on the way (g_preview_seq); older results are dropped
    Arena arena;     // the preview's lines
    Slice<Line> preview;
    // The delay box.
    char at[8];      // "HH:MM"
    // A message on the popup: "Copied 12 lines", or what went wrong.
    char message[256];
    bool message_ok;
    f64 message_ts;
    f64 close_ts;    // close at this moment (after "OK, ..."), 0 for not
};

static u64 g_preview_seq;

struct PreviewJob {
    u64 asked;
    f64 start;
    Slice<Line> lines;
};

static void preview_run(sqlite3* db, Job* job) {
    PreviewJob* p = (PreviewJob*)job->data;
    if (db) p->lines = db_lines_after(db, &job->arena, p->start, 4);
}

static void preview_apply(App* app, Job* job) {
    PopupState* s = app->ui.popup_state;
    PreviewJob* p = (PreviewJob*)job->data;
    if (!s || p->asked != s->preview_asked) return;
    arena_release(&s->arena);
    s->arena = job_take_arena(job);
    s->preview = p->lines;
    s->preview_loaded = true;
    wake_window(app);
}

static void popup_say(PopupState* s, bool ok, const char* text) {
    SET(s->message, str(text));
    s->message_ok = ok;
    s->message_ts = now_ts();
}

static void popup_wait(f32* next_frame_ms, f32 ms) {
    if (*next_frame_ms < 0 || *next_frame_ms > ms) *next_frame_ms = ms;
}

// `text` cut to `width` with an ellipsis. Into scratch.
static const char* popup_fit_text(Str text, f32 width) {
    if (text.len > 400) text.len = 400;  // never more than a line's worth to measure
    const char* full = tmp("%.*s", (int)text.len, text.ptr);
    if (ImGui::CalcTextSize(full).x <= width) return full;
    f32 dots = ImGui::CalcTextSize("…").x;
    i64 n = text.len;
    while (n > 0) {
        n--;
        while (n > 0 && (full[n] & 0xC0) == 0x80) n--;  // never inside a UTF-8 character
        if (ImGui::CalcTextSize(full, full + n).x + dots <= width) break;
    }
    return tmp("%.*s…", (int)n, full);
}

static void message_label(PopupState* s) {
    const char* text = tmp("%s  %s", s->message_ok ? ICON_CHECK : ICON_X, s->message);
    ImGui::PushStyleColor(ImGuiCol_Text, s->message_ok ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ACCENT);
    ImGui::TextUnformatted(popup_fit_text(str(text), ImGui::GetContentRegionAvail().x));
    ImGui::PopStyleColor();
}

// A menu line: icon and text, no frame, the full width. Its hover reaches into the window's
// padding, so the icons line up with the lines above.
static bool menu_item(const char* icon, const char* text) {
    f32 pad = ImGui::GetStyle().FramePadding.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - pad);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5f));
    bool clicked = ImGui::Button(label(icon, text), ImVec2(ImGui::GetContentRegionAvail().x + pad, px(30)));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return clicked;
}

// A button that looks picked when `on` (min / h).
static bool choice(const char* text, bool on, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : weak_text());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(2), ImGui::GetStyle().FramePadding.y));
    bool clicked = ImGui::Button(text, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    return clicked;
}

// The preview box's background: a step further from the text than the window.
static ImVec4 sunken_bg() {
    ImVec4 w = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    bool dark = w.x + w.y + w.z < 1.5f;
    f32 k = dark ? 0.55f : 1.0f;
    if (dark) return ImVec4(w.x * k, w.y * k, w.z * k, 1);
    return ImVec4(1, 1, 1, 1);
}

static void start_copy(App* app, PopupState* s, f64 secs, ClipKind kind) {
    if (g_copy.busy) return;
    g_copy.busy = true;
    g_copy.done = false;
    g_copy.kind = kind;
    s->message[0] = 0;
    f64 to = now_ts();
    clip_copy_later(app, to - secs, to, hidden_speakers(app), kind, 0, copy_done, 0);
}

// One frame of lifelog's menu. False when it should close.
static bool tray_draw(App* app, PopupState* s, f32* next_frame_ms) {
    bool keep = true;
    f64 now = now_ts();

    // The header: lifelog, and the listening switch on the right.
    bool on = !app->capture.paused;
    ImGui::PushFont(0, 18);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("lifelog");
    ImGui::PopFont();
    {
        const char* state = on ? "Listening" : "Not listening";
        f32 sw = ImGui::GetFrameHeight() * 0.8f * 1.8f;
        f32 right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine(right - sw - px(8) - ImGui::CalcTextSize(state).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(weak_text(), "%s", state);
        ImGui::SameLine(right - sw);
        if (toggle("##listening", &on)) {
            capture_set_paused(app, !on);
            tray_update(app);
        }
    }
    ImGui::Separator();

    // "Copy the last  [15] min h" on one line.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label(ICON_HISTORY, "Copy the last"));
    {
        f32 right = ImGui::GetWindowContentRegionMax().x;
        f32 x = right - px(48 + 4 + 36 + 4 + 36);
        ImGui::SameLine(x);
        ImGui::SetNextItemWidth(px(48));
        const char* fmt = s->amount == floorf(s->amount) ? "%.0f" : "%.1f";
        if (ImGui::DragFloat("##amount", &s->amount, 0.5f, 1, 999, fmt, ImGuiSliderFlags_AlwaysClamp)) s->changed_ts = now;
        ImGui::SameLine(x + px(52));
        if (choice("min", !s->hours, ImVec2(px(36), 0))) {
            s->hours = false;
            s->changed_ts = now;
        }
        ImGui::SameLine(x + px(92));
        if (choice("h", s->hours, ImVec2(px(36), 0))) {
            s->hours = true;
            s->changed_ts = now;
        }
    }
    f64 secs = s->amount * (s->hours ? 3600.0 : 60.0);
    f64 start = now - secs;

    // Where that stretch starts: read again once the amount has stopped changing for 200 ms.
    if (s->changed_ts) {
        f64 left = s->changed_ts + 0.2 - now;
        if (left <= 0) {
            s->changed_ts = 0;
            Job* job = job_new(preview_apply);
            PreviewJob* p = PUSH(&job->arena, PreviewJob);
            p->asked = s->preview_asked = ++g_preview_seq;
            p->start = start;
            job->data = p;
            db_post(job, preview_run);
        } else {
            popup_wait(next_frame_ms, (f32)(left * 1000) + 10);
        }
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, sunken_bg());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), px(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(6), px(3)));
    ImGui::BeginChild("preview", ImVec2(0, px(92)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(0, 13);
    // The top line doubles as where "Copying…" and "Copied …" show.
    if (g_copy.busy) {
        spinner(next_frame_ms);
        ImGui::SameLine();
        ImGui::TextColored(weak_text(), "Copying…");
    } else if (s->message[0]) {
        message_label(s);
    } else {
        ImGui::TextColored(weak_text(), "From %s %s", day_name(start), hm_text(start));
    }
    if (!s->preview_loaded) {
        spinner(next_frame_ms);
    } else if (s->preview.count == 0) {
        ImGui::TextColored(weak_text(), "Nothing heard since then");
    } else {
        f32 w = ImGui::GetContentRegionAvail().x;
        for (i32 i = 0; i < s->preview.count && i < 3; i++) {
            Line* l = &s->preview.items[i];
            Str name = app_name(app, l->speaker);
            Str text = str_fmt(scratch(), "%s  %.*s: %.*s", hm_text(l->start_ts), (int)name.len, name.ptr, (int)l->text.len, l->text.ptr);
            ImGui::TextUnformatted(popup_fit_text(text, w));
        }
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    // Three equal buttons across the full width.
    {
        f32 gap = ImGui::GetStyle().ItemSpacing.x;
        f32 w = (ImGui::GetContentRegionAvail().x - 2 * gap) / 3;
        struct { const char* icon; const char* text; ClipKind kind; } kinds[] = {
            {ICON_FILE_TEXT, "File", CLIP_FILE}, {ICON_COPY, "Text", CLIP_TEXT}, {ICON_AUDIO_LINES, "Audio", CLIP_AUDIO}};
        ImGui::BeginDisabled(g_copy.busy);
        for (i32 i = 0; i < COUNT(kinds); i++) {
            if (i) ImGui::SameLine();
            if (ImGui::Button(label(kinds[i].icon, kinds[i].text), ImVec2(w, px(32)))) start_copy(app, s, secs, kinds[i].kind);
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), 0));
    if (menu_item(ICON_SCROLL_TEXT, "Timeline")) {
        ui_show(app, TAB_TIMELINE, 0);
        keep = false;
    }
    if (menu_item(ICON_CALENDAR, "Journal")) {
        ui_show(app, TAB_JOURNAL, 0);
        keep = false;
    }
    if (menu_item(ICON_PASTE, "Paste here")) {
        char error[256] = {};
        i32 n = files_paste(app, error, sizeof(error));
        // The paste thread says "Saved N items" in the window when it is done.
        if (n < 0) {
            popup_say(s, false, error);
        } else {
            popup_say(s, true, tmp("Saving %d item%s", n, n == 1 ? "" : "s"));
            s->close_ts = now + 0.9;
        }
    }
    if (menu_item(ICON_SETTINGS, "Settings")) {
        ui_show(app, TAB_SETTINGS, 0);
        keep = false;
    }
    if (menu_item(ICON_REFRESH, "Restart")) {
        app_restart(app);
        keep = false;
    }
    if (menu_item(ICON_POWER, "Quit")) {
        app_quit(app);
        keep = false;
    }
    ImGui::PopStyleVar();
    return keep;
}

// "HH:MM", `hours` from now.
static void hm_from_now(char out[8], f64 hours) {
    LocalTime t = local_time(now_ts() + hours * 3600);
    snprintf(out, 8, "%02d:%02d", t.hour, t.minute);
}

// One frame of the delay box. False when it should close.
static bool delay_draw(App* app, PopupState* s) {
    bool keep = true;
    const char* day = app->ui.popup_day[0] ? app->ui.popup_day : 0;
    ImGui::TextUnformatted(label(ICON_CLOCK, "Ask about the summary later"));
    if (day) {
        char today[11];
        today_day(today);
        if (strcmp(day, today) != 0) ImGui::TextColored(weak_text(), "For %s", page_name(day));
    }
    ImGui::Dummy(ImVec2(0, px(4)));

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("At");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(60));
    bool enter = ImGui::InputText("##at", s->at, sizeof(s->at), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank);
    for (i32 h = 1; h <= 3; h++) {
        ImGui::SameLine(0, px(4));
        if (ImGui::Button(tmp("+%d h", h))) hm_from_now(s->at, h);
    }
    ImGui::SameLine(0, px(4));
    if (ImGui::Button("23:30")) SET(s->at, str("23:30"));
    ImGui::Dummy(ImVec2(0, px(4)));

    if (ImGui::Button(label(ICON_CHECK, "Delay")) || enter) {
        Str r = summary_act(app, scratch(), "until", day, str_trim(str(s->at)).ptr);
        bool ok = str_starts(r, str("asking"));
        popup_say(s, ok, ok ? tmp("OK, %.*s", (int)r.len, r.ptr) : tmp("%.*s", (int)r.len, r.ptr));
        if (ok) s->close_ts = now_ts() + 0.9;
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip the day")) {
        Str r = summary_act(app, scratch(), "skip", day, 0);
        bool ok = str_eq(r, str("skipped"));
        popup_say(s, ok, ok ? "OK, skipped" : tmp("%.*s", (int)r.len, r.ptr));
        if (ok) s->close_ts = now_ts() + 0.9;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) keep = false;

    if (s->message[0]) {
        f32 h = ImGui::GetTextLineHeightWithSpacing();
        f32 bottom = ImGui::GetWindowContentRegionMax().y;
        if (ImGui::GetCursorPosY() < bottom - h) ImGui::SetCursorPosY(bottom - h);
        message_label(s);
    }
    return keep;
}

// One frame of the popup. False when it should close.
static bool popup_draw(App* app, PopupState* s, f32* next_frame_ms) {
    f64 now = now_ts();
    if (!s->ready) {
        s->ready = true;
        s->amount = 15;
        s->changed_ts = now - 1;  // the preview at once
        hm_from_now(s->at, 2);
        if (g_copy.done) g_copy.done = false;  // from a menu opened before
    }
    if (g_copy.done) {
        g_copy.done = false;
        popup_say(s, g_copy.result.ok, copy_message(&g_copy.result, g_copy.kind));
    }
    // Messages go after 4 s; "OK, ..." closes the popup shortly after.
    if (s->message[0]) {
        f64 left = s->message_ts + 4 - now;
        if (left <= 0) s->message[0] = 0;
        else popup_wait(next_frame_ms, (f32)(left * 1000) + 10);
    }
    if (s->close_ts) {
        if (now >= s->close_ts) return false;
        popup_wait(next_frame_ms, (f32)((s->close_ts - now) * 1000) + 10);
    }

    bool keep = app->ui.want_popup == POPUP_DELAY ? delay_draw(app, s) : tray_draw(app, s, next_frame_ms);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) keep = false;
    return keep;
}

static void popup_free(PopupState* s) {
    arena_release(&s->arena);
}

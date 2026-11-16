// ui_common.cpp: what the window's screens share: icons, colours, small widgets, time and size
// formatting, app icon textures, and which speakers are shown.

// Lucide icons (https://lucide.dev, ISC licence) as UTF-8, merged into the UI font.
#define ICON_PLAY "\xee\x84\xbf"           // U+E13F
#define ICON_SQUARE "\xee\x85\xaa"         // U+E16A
#define ICON_MIC "\xee\x84\x9b"            // U+E11B
#define ICON_SEARCH "\xee\x85\x94"         // U+E154
#define ICON_X "\xee\x86\xb1"              // U+E1B1
#define ICON_CHEVRON_UP "\xee\x81\xb4"     // U+E074
#define ICON_CHEVRON_DOWN "\xee\x81\xb1"   // U+E071
#define ICON_CHEVRON_LEFT "\xee\x81\xb2"   // U+E072
#define ICON_CHEVRON_RIGHT "\xee\x81\xb3"  // U+E073
#define ICON_FOLDER_OPEN "\xee\x89\x86"    // U+E246
#define ICON_FILE_TEXT "\xee\x83\x90"      // U+E0D0
#define ICON_EXTERNAL_LINK "\xee\x82\xbd"  // U+E0BD
#define ICON_CHECK "\xee\x81\xb0"          // U+E070
#define ICON_COPY "\xee\x82\xa2"           // U+E0A2
#define ICON_PASTE "\xee\x8f\xab"          // U+E3EB
#define ICON_POWER "\xee\x85\x83"          // U+E143
#define ICON_REFRESH "\xee\x85\x88"        // U+E148
#define ICON_SETTINGS "\xee\x85\x97"       // U+E157
#define ICON_SCROLL_TEXT "\xee\x91\xa3"    // U+E463
#define ICON_AUDIO_LINES "\xee\x95\x9e"    // U+E55E
#define ICON_APP_WINDOW "\xee\x90\xaa"     // U+E42A
#define ICON_CALENDAR "\xee\x8a\xb8"       // U+E2B8
#define ICON_FILE "\xee\x83\x84"           // U+E0C4
#define ICON_LIST "\xee\x84\x8b"           // U+E10B
#define ICON_IMAGE "\xee\x83\xb9"          // U+E0F9
#define ICON_VIDEO "\xee\x86\xa4"          // U+E1A4
#define ICON_CLOCK "\xee\x82\x8b"          // U+E08B
#define ICON_PLUS "\xee\x85\x80"           // U+E140
#define ICON_HISTORY "\xee\x87\xb4"        // U+E1F4
#define ICON_CIRCLE_PAUSE "\xee\x82\x84"   // U+E084
#define ICON_GAMEPAD "\xee\x83\xa2"        // U+E0E2
#define ICON_BELL "\xee\x81\x9d"           // U+E05D
#define ICON_GLOBE "\xee\x83\xab"          // U+E0EB
#define ICON_BAN "\xee\x81\x95"            // U+E055
#define ICON_LINK "\xee\x84\x87"           // U+E107
// Segoe UI Semibold, for titles; null if Windows lacks it. Each window has its own ImGui context
// and font atlas, so it is looked up in the current one: a pointer kept from another window's
// atlas is freed when that window closes. main.cpp's load_fonts adds it second.
static ImFont* bold_font() {
    ImFontAtlas* fonts = ImGui::GetIO().Fonts;
    return fonts->Fonts.Size > 1 ? fonts->Fonts[1] : 0;
}

#define ICON_TRASH "\xee\x86\x8e"          // U+E18E
#define ICON_FOLDER "\xee\x83\x9c"         // U+E0DC
#define ICON_BELL_RING "\xee\x88\xa3"      // U+E223
#define ICON_LOCATE "\xee\x87\x99"         // U+E1D9
#define ICON_ARROW_DOWN "\xee\x81\x86"     // U+E046

static const ImVec4 ACCENT = ImVec4(0 / 255.0f, 120 / 255.0f, 212 / 255.0f, 1);

// A size in pixels at 96 dpi, scaled to the window's screen (the style and fonts already are).
static f32 px(f32 v) { return v * ImGui::GetStyle().FontScaleDpi; }

static ImU32 accent_u32(f32 alpha = 1) { return ImGui::GetColorU32(ImVec4(ACCENT.x, ACCENT.y, ACCENT.z, alpha)); }
static ImVec4 weak_text() { return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled); }

// "icon  text", for buttons and tabs. Formatted into scratch.
static const char* label(const char* icon, const char* text) {
    return str_fmt(scratch(), "%s  %s", icon, text).ptr;
}

// A printf into scratch, for one frame's labels.
static const char* tmp(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return str_copy(scratch(), str(buf)).ptr;
}

// A small frameless button showing one icon, with a tooltip.
static bool icon_button(const char* icon, const char* tip, const char* id = 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    bool clicked = ImGui::Button(id ? tmp("%s##%s", icon, id) : icon);
    ImGui::PopStyleColor();
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return clicked;
}

// An on/off switch. True when it was flipped.
static bool toggle(const char* id, bool* on) {
    f32 h = ImGui::GetFrameHeight() * 0.8f, w = h * 1.8f;
    ImVec2 at = ImGui::GetCursorScreenPos();
    at.y += (ImGui::GetFrameHeight() - h) / 2;
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    if (clicked) *on = !*on;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = *on ? accent_u32() : ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), bg, h / 2);
    f32 r = h / 2 - px(3), x = *on ? at.x + w - h / 2 : at.x + h / 2;
    dl->AddCircleFilled(ImVec2(x, at.y + h / 2), r, IM_COL32(255, 255, 255, 255));
    return clicked;
}

// A centred column at most `max` wide (call column_end after).
static void column_begin(f32 max) {
    f32 avail = ImGui::GetContentRegionAvail().x;
    f32 width = avail < max ? avail : max;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - width) / 2);
    ImGui::BeginGroup();
    ImGui::PushItemWidth(width * 0.5f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
}
static void column_end() {
    ImGui::PopTextWrapPos();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

// A rounded card around a section (call card_end after).
static void card_begin(const char* id) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(14), px(14)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}
static void card_end() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------------------------------------
// Time and sizes

// "14:05:09"
static const char* clock_text(f64 ts) {
    LocalTime t = local_time(ts);
    return tmp("%02d:%02d:%02d", t.hour, t.minute, t.second);
}

// "14:05"
static const char* hm_text(f64 ts) {
    LocalTime t = local_time(ts);
    return tmp("%02d:%02d", t.hour, t.minute);
}

// WEEKDAYS and MONTHS are in summary.cpp.

static void today_day(char out[11]) { day_of(now_ts(), out); }

// Days from `day` to today (0 for today).
static i32 days_ago(const char* day) {
    f64 from, to, tf, tt;
    char today[11];
    today_day(today);
    day_bounds(day, &from, &to);
    day_bounds(today, &tf, &tt);
    return (i32)floor((tf - from) / 86400.0 + 0.5);
}

// "Today", "Yesterday" or "Wed 12 Nov", for the day `ts` is in.
static const char* day_name(f64 ts) {
    char day[11];
    day_of(ts, day);
    i32 ago = days_ago(day);
    if (ago == 0) return "Today";
    if (ago == 1) return "Yesterday";
    f64 from, to;
    day_bounds(day, &from, &to);
    LocalTime t = local_time(from + 12 * 3600);
    return tmp("%.3s %d %.3s", WEEKDAYS[t.weekday], t.day, MONTHS[t.month - 1]);
}

// "Today", "Yesterday" or "Wednesday 12 November" (with the year when it isn't this one).
static const char* page_name(const char* day) {
    i32 ago = days_ago(day);
    if (ago == 0) return "Today";
    if (ago == 1) return "Yesterday";
    f64 from, to;
    day_bounds(day, &from, &to);
    LocalTime t = local_time(from + 12 * 3600);
    LocalTime now = local_time(now_ts());
    if (t.year != now.year) return tmp("%s %d %s %d", WEEKDAYS[t.weekday], t.day, MONTHS[t.month - 1], t.year);
    return tmp("%s %d %s", WEEKDAYS[t.weekday], t.day, MONTHS[t.month - 1]);
}

// "5 min", "1 h 20 min"
static const char* duration_text(f64 secs) {
    i64 m = (i64)floor(secs / 60.0 + 0.5);
    if (m < 60) return tmp("%lld min", m);
    return tmp("%lld h %lld min", m / 60, m % 60);
}

// "now", "5 min ago", "3 h ago", "12 Nov"
static const char* ago_text(f64 ts) {
    if (ts <= 0) return "";
    f64 m = (now_ts() - ts) / 60.0;
    if (m < 2) return "now";
    if (m < 60) return tmp("%.0f min ago", floor(m + 0.5));
    if (m < 1440) return tmp("%.0f h ago", floor(m / 60 + 0.5));
    LocalTime t = local_time(ts);
    return tmp("%d %.3s", t.day, MONTHS[t.month - 1]);
}

static const char* size_text(i64 bytes) {
    f64 b = (f64)bytes;
    if (b > 1e9) return tmp("%.1f GB", b / 1e9);
    if (b > 1e6) return tmp("%.1f MB", b / 1e6);
    return tmp("%.0f KB", ceil(b / 1e3));
}

// An app's own name ("Brave Browser"), else "spotify.exe" → "spotify"; the mic is "me".
static Str app_name(App* app, Str exe) { return app_label(app, exe); }

// Whether a speaker is an excluded app, by its own decision or its folder's.
static bool speaker_excluded(App* app, Str speaker) {
    if (names_has(&app->config.capture.excluded_apps, speaker)) return true;
    AppInfo* a = app_find(app, speaker);
    return a && app_decision(app, a) == DECISION_EXCLUDED;
}

// ---------------------------------------------------------------------------------------------
// Speakers shown

// Whether a speaker's lines are shown (and played, and copied): not hidden, and not an excluded
// app unless "show dismissed" is on.
static bool speaker_visible(App* app, Str speaker) {
    if (names_has(&app->config.ui.hidden, speaker)) return false;
    if (app->config.ui.show_dismissed) return true;
    return !speaker_excluded(app, speaker);
}

// The speakers left out of copies and playback: the hidden ones plus excluded apps (unless shown).
static Names hidden_speakers(App* app) {
    Names out = {};
    for (i32 i = 0; i < app->config.ui.hidden.count; i++) names_set(&out, str(app->config.ui.hidden[i].s), true);
    if (!app->config.ui.show_dismissed)
        for (i32 i = 0; i < app->apps.list.count; i++)
            if (app_decision(app, &app->apps.list[i]) == DECISION_EXCLUDED) names_set(&out, str(app->apps.list[i].exe), true);
    return out;  // the caller frees it with list_free
}

static void set_hidden(App* app, Str speaker, bool hidden) {
    names_set(&app->config.ui.hidden, speaker, hidden);
    config_save_names("ui", "hidden", &app->config.ui.hidden);
}

static void set_show_dismissed(App* app, bool show) {
    app->config.ui.show_dismissed = show;
    config_save_bool("ui", "show_dismissed", show);
}

// ---------------------------------------------------------------------------------------------
// App icons as textures (shared by every window: one D3D11 device)

struct IconTexture {
    char exe[64];
    u32 version;
    ID3D11ShaderResourceView* view;  // null: the app has no icon
};
static List<IconTexture> g_icons;

static ID3D11ShaderResourceView* texture_from_rgba(const u8* rgba, i32 w, i32 h);

// The app's icon, or 0 if it has none (yet).
static ImTextureID app_icon(App* app, Str exe) {
    AppInfo* a = app_find(app, exe);
    if (!a || !a->icon) return 0;
    for (i32 i = 0; i < g_icons.count; i++) {
        IconTexture* t = &g_icons[i];
        if (!str_eq_nocase(str(t->exe), exe)) continue;
        if (t->version == a->icon_version) return (ImTextureID)t->view;
        if (t->view) t->view->Release();
        t->view = texture_from_rgba(a->icon, 32, 32);
        t->version = a->icon_version;
        return (ImTextureID)t->view;
    }
    IconTexture* t = list_push(&g_icons);
    SET(t->exe, exe);
    t->version = a->icon_version;
    t->view = texture_from_rgba(a->icon, 32, 32);
    return (ImTextureID)t->view;
}

// An app's icon at text height, or a stand-in glyph (the mic for "me").
static void draw_app_icon(App* app, Str speaker, f32 size = 0) {
    if (size <= 0) size = ImGui::GetTextLineHeight();
    ImTextureID tex = str_eq(speaker, str("me")) ? 0 : app_icon(app, speaker);
    if (tex) {
        ImGui::Image(ImTextureRef(tex), ImVec2(size, size));
    } else {
        ImGui::TextColored(weak_text(), "%s", str_eq(speaker, str("me")) ? ICON_MIC : ICON_APP_WINDOW);
    }
}

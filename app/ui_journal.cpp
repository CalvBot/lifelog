// ui_journal.cpp: the Journal tab. Each day's summary and the things
// you said you'd do. A point can be dismissed, or taken out with its topic never mentioned
// again. A month calendar on the left finds days far back.

// Width of a hover button beside a journal line.
static const f32 SLOT_BUTTON = 22;

// Lines heard per day: the days that can have a summary. Counting them reads every line, so it
// is done on a thread, once each time the window opens (and again when the day turns). Kept
// outside the window's state because the thread may finish after the window has closed.
struct HeardDays {
    bool loading;
    bool loaded;
    char today[11];  // the day it was counted on
    Job* job;              // holds `days`
    Slice<DayCount> days;  // oldest first
};
static HeardDays g_heard;

struct HeardJob {
    Slice<DayCount> days;
};

static void heard_run(sqlite3* db, Job* job) {
    if (db) ((HeardJob*)job->data)->days = db_days(db, &job->arena);
}

static void heard_apply(App* app, Job* job) {
    HeardJob* h = (HeardJob*)job->data;
    job_drop(g_heard.job);
    g_heard.job = job_hold(job);
    g_heard.days = h->days;
    g_heard.loading = false;
    g_heard.loaded = true;
    wake_window(app);
}

static i32 heard_lines(const char* day) {
    for (i32 i = 0; i < g_heard.days.count; i++)
        if (strcmp(g_heard.days.items[i].day, day) == 0) return g_heard.days.items[i].lines;
    return 0;
}

struct JournalState {
    char day[11];        // the day shown, "" for today
    i32 month;           // the month the calendar shows, as year * 12 + month - 1; 0 follows the day

    // The shown day's summary, read again when the day or Summary.version changes.
    Job* job;            // holds `summary`
    bool loaded;
    char loaded_day[11];
    u64 version;
    StoredSummary summary;

    // The point being taken out with "never mention", and the topic typed for it.
    bool never_open;
    bool never_focus;
    char never_bullet[1024];
    char never_topic[128];
    u64 asked;  // the request on the way (g_journal_seq); older results are dropped
};

static JournalState* g_journal;  // the window's journal while it is open
static u64 g_journal_seq;

struct JournalJob {
    u64 asked;
    char day[11];
    StoredSummary summary;
};

static void journal_run(sqlite3* db, Job* job) {
    JournalJob* j = (JournalJob*)job->data;
    if (db) j->summary = summary_get(db, &job->arena, j->day);
}

static void journal_apply(App* app, Job* job) {
    JournalState* s = g_journal;
    JournalJob* j = (JournalJob*)job->data;
    if (!s || j->asked != s->asked) return;
    job_drop(s->job);
    s->job = job_hold(job);
    s->summary = j->summary;
    wake_window(app);
}

static void journal_show_day(JournalState* s, const char* day) {
    if (day && day[0]) {
        SET(s->day, str(day));
        s->month = 0;
        s->never_open = false;
    }
    s->loaded = false;
}

static void journal_free(JournalState* s) {
    if (g_journal == s) g_journal = 0;
    job_drop(s->job);
    s->job = 0;
    // Count again next time the window opens: lines will have come in.
    if (!g_heard.loading) g_heard.loaded = false;
}

// ---------------------------------------------------------------------------------------------
// Small helpers

static void want_frame(f32* next_frame_ms, f32 ms) {
    if (*next_frame_ms < 0 || *next_frame_ms > ms) *next_frame_ms = ms;
}

// A turning arc at text height, for "writing".
static void spinner(f32* next_frame_ms) {
    f32 size = ImGui::GetTextLineHeight();
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImVec2 c = ImVec2(at.x + size / 2, at.y + size / 2);
    f32 start = (f32)ImGui::GetTime() * 6.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PathArcTo(c, size / 2 - px(2), start, start + 4.2f, 24);
    dl->PathStroke(accent_u32(), px(2));
    want_frame(next_frame_ms, 33);
}

// Text in another size (headings, small print).
static void sized_text(f32 size, ImVec4 color, Str text) {
    ImGui::PushFont(0, size);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text.ptr, text.ptr + text.len);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

static ImVec4 text_color() { return ImGui::GetStyleColorVec4(ImGuiCol_Text); }

// Between normal and weak, for days heard but not summed up.
static ImVec4 plain_color() {
    ImVec4 a = text_color(), b = weak_text();
    return ImVec4(a.x + (b.x - a.x) * 0.35f, a.y + (b.y - a.y) * 0.35f, a.z + (b.z - a.z) * 0.35f, a.w);
}

// A small icon button painted at (x, y) without taking layout space, so showing it on hover never
// moves the text. True when clicked.
static bool slot_button(f32 x, f32 y, const char* glyph, const char* tip) {
    f32 sb = px(SLOT_BUTTON);
    ImVec2 a = ImVec2(x, y - px(1)), b = ImVec2(x + sb, y - px(1) + sb);
    bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) {
        dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), px(4));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", tip);
    }
    ImVec2 size = ImGui::CalcTextSize(glyph);
    ImVec2 at = ImVec2(a.x + (sb - size.x) / 2, a.y + (sb - size.y) / 2);
    dl->AddText(at, ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled), glyph);
    return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// Drops **bold** and `code` marks. Into scratch.
static Str strip_md(Str s) {
    char* out = PUSH_ARRAY(scratch(), char, s.len + 1);
    i64 n = 0;
    for (i64 i = 0; i < s.len; i++) {
        if (s.ptr[i] == '`') continue;
        if (s.ptr[i] == '*' && i + 1 < s.len && s.ptr[i + 1] == '*') {
            i++;
            continue;
        }
        out[n++] = s.ptr[i];
    }
    out[n] = 0;
    return {out, n};
}

// The "HH:MM" at the end of a "To remember" point: "Call Sam back (14:05)". Empty if none.
static Str trailing_time(Str text) {
    text = str_trim(text);
    if (text.len < 7 || text.ptr[text.len - 1] != ')' || text.ptr[text.len - 7] != '(') return {};
    Str t = str_sub(text, text.len - 6, 5);
    return t.ptr[2] == ':' ? t : Str{};
}

// Unix time of "HH:MM" on journal day `day` (before 04:00 is the next morning); 0 if not a time.
static f64 moment(const char* day, Str hm) {
    i32 h, m;
    if (!parse_hhmm(hm, &h, &m)) return 0;
    return said_ts(day, hm);
}

// "Today" or "Wed 12 Nov", for the day list.
static const char* list_name(const char* day, const char* today) {
    if (strcmp(day, today) == 0) return "Today";
    f64 from, to;
    day_bounds(day, &from, &to);
    LocalTime t = local_time(from + 12 * 3600);
    return tmp("%.3s %d %.3s", WEEKDAYS[t.weekday], t.day, MONTHS[t.month - 1]);
}

static SummaryDay* summary_day(App* app, const char* day) {
    for (i32 i = 0; i < app->summary.days.count; i++)
        if (strcmp(app->summary.days[i].day, day) == 0) return &app->summary.days[i];
    return 0;
}

static i32 month_of(const char* day) {
    i32 y, m, d;
    if (!parse_day(day, &y, &m, &d)) return 0;
    return y * 12 + m - 1;
}

static void journal_open(JournalState* s, const char* day) {
    SET(s->day, str(day));
    s->month = month_of(day);
    s->never_open = false;
    s->loaded = false;
}

// ---------------------------------------------------------------------------------------------
// The calendar and the day list

// A month of days: bold for a summary, a dot for things to do, plain for heard but not summed up.
static void calendar(App* app, JournalState* s, const char* today) {
    i32 month = s->month ? s->month : month_of(s->day);
    i32 this_month = month_of(today);
    i32 year = month / 12, mon = month % 12 + 1;
    // Seven cells fill the column; the arrows sit over the first and last of them.
    f32 gap = px(2);
    f32 cell_w = floorf((ImGui::GetContentRegionAvail().x - 6 * gap) / 7), cell_h = px(28);
    f32 x0 = ImGui::GetCursorPosX();
    f32 row_w = 7 * cell_w + 6 * gap;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ImGui::Dummy(ImVec2(0, px(4)));
    f32 head_h = ImGui::GetFrameHeight();
    if (button(tmp("%s##prev-month", ICON_CHEVRON_LEFT), ImVec2(cell_w, head_h))) s->month = month - 1;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Month before");
    ImGui::SameLine();
    {
        const char* name = tmp("%s %d", MONTHS[mon - 1], year);
        f32 y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosX(x0 + (row_w - ImGui::CalcTextSize(name).x) / 2);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name);
        ImGui::SameLine(x0 + row_w - cell_w);
        ImGui::SetCursorPosY(y);
    }
    ImGui::BeginDisabled(month >= this_month);
    if (button(tmp("%s##next-month", ICON_CHEVRON_RIGHT), ImVec2(cell_w, head_h))) s->month = month + 1;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Month after");
    ImGui::EndDisabled();

    static const char* NAMES[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    ImGui::Dummy(ImVec2(0, px(2)));
    ImGui::PushFont(0, 13);
    for (i32 i = 0; i < 7; i++) {
        if (i) ImGui::SameLine(x0 + i * (cell_w + gap));
        f32 tw = ImGui::CalcTextSize(NAMES[i]).x;
        ImGui::SetCursorPosX(x0 + i * (cell_w + gap) + (cell_w - tw) / 2);
        ImGui::TextColored(weak_text(), "%s", NAMES[i]);
    }
    ImGui::PopFont();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    i32 column = local_time(local_ts(year, mon, 1, 12, 0)).weekday;
    for (i32 d = 1; d <= 31; d++) {
        LocalTime t = local_time(local_ts(year, mon, d, 12, 0));
        if (t.month != mon) break;
        char key[11];
        snprintf(key, sizeof(key), "%04d-%02d-%02d", year, mon, d);
        SummaryDay* sd = summary_day(app, key);
        bool done = sd && strcmp(sd->status, "done") == 0;
        i32 todo = sd ? sd->reminders : 0;
        bool heard = heard_lines(key) > 0;
        bool selected = strcmp(s->day, key) == 0;
        bool enabled = strcmp(key, today) <= 0 && (sd || heard);

        // Each row starts on a new line; the first day sits under its weekday.
        if (d > 1 && column) ImGui::SameLine(x0 + column * (cell_w + gap));
        else ImGui::SetCursorPosX(x0 + column * (cell_w + gap));
        ImVec4 bg = selected ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : done ? ImGui::GetStyleColorVec4(ImGuiCol_Button) : ImVec4(0, 0, 0, 0);
        ImGui::PushStyleColor(ImGuiCol_Button, bg);
        ImGui::PushStyleColor(ImGuiCol_Text, done ? text_color() : heard ? plain_color() : weak_text());
        ImGui::BeginDisabled(!enabled);
        if (button(tmp("%d##cal-%s", d, key), ImVec2(cell_w, cell_h))) journal_open(s, key);
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (strcmp(key, today) == 0) dl->AddRect(a, b, accent_u32(), ImGui::GetStyle().FrameRounding);
        if (todo > 0) dl->AddCircleFilled(ImVec2((a.x + b.x) / 2, b.y - px(4)), px(2), accent_u32());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (done && todo == 0) ImGui::SetTooltip("Summary");
            else if (done) ImGui::SetTooltip("Summary · %d to do", todo);
            else if (sd && strcmp(sd->status, "skipped") == 0) ImGui::SetTooltip("Skipped");
            else ImGui::SetTooltip("No summary yet");
        }
        column = (column + 1) % 7;
    }
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0, px(4)));
    f32 half = (row_w - ImGui::GetStyle().ItemSpacing.x) / 2;
    if (button("Today", ImVec2(month != this_month ? half : row_w, 0))) journal_open(s, today);
    if (month != this_month) {
        ImGui::SameLine();
        if (button("This month", ImVec2(half, 0))) s->month = this_month;
    }
    ImGui::Dummy(ImVec2(0, px(2)));
}

struct ListDay {
    char day[11];
    const char* status;  // "done", "skipped", "none", ...
    i32 reminders;
};

// The shown month's days, newest first: those with a summary, and those heard but not summed up.
static void day_list(App* app, JournalState* s, const char* today) {
    i32 month = s->month ? s->month : month_of(s->day);
    List<ListDay> list = {};
    for (i32 i = 0; i < app->summary.days.count; i++) {
        SummaryDay* d = &app->summary.days[i];
        if (month_of(d->day) != month) continue;
        ListDay* l = list_push(&list);
        SET(l->day, str(d->day));
        l->status = d->status;
        l->reminders = d->reminders;
    }
    // Today is always there, so it can be written.
    if (month_of(today) == month && !summary_day(app, today)) {
        ListDay* l = list_push(&list);
        SET(l->day, str(today));
        l->status = "none";
    }
    for (i32 i = 0; i < g_heard.days.count; i++) {
        DayCount* d = &g_heard.days.items[i];
        if (d->lines <= 0 || month_of(d->day) != month || summary_day(app, d->day) || strcmp(d->day, today) == 0) continue;
        ListDay* l = list_push(&list);
        SET(l->day, str(d->day));
        l->status = "none";
    }
    // Newest first (a short list: a month at most).
    for (i32 i = 1; i < list.count; i++)
        for (i32 j = i; j > 0 && strcmp(list[j - 1].day, list[j].day) < 0; j--) {
            ListDay t = list[j];
            list[j] = list[j - 1];
            list[j - 1] = t;
        }

    // The rest of the column, but never so short that only a day or two show: in a low window
    // the column scrolls instead (journal-left).
    f32 list_h = ImGui::GetContentRegionAvail().y;
    if (list_h < px(160)) list_h = px(160);
    ImGui::BeginChild("day-list", ImVec2(0, list_h));
    ImGui::Dummy(ImVec2(0, px(2)));
    if (list.count == 0) ImGui::TextColored(weak_text(), "Nothing this month");
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
    for (i32 i = 0; i < list.count; i++) {
        ListDay* d = &list[i];
        const char* extra = "";
        if (strcmp(d->status, "done") == 0) extra = d->reminders ? tmp("%d to do", d->reminders) : "";
        else if (strcmp(d->status, "skipped") == 0) extra = "skipped";
        else if (strcmp(d->status, "none") == 0) extra = strcmp(d->day, today) == 0 ? "" : "no summary";
        else extra = d->status;
        // The day, padded like the buttons above, and the rest in weak text on the right.
        ImVec2 at = ImGui::GetCursorScreenPos();
        f32 row_h = px(28), pad = ImGui::GetStyle().FramePadding.x;
        if (ImGui::Selectable(tmp("##list-%s", d->day), strcmp(s->day, d->day) == 0, 0, ImVec2(0, row_h))) journal_open(s, d->day);
        f32 right = ImGui::GetItemRectMax().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        f32 ty = at.y + (row_h - ImGui::GetTextLineHeight()) / 2;
        dl->AddText(ImVec2(at.x + pad, ty), ImGui::GetColorU32(ImGuiCol_Text), list_name(d->day, today));
        if (extra[0]) dl->AddText(ImVec2(right - pad - ImGui::CalcTextSize(extra).x, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), extra);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    list_free(&list);
}

// ---------------------------------------------------------------------------------------------
// The day

// The topics the summary never mentions; click one to allow it again.
static void never_list(App* app) {
    if (app->summary.never.count == 0) return;
    ImGui::Dummy(ImVec2(0, 6));
    sized_text(13, weak_text(), str("Never mentioned (click to allow again)"));
    ImGui::PushFont(0, 13);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8), px(2)));
    f32 right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    i64 remove = 0;
    for (i32 i = 0; i < app->summary.never.count; i++) {
        NeverTopic* t = &app->summary.never[i];
        const char* text = tmp("%s  %s##never-%lld", t->topic, ICON_X, t->id);
        f32 w = ImGui::CalcTextSize(text, 0, true).x + px(16);
        if (i) {
            ImGui::SameLine();
            if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
        }
        if (ImGui::Button(text)) remove = t->id;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Allow it again");
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (remove) summary_remove_never(app, remove);
}

static void write_now(App* app, const char* day, const char* said) {
    Str r = summary_act(app, scratch(), "now", day, 0);
    if (str_eq(r, str("writing"))) ui_say(app, "%s", said);
    else ui_say(app, "Summary: %.*s", (int)r.len, r.ptr);
}

static void day_view(App* app, JournalState* s, f32 col_w, bool writing, f32* next_frame_ms) {
    const char* day = s->day;
    StoredSummary* sum = &s->summary;

    ImGui::Dummy(ImVec2(0, px(2)));
    sized_text(22, text_color(), str(page_name(day)));
    ImGui::Dummy(ImVec2(0, 0));

    if (writing && strcmp(app->summary.writing_day, day) == 0) {
        spinner(next_frame_ms);
        ImGui::SameLine();
        ImGui::TextColored(weak_text(), "%s", app->summary.phase[0] ? app->summary.phase : "writing");
    }

    if (!sum->found || sum->markdown.len == 0) {
        bool skipped = sum->found && str_eq(sum->status, str("skipped"));
        ImGui::TextColored(weak_text(), "%s", skipped ? "Skipped." : "No summary yet.");
        if (!writing) {
            if (ImGui::Button(label(ICON_SCROLL_TEXT, "Write it now"))) write_now(app, day, "Writing the summary");
            if (!sum->found) {
                ImGui::SameLine();
                if (ImGui::Button("Skip this day")) {
                    Str r = summary_act(app, scratch(), "skip", day, 0);
                    ui_say(app, "Summary: %.*s", (int)r.len, r.ptr);
                }
            }
        }
        never_list(app);
        return;
    }

    // Markdown: headings, bullets and plain lines.
    const f32 slot_w = px(3 * SLOT_BUTTON + 8);
    Str rest = sum->markdown;
    while (rest.len) {
        i64 nl = str_find(rest, str("\n"));
        Str raw = nl < 0 ? rest : str_sub(rest, 0, nl);
        rest = nl < 0 ? Str{} : str_sub(rest, nl + 1);
        Str line = raw;
        while (line.len && (line.ptr[line.len - 1] == '\r' || line.ptr[line.len - 1] == ' ' || line.ptr[line.len - 1] == '\t')) line.len--;
        Str lead = line;
        while (lead.len && (lead.ptr[0] == ' ' || lead.ptr[0] == '\t')) lead = str_sub(lead, 1);

        if (str_starts(line, str("## ")) || str_starts(line, str("# "))) {
            ImGui::Dummy(ImVec2(0, px(2)));
            sized_text(18, text_color(), str_sub(line, str_starts(line, str("## ")) ? 3 : 2));
        } else if (str_starts(lead, str("- ")) || str_starts(lead, str("* "))) {
            Str bullet = str_sub(lead, 2);
            ImGui::PushID((i32)(raw.ptr - sum->markdown.ptr));
            ImVec2 row_min = ImGui::GetCursorScreenPos();
            ImGui::TextUnformatted("•");
            ImGui::SameLine();
            f32 width = col_w - (ImGui::GetCursorScreenPos().x - row_min.x) - slot_w - px(8);
            if (width < px(80)) width = px(80);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            Str shown = strip_md(bullet);
            ImGui::TextUnformatted(shown.ptr, shown.ptr + shown.len);
            ImGui::PopTextWrapPos();
            ImVec2 row_max = ImVec2(row_min.x + col_w, ImGui::GetItemRectMax().y);
            bool open = s->never_open && str_eq(str(s->never_bullet), bullet);
            if (open) {
                ImGui::Indent(px(16));
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(weak_text(), "Never mention");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(px(200));
                if (s->never_focus) {
                    ImGui::SetKeyboardFocusHere();
                    s->never_focus = false;
                }
                bool enter = ImGui::InputTextWithHint("##topic", "Topic", s->never_topic, sizeof(s->never_topic), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                bool save = ImGui::Button("Save") || enter;
                ImGui::SameLine();
                if (icon_button(ICON_X, "Cancel", "never-cancel")) s->never_open = false;
                ImGui::Unindent(px(16));
                if (save && str_trim(str(s->never_topic)).len) {
                    s->never_open = false;
                    summary_add_never(app, str(s->never_topic), day, bullet);
                }
            } else if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(row_min, row_max)) {
                // Drawn into space the row always keeps, so hovering never moves the text.
                f32 x = row_min.x + col_w - slot_w;
                Str hm = trailing_time(bullet);
                f64 ts = hm.len ? moment(day, hm) : 0;
                if (ts) {
                    if (slot_button(x, row_min.y, ICON_SCROLL_TEXT, "Show in timeline")) ui_jump(app, ts, strip_md(bullet));
                    x += px(SLOT_BUTTON + 4);
                }
                if (slot_button(x, row_min.y, ICON_X, "Dismiss (not worth keeping)")) {
                    summary_dismiss(app, day, bullet, -1);
                    ui_say(app, "Dismissed. Rewrite brings it back");
                }
                x += px(SLOT_BUTTON + 4);
                if (slot_button(x, row_min.y, ICON_BAN, "Never mention this topic")) {
                    s->never_open = true;
                    s->never_focus = true;
                    SET(s->never_bullet, bullet);
                    s->never_topic[0] = 0;
                }
            }
            ImGui::PopID();
        } else if (str_trim(line).len == 0) {
            ImGui::Dummy(ImVec2(0, 0));
        } else {
            Str shown = strip_md(line);
            ImGui::TextUnformatted(shown.ptr, shown.ptr + shown.len);
        }
    }

    if (sum->reminder_count) {
        ImGui::Dummy(ImVec2(0, px(6)));
        sized_text(18, text_color(), str("You said you'd"));
        for (i32 n = 0; n < sum->reminder_count; n++) {
            Reminder* r = &sum->reminders[n];
            bool set = str_eq(r->alert_state, str("set"));
            ImGui::PushID(n);
            ImVec2 row_min = ImGui::GetCursorScreenPos();
            ImGui::TextColored(weak_text(), "%.*s", (int)r->time.len, r->time.ptr);
            ImGui::SameLine();
            f32 width = col_w - (ImGui::GetCursorScreenPos().x - row_min.x) - px(3 * SLOT_BUTTON + 16);
            if (width < px(80)) width = px(80);
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextUnformatted(r->text.ptr, r->text.ptr + r->text.len);
            sized_text(13, weak_text(), str_fmt(scratch(), "\xe2\x80\x9c%.*s\xe2\x80\x9d", (int)r->said.len, r->said.ptr));
            if (set) sized_text(13, ACCENT, str_fmt(scratch(), "%s  Reminding you %.*s", ICON_BELL_RING, (int)r->alert_when.len, r->alert_when.ptr));
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImVec2 row_max = ImVec2(row_min.x + col_w, ImGui::GetItemRectMax().y);
            bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(row_min, row_max);
            f32 x = row_min.x + col_w - px(3 * SLOT_BUTTON + 8);
            // The bell stays in view while a notification is set; the rest show on hover.
            if (hovered || set) {
                const char* tip = set ? "Don't remind me"
                                : (r->alert_when.len && r->alert_due > now_ts()) ? tmp("Remind me %.*s", (int)r->alert_when.len, r->alert_when.ptr)
                                : "Remind me tomorrow at 09:00";
                if (slot_button(x, row_min.y, set ? ICON_BELL_RING : ICON_BELL, tip)) {
                    reminder_set(app, day, n, !set);  // it says when
                }
            }
            f64 ts = moment(day, r->time);
            if (hovered && ts && slot_button(x + px(SLOT_BUTTON + 4), row_min.y, ICON_SCROLL_TEXT, "Show in timeline")) ui_jump(app, ts, r->said);
            if (hovered && slot_button(x + px(2 * (SLOT_BUTTON + 4)), row_min.y, ICON_X, "Dismiss (not a real to-do)")) {
                // A notification that is on goes off first, while its place in the list is known.
                if (set) reminder_set(app, day, n, false, true);  // the dismiss says what happened
                summary_dismiss(app, day, {}, n);
                ui_say(app, "Dismissed. Rewrite brings it back");
            }
            ImGui::PopID();
        }
    }

    ImGui::Dummy(ImVec2(0, px(6)));
    const char* meta = tmp("Written %s %s%s%.*s", day_name(sum->created_ts), hm_text(sum->created_ts), sum->model.len ? " · " : "",
                           (int)sum->model.len, sum->model.ptr);
    // The Rewrite button at the usual size, the small print centred on it.
    f32 frame_h = ImGui::GetFrameHeight();
    ImVec2 at = ImGui::GetCursorPos();
    ImGui::PushFont(0, 13);
    ImGui::SetCursorPosY(at.y + (frame_h - ImGui::GetTextLineHeight()) / 2);
    ImGui::TextColored(weak_text(), "%s", meta);
    ImGui::PopFont();
    if (!writing) {
        ImGui::SameLine(0, px(12));
        ImGui::SetCursorPosY(at.y);
        if (ImGui::Button(label(ICON_REFRESH, "Rewrite"))) write_now(app, day, "Rewriting the summary");
    }
    never_list(app);
}

static void journal_draw(App* app, JournalState* s, f32* next_frame_ms) {
    char today[11];
    today_day(today);
    if (!s->day[0]) SET(s->day, str(today));
    g_journal = s;

    if (!app->storage.ready) {
        ImGui::TextWrapped("The database could not be opened. The log says why.");
        return;
    }
    if (!g_heard.loading && (!g_heard.loaded || strcmp(g_heard.today, today) != 0)) {
        g_heard.loading = true;
        SET(g_heard.today, str(today));
        Job* job = job_new(heard_apply);
        job->data = PUSH(&job->arena, HeardJob);
        db_post(job, heard_run);
    }

    if (!s->loaded || strcmp(s->loaded_day, s->day) != 0 || s->version != app->summary.version) {
        // Read on the database thread; the old one stays on screen until it comes.
        Job* job = job_new(journal_apply);
        JournalJob* j = PUSH(&job->arena, JournalJob);
        j->asked = s->asked = ++g_journal_seq;
        SET(j->day, str(s->day));
        job->data = j;
        db_post(job, journal_run);
        SET(s->loaded_day, str(s->day));
        s->version = app->summary.version;
        s->loaded = true;
    }
    bool writing = app->summary.writing;

    ImGui::BeginChild("journal-left", ImVec2(px(248), 0));
    calendar(app, s, today);
    ImGui::Separator();
    day_list(app, s, today);
    ImGui::EndChild();
    ImGui::SameLine();
    {
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y), ImVec2(at.x, at.y + ImGui::GetContentRegionAvail().y), ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(1, 0));
        ImGui::SameLine();
    }

    ImGui::BeginChild("journal-day");
    f32 avail = ImGui::GetContentRegionAvail().x - px(16);
    f32 col_w = avail < px(720) ? avail : px(720);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - col_w) / 2 + px(8));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + col_w);
    day_view(app, s, col_w, writing, next_frame_ms);
    ImGui::Dummy(ImVec2(0, px(12)));
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::EndChild();
}

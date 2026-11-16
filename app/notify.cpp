// notify.cpp: Windows notifications with buttons ("toasts"), through the WinRT ABI.
//
// A plain desktop app can show toasts once it has an app id registered under
// HKCU\Software\Classes\AppUserModelId. Clicking a button opens a `lifelog:` link; that link type
// is registered to run lifelog.exe <link>, which passes the click on to the running app
// (notify_forward, then notify_open_link) and exits. No COM server or Start-menu shortcut.
//
// A test copy (LIFELOG_CONFIG set) shows its toasts under its own app id with `lifelog-test:`
// links and registers no link type, so it never takes over or answers the installed lifelog's.
//
// The toasts are shown from the notify thread, which also owns the window links are sent to; the
// other threads only queue them, so no WinRT call is made with App.lock held.

#include <roapi.h>
#include <winstring.h>
#include <windows.ui.notifications.h>
#include <windows.data.xml.dom.h>
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "advapi32.lib")

static const char* NOTIFY_GROUP = "lifelog";
static const wchar_t* NOTIFY_CLASS = L"lifelog.notify";
static const ULONG_PTR LINK_MAGIC = 0x6C696E6B;  // "link", in WM_COPYDATA's dwData
enum : UINT { WM_NOTIFY_QUEUE = WM_APP + 40 };

static bool notify_test_mode() { return GetEnvironmentVariableW(L"LIFELOG_CONFIG", 0, 0) != 0; }
static const char* notify_app_id() { return notify_test_mode() ? "lifelog.test" : "lifelog.app"; }
static const char* link_scheme() { return notify_test_mode() ? "lifelog-test" : "lifelog"; }

// "lifelog:<path>" into `out`, `path` given printf-style.
static void make_link(char* out, i32 cap, const char* fmt, ...) {
    i32 n = snprintf(out, cap, "%s:", link_scheme());
    va_list args;
    va_start(args, fmt);
    vsnprintf(out + n, cap - n, fmt, args);
    va_end(args);
}

static void toast_button(ToastJob* t, const char* text, const char* link) {
    CHECK(t->button_count < COUNT(t->buttons), "at most %d buttons on a toast", COUNT(t->buttons));
    ToastButton* b = &t->buttons[t->button_count++];
    SET(b->text, str(text));
    SET(b->link, str(link));
}

// ---------------------------------------------------------------------------------------------
// Queueing (any thread)

static void notify_queue(App* app, ToastJob* job) {
    Notify* n = &app->summary.notify;
    AcquireSRWLockExclusive(&n->lock);
    list_push(&n->queue, *job);
    HWND w = n->window;
    ReleaseSRWLockExclusive(&n->lock);
    if (w) PostMessageW(w, WM_NOTIFY_QUEUE, 0, 0);
}

void notify_show(App* app, ToastJob* toast) {
    toast->remove = false;
    notify_queue(app, toast);
}

void notify_remove(App* app, const char* tag) {
    ToastJob job = {};
    job.remove = true;
    SET(job.tag, str(tag));
    notify_queue(app, &job);
}

// ---------------------------------------------------------------------------------------------
// Registration

static bool reg_string(const wchar_t* key, const wchar_t* name, const wchar_t* value) {
    LSTATUS rc = RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    if (rc != ERROR_SUCCESS) WARN("could not write the registry key HKCU\\%ls (error %ld)", key, (long)rc);
    return rc == ERROR_SUCCESS;
}

// Registers the app id (the name and logo shown on its notifications) and the `lifelog:` link
// type. Safe to repeat; done at every start so the path follows the exe.
static void notify_register() {
    wchar_t key[256], value[MAX_PATH + 16];
    Mark m = mark(scratch());
    swprintf(key, COUNT(key), L"Software\\Classes\\AppUserModelId\\%ls", wide(scratch(), str(notify_app_id())));
    reg_string(key, L"DisplayName", notify_test_mode() ? L"lifelog (test)" : L"lifelog");
    // The lifelog logo; shown when it is there.
    swprintf(value, COUNT(value), L"%ls\\logo.png", local_dir());
    if (GetFileAttributesW(value) != INVALID_FILE_ATTRIBUTES) reg_string(key, L"IconUri", value);
    rewind(m);
    if (notify_test_mode()) {
        LOG("notifications: test copy, the lifelog: link type is left as it is");
        return;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(0, exe, MAX_PATH);
    reg_string(L"Software\\Classes\\lifelog", 0, L"URL:lifelog");
    reg_string(L"Software\\Classes\\lifelog", L"URL Protocol", L"");
    swprintf(value, COUNT(value), L"\"%ls\" \"%%1\"", exe);
    reg_string(L"Software\\Classes\\lifelog\\shell\\open\\command", 0, value);
}

// ---------------------------------------------------------------------------------------------
// Showing (the notify thread)

using namespace ABI::Windows::UI::Notifications;
using namespace ABI::Windows::Data::Xml::Dom;

static HSTRING hstr(const wchar_t* s) {
    HSTRING h = 0;
    WindowsCreateString(s, (UINT32)wcslen(s), &h);
    return h;
}

static HSTRING hstr_utf8(Str s) {
    Mark m = mark(scratch());
    HSTRING h = hstr(wide(scratch(), s));
    rewind(m);
    return h;
}

static void xml_escape(Buf* b, const char* s) {
    for (; *s; s++) {
        if (*s == '&') buf_cstr(b, "&amp;");
        else if (*s == '<') buf_cstr(b, "&lt;");
        else if (*s == '>') buf_cstr(b, "&gt;");
        else if (*s == '"') buf_cstr(b, "&quot;");
        else list_push(&b->chars, *s);
    }
}

static IToastNotificationManagerStatics* toast_manager() {
    IToastNotificationManagerStatics* mgr = 0;
    HSTRING cls = hstr(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager);
    HRESULT hr = RoGetActivationFactory(cls, __uuidof(IToastNotificationManagerStatics), (void**)&mgr);
    WindowsDeleteString(cls);
    if (FAILED(hr)) WARN("notifications: no toast manager (0x%08lx)", (unsigned long)hr);
    return mgr;
}

static HRESULT toast_show(ToastJob* t) {
    Buf xml = {};
    buf_cstr(&xml, "<toast");
    if (t->launch[0]) {
        buf_cstr(&xml, " launch=\"");
        xml_escape(&xml, t->launch);
        buf_cstr(&xml, "\" activationType=\"protocol\"");
    }
    if (t->sticky) buf_cstr(&xml, " scenario=\"reminder\"");
    buf_cstr(&xml, "><visual><binding template=\"ToastGeneric\"><text>");
    xml_escape(&xml, t->title);
    buf_cstr(&xml, "</text><text>");
    xml_escape(&xml, t->body);
    buf_cstr(&xml, "</text></binding></visual><actions>");
    for (i32 i = 0; i < t->button_count; i++) {
        buf_cstr(&xml, "<action content=\"");
        xml_escape(&xml, t->buttons[i].text);
        buf_cstr(&xml, "\" activationType=\"protocol\" arguments=\"");
        xml_escape(&xml, t->buttons[i].link);
        buf_cstr(&xml, "\"/>");
    }
    buf_cstr(&xml, "</actions></toast>");
    Mark m = mark(scratch());
    Str text = buf_take(&xml, scratch());

    IInspectable* inspectable = 0;
    IXmlDocument* doc = 0;
    IXmlDocumentIO* io = 0;
    IToastNotificationFactory* factory = 0;
    IToastNotification* toast = 0;
    IToastNotification2* toast2 = 0;
    IToastNotificationManagerStatics* mgr = 0;
    IToastNotifier* notifier = 0;
    HSTRING xml_class = hstr(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument);
    HSTRING toast_class = hstr(RuntimeClass_Windows_UI_Notifications_ToastNotification);
    HSTRING content = hstr_utf8(text);
    HSTRING tag = hstr_utf8(str(t->tag));
    HSTRING group = hstr_utf8(str(NOTIFY_GROUP));
    HSTRING app_id = hstr_utf8(str(notify_app_id()));

    HRESULT hr = RoActivateInstance(xml_class, &inspectable);
    if (SUCCEEDED(hr)) hr = inspectable->QueryInterface(__uuidof(IXmlDocument), (void**)&doc);
    if (SUCCEEDED(hr)) hr = doc->QueryInterface(__uuidof(IXmlDocumentIO), (void**)&io);
    if (SUCCEEDED(hr)) hr = io->LoadXml(content);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(toast_class, __uuidof(IToastNotificationFactory), (void**)&factory);
    if (SUCCEEDED(hr)) hr = factory->CreateToastNotification(doc, &toast);
    if (SUCCEEDED(hr)) hr = toast->QueryInterface(__uuidof(IToastNotification2), (void**)&toast2);
    if (SUCCEEDED(hr)) hr = toast2->put_Tag(tag);
    if (SUCCEEDED(hr)) hr = toast2->put_Group(group);
    if (SUCCEEDED(hr)) {
        mgr = toast_manager();
        hr = mgr ? mgr->CreateToastNotifierWithId(app_id, &notifier) : E_FAIL;
    }
    if (SUCCEEDED(hr)) hr = notifier->Show(toast);

    if (notifier) notifier->Release();
    if (mgr) mgr->Release();
    if (toast2) toast2->Release();
    if (toast) toast->Release();
    if (factory) factory->Release();
    if (io) io->Release();
    if (doc) doc->Release();
    if (inspectable) inspectable->Release();
    WindowsDeleteString(xml_class);
    WindowsDeleteString(toast_class);
    WindowsDeleteString(content);
    WindowsDeleteString(tag);
    WindowsDeleteString(group);
    WindowsDeleteString(app_id);
    rewind(m);
    return hr;
}

// Takes a toast off the screen and out of the notification centre.
static void toast_remove(const char* tag_text) {
    IToastNotificationManagerStatics* mgr = toast_manager();
    IToastNotificationManagerStatics2* mgr2 = 0;
    IToastNotificationHistory* history = 0;
    HSTRING tag = hstr_utf8(str(tag_text));
    HSTRING group = hstr_utf8(str(NOTIFY_GROUP));
    HSTRING app_id = hstr_utf8(str(notify_app_id()));
    HRESULT hr = mgr ? mgr->QueryInterface(__uuidof(IToastNotificationManagerStatics2), (void**)&mgr2) : E_FAIL;
    if (SUCCEEDED(hr)) hr = mgr2->get_History(&history);
    if (SUCCEEDED(hr)) hr = history->RemoveGroupedTagWithId(tag, group, app_id);
    if (FAILED(hr)) LOG("could not remove toast %s (0x%08lx)", tag_text, (unsigned long)hr);
    if (history) history->Release();
    if (mgr2) mgr2->Release();
    if (mgr) mgr->Release();
    WindowsDeleteString(tag);
    WindowsDeleteString(group);
    WindowsDeleteString(app_id);
}

static void notify_drain(App* app) {
    Notify* n = &app->summary.notify;
    for (;;) {
        AcquireSRWLockExclusive(&n->lock);
        bool any = n->queue.count > 0;
        ToastJob job = {};
        if (any) {
            job = n->queue.items[0];
            list_remove(&n->queue, 0);
        }
        ReleaseSRWLockExclusive(&n->lock);
        if (!any) return;
        if (job.remove) {
            toast_remove(job.tag);
        } else {
            HRESULT hr = toast_show(&job);
            if (FAILED(hr)) ERR("could not show the notification \"%s\" (0x%08lx)", job.title, (unsigned long)hr);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Links

// The value of `key` in a query like "day=2026-11-15&id=3", into `out`.
static void query_value(Str query, const char* key, char* out, i32 cap) {
    out[0] = 0;
    Str k = str(key);
    while (query.len) {
        i64 amp = str_find(query, str("&"));
        Str part = amp < 0 ? query : str_sub(query, 0, amp);
        if (part.len > k.len && str_starts(part, k) && part.ptr[k.len] == '=') {
            set(out, cap, str_sub(part, k.len + 1));
            return;
        }
        query = amp < 0 ? Str{"", 0} : str_sub(query, amp + 1);
    }
}

// Only a few fixed actions exist, and the query may only hold a date or an id.
bool notify_open_link(App* app, Str link) {
    Str rest = {};
    if (str_starts(link, str("lifelog:"))) rest = str_sub(link, 8);
    else if (str_starts(link, str("lifelog-test:"))) rest = str_sub(link, 13);
    else {
        WARN("not a lifelog: link: %.*s", (int)link.len, link.ptr);
        return false;
    }
    while (rest.len && rest.ptr[0] == '/') rest = str_sub(rest, 1);
    i64 q = str_find(rest, str("?"));
    Str path = q < 0 ? rest : str_sub(rest, 0, q);
    Str query = q < 0 ? Str{"", 0} : str_sub(rest, q + 1);
    while (path.len && path.ptr[path.len - 1] == '/') path.len--;
    for (i64 i = 0; i < query.len; i++) {
        char c = query.ptr[i];
        if (!isalnum((u8)c) && c != '=' && c != '&' && c != '-') {
            WARN("unexpected characters in %.*s", (int)link.len, link.ptr);
            return false;
        }
    }
    char day[16], id[24];
    query_value(query, "day", day, sizeof(day));
    query_value(query, "id", id, sizeof(id));
    Mark m = mark(scratch());
    bool ok = true;
    if (str_eq(path, str("summary/open"))) {
        ui_show(app, TAB_JOURNAL, day);
    } else if (str_eq(path, str("summary/pick"))) {
        ui_popup(app, POPUP_DELAY, day);
    } else if (str_eq(path, str("summary/now")) || str_eq(path, str("summary/later")) || str_eq(path, str("summary/skip"))) {
        Str action = str_copy(scratch(), str_sub(path, 8));
        Str said = summary_act(app, scratch(), action.ptr, day, 0);
        LOG("summary link %s: %s", action.ptr, said.ptr);
    } else if (str_eq(path, str("remind/yes")) || str_eq(path, str("remind/no")) || str_eq(path, str("remind/done")) ||
               str_eq(path, str("remind/snooze"))) {
        Str action = str_copy(scratch(), str_sub(path, 7));
        reminder_answer(app, _strtoi64(id, 0, 10), action.ptr);
    } else {
        WARN("unknown link %.*s", (int)link.len, link.ptr);
        ok = false;
    }
    rewind(m);
    return ok;
}

bool notify_forward(const wchar_t* link) {
    HWND w = FindWindowExW(HWND_MESSAGE, 0, NOTIFY_CLASS, 0);
    if (!w) {
        WARN("a link was opened but lifelog is not running");
        return false;
    }
    // The running app opens a window for some links; let it come to the front, as this click would have.
    AllowSetForegroundWindow(ASFW_ANY);
    Mark m = mark(scratch());
    Str utf8 = narrow(scratch(), link);
    COPYDATASTRUCT data = {LINK_MAGIC, (DWORD)utf8.len, (void*)utf8.ptr};
    DWORD_PTR result = 0;
    bool ok = SendMessageTimeoutW(w, WM_COPYDATA, 0, (LPARAM)&data, SMTO_ABORTIFHUNG, 10000, &result) && result;
    rewind(m);
    return ok;
}

static LRESULT CALLBACK notify_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (app && msg == WM_NOTIFY_QUEUE) {
        notify_drain(app);
        return 0;
    }
    if (app && msg == WM_COPYDATA) {
        COPYDATASTRUCT* data = (COPYDATASTRUCT*)lp;
        if (data->dwData != LINK_MAGIC || data->cbData > 1024) return FALSE;
        char link[1025];
        memcpy(link, data->lpData, data->cbData);
        link[data->cbData] = 0;
        LOG("link: %s", link);
        app_lock(app);
        bool ok = notify_open_link(app, str(link));
        app_unlock(app);
        return ok;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI notify_main(void* param) {
    App* app = (App*)param;
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) WARN("notifications unavailable: WinRT did not start (0x%08lx)", (unsigned long)hr);
    notify_register();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = notify_proc;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = NOTIFY_CLASS;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, NOTIFY_CLASS, L"lifelog", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, wc.hInstance, 0);
    CHECK(hwnd, "the notifications' message window (error %lu)", GetLastError());
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
    // Links come from the lifelog.exe the shell starts, which runs at the same level.
    ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, 0);
    Notify* n = &app->summary.notify;
    AcquireSRWLockExclusive(&n->lock);
    n->window = hwnd;
    ReleaseSRWLockExclusive(&n->lock);
    notify_drain(app);  // what was queued before the window existed

    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &app->stop, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
    AcquireSRWLockExclusive(&n->lock);
    n->window = 0;
    ReleaseSRWLockExclusive(&n->lock);
    DestroyWindow(hwnd);
    return 0;
}

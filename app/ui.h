// ui.h: the window (Timeline, Journal, Settings), the small menu by the tray, the delay box, the
// tray icon, and the notifications' links. The window draws App each frame; its buttons call the
// module functions directly.

#pragma once

enum Tab : u8 {
    TAB_TIMELINE,
    TAB_JOURNAL,
    TAB_SETTINGS,
};

enum Popup : u8 {
    POPUP_NONE,
    POPUP_TRAY,   // lifelog's own menu by the tray
    POPUP_DELAY,  // "Ask about the summary later"
};

struct UiState;     // ui.cpp's own: the window's widgets' state, freed when the window closes
struct PopupState;  // ui_popups.cpp's own, for the popup window

struct Ui {
    // Asked for by other threads; the main thread opens them.
    bool want_window;
    Tab want_tab;
    char want_day[11];
    bool want_jump;        // show this moment in the timeline (a reminder's "show me where")
    f64 jump_ts;
    char jump_said[256];   // words said then, to find the exact line
    Popup want_popup;
    char popup_day[11];
    bool want_apps;       // the timeline's "All apps…" window

    HWND window;          // the main window while it is open
    HWND popup;
    HWND apps;            // the "All apps…" window while it is open
    HWND tray;            // the tray icon's message window
    DWORD main_thread;
    char toast[256];      // a short message at the bottom of the window
    f64 toast_ts;
    UiState* state;
    PopupState* popup_state;
    Tab tab;              // the tab on screen
};

// Open the main window on `tab` (and `day`, for the journal), or a popup. Any thread, with the lock.
void ui_show(App* app, Tab tab, const char* day);
void ui_popup(App* app, Popup popup, const char* day);
void ui_say(App* app, const char* fmt, ...);
void ui_open_apps(App* app);  // the timeline's "All apps…" window

// For main.cpp: draw one frame of the main window or the popup, with the lock held. False when
// it should close. `next_frame_ms`: set to 0 to draw again at once, or to a wait for an animation.
bool ui_draw_window(App* app, f32* next_frame_ms);
bool ui_draw_popup(App* app, f32* next_frame_ms);
bool ui_draw_apps(App* app, f32* next_frame_ms);  // the "All apps…" window, part of the main one
void ui_apps_closed(App* app);
// The window's own state is made when it opens and freed when it closes.
void ui_window_opened(App* app);
void ui_window_closed(App* app);
void ui_popup_opened(App* app);
void ui_popup_closed(App* app);
// Show `ts` in the timeline (switches to it). Any thread, with the lock.
void ui_jump(App* app, f64 ts, Str said);

// Quit or restart the whole program. Any thread, with the lock.
void app_quit(App* app);
void app_restart(App* app);

// The tray icon follows App.capture.paused.
void tray_update(App* app);

// lifelog.cpp: the whole program as one translation unit (a unity build). build.bat compiles this
// file; the vendored libraries (SQLite, Dear ImGui) are compiled once on their own and linked.
// Headers come in dependency order, then every .cpp.

#include "base.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shlobj.h>

#include "vendor/sqlite/sqlite3.h"
#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_impl_win32.h"
#include "vendor/imgui/imgui_impl_dx11.h"

struct App;

#include "disk.h"
#include "config.h"
#include "apps.h"
#include "capture.h"
#include "worker.h"
#include "db.h"
#include "summary.h"
#include "setup.h"
#include "clip.h"
#include "chime.h"
#include "ui.h"
#include "events.h"
#include "app.h"
#include "pool.h"

#include "base.cpp"
#include "events.cpp"
#include "config.cpp"
#include "disk.cpp"
#include "db.cpp"
#include "pool.cpp"
#include "apps.cpp"
#include "capture.cpp"
#include "icon.cpp"
#include "json.cpp"
#include "llm.cpp"
#include "setup.cpp"
#include "worker.cpp"
#include "notify.cpp"
#include "summary.cpp"
#include "clip.cpp"
#include "files.cpp"
#include "player.cpp"
#include "chime.cpp"
#include "ui_common.cpp"
#include "ui_timeline.cpp"
#include "ui_journal.cpp"
#include "ui_settings.cpp"
#include "ui_popups.cpp"
#include "ui.cpp"
#include "api.cpp"
#include "cli.cpp"
#include "main.cpp"

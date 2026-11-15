// apps.cpp: see apps.h. The scan of installed apps, the process watcher and icons are to come.

AppInfo* app_find(App* app, Str exe) {
    for (i32 i = 0; i < app->apps.list.count; i++)
        if (str_eq_nocase(str(app->apps.list[i].exe), exe)) return &app->apps.list[i];
    return 0;
}

AppInfo* app_get(App* app, Str exe) {
    AppInfo* a = app_find(app, exe);
    if (a) return a;
    a = list_push(&app->apps.list);
    Mark m = mark(scratch());
    SET(a->exe, str_lower(scratch(), exe));
    rewind(m);
    return a;
}

void apps_from_config(App* app) {
    Config* c = &app->config;
    for (i32 i = 0; i < c->capture.included_apps.count; i++) app_get(app, str(c->capture.included_apps[i].s))->decision = DECISION_INCLUDED;
    for (i32 i = 0; i < c->capture.excluded_apps.count; i++) app_get(app, str(c->capture.excluded_apps[i].s))->decision = DECISION_EXCLUDED;
    for (i32 i = 0; i < c->processing.games.count; i++) app_get(app, str(c->processing.games[i].s))->game = GAME_YES;
    for (i32 i = 0; i < c->processing.not_games.count; i++) app_get(app, str(c->processing.not_games[i].s))->game = GAME_DISMISSED;
}

void app_set_decision(App* app, Str exe, Decision d) {
    app_get(app, exe)->decision = d;
    Config* c = &app->config;
    names_set(&c->capture.included_apps, exe, d == DECISION_INCLUDED);
    names_set(&c->capture.excluded_apps, exe, d == DECISION_EXCLUDED);
    config_save_names("capture", "included_apps", &c->capture.included_apps);
    config_save_names("capture", "excluded_apps", &c->capture.excluded_apps);
}

void app_set_game(App* app, Str exe, GameMark g) {
    app_get(app, exe)->game = g;
    Config* c = &app->config;
    names_set(&c->processing.games, exe, g == GAME_YES);
    names_set(&c->processing.not_games, exe, g == GAME_DISMISSED);
    config_save_names("processing", "games", &c->processing.games);
    config_save_names("processing", "not_games", &c->processing.not_games);
}

void apps_start(App* app) {
    (void)app;
}

// worker.cpp: see worker.h. To come.

void worker_start(App* app) { SET(app->worker.state, str("not started")); }

void worker_set_mode(App* app, Mode mode) {
    app->config.processing.mode = mode;
    config_save_string("processing", "mode", mode_name(mode));
}

void worker_set_gpu_limit(App* app, u32 percent) {
    app->config.processing.gpu_busy_percent = percent;
    config_save_u32("processing", "gpu_busy_percent", percent);
}

void worker_stop(App* app) { (void)app; }

// disk.h: the file writer. One thread does the small writes the other threads ask for, in the
// order they asked: config.toml keys and lines added to a file (paths.txt). A click never waits
// on the disk.
//
// Before disk_start and after disk_stop (the CLI, the start of run_app, the end of shutdown) the
// writes happen at once, on the calling thread.

void disk_start();
// Waits until every write asked for has been done, then ends the thread.
void disk_stop();

// Sets `key` in `[table]` of config.toml to `value` (TOML text, already quoted), keeping the rest.
void disk_save_key(const char* table, const char* key, Str value);
// Adds `text` to the end of `file`.
void disk_append(const wchar_t* file, Str text);

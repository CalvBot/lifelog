// cli.cpp: `lifelog.exe <command>` from a terminal (port of src/main): talks to the running
// lifelog through its API, or works straight from the database where the old CLI did.

// Runs a command and returns the exit code. Called before anything else starts.
static i32 cli_main(i32 argc, wchar_t** argv) {
    (void)argc; (void)argv;
    return 1;
}

// chime.h: the short sounds voice commands make, synthesised here (no sound files). The style and
// volume are in config.toml's [chimes] and the Settings card of that name.

#pragma once

enum Chime : i32 {
    CHIME_LISTENING,    // the wake name was heard: go ahead
    CHIME_DONE,         // the command was carried out
    CHIME_CLIP_SAVED,   // a clip was saved
    CHIME_DIDNT_CATCH,  // the command could not be made out
    CHIME_ERROR,        // it could not be carried out (e.g. clipback not running)
    CHIME_CANCELLED,    // nothing was said after the wake name
    CHIME_COUNT,
};

// "Listening", "Clip saved" and so on, for Settings.
const char* chime_label(Chime c);

// Main thread: plays `c` in the configured style and volume, cutting off any chime still playing.
// Nothing when chimes are off.
void chime_play(App* app, Chime c);

// Plays `c` as `style` at `volume` (0-100), whatever is configured: Settings' previews.
void chime_play_as(Chime c, ChimeStyle style, u32 volume);

// Main thread: change and save the settings.
void chime_set_style(App* app, ChimeStyle style);
void chime_set_volume(App* app, u32 volume);

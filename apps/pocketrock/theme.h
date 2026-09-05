#ifndef POCKETROCK_THEME_H
#define POCKETROCK_THEME_H

#include <stdbool.h>

/* Replace every user-selected native theme setting with PocketRock's fixed
   compatibility palette. Resource reload is only safe while playback is
   stopped because Rockbox's skin loader releases the audio buffer. */
void pocketrock_apply_native_theme(bool reload_resources);

#endif

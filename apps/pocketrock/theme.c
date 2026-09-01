#include "config.h"
#include "audio.h"
#include "settings.h"
#include "string.h"
#include "theme.h"

struct pocketrock_theme_setting {
    const char *name;
    const char *value;
};

static const struct pocketrock_theme_setting theme_settings[] = {
    { "wps", ROCKBOX_DIR "/wps/pocketrock.wps" },
    { "sbs", ROCKBOX_DIR "/wps/pocketrock.sbs" },
    { "backdrop", "-" },
    { "filetype colours", "-" },
    { "foreground color", "18202A" },
    { "background color", "F5F6F8" },
    { "line selector start color", "2378D4" },
    { "line selector end color", "2378D4" },
    { "line selector text color", "18202A" },
    { "list separator color", "D5D9DF" },
    { "selector type", "bar (color)" },
    { "font", ROCKBOX_DIR "/fonts/15-Adobe-Helvetica.fnt" },
    { "statusbar", "off" },
    { "iconset", "-" },
    { "viewers iconset", "-" },
    { "show icons", "off" },
    { "scrollbar", "off" },
    { "ui viewport", "-" },
};

void pocketrock_apply_native_theme(bool reload_resources)
{
    bool theme_changed = false;
    char value[MAX_PATH];
    for (size_t i = 0; i < ARRAYLEN(theme_settings); ++i) {
        size_t length = strlen(theme_settings[i].value);
        if (length >= sizeof(value))
            length = sizeof(value) - 1;
        memcpy(value, theme_settings[i].value, length);
        value[length] = '\0';
        string_to_cfg(theme_settings[i].name, value, &theme_changed);
    }

    /* Always refresh the palette used by gui_synclist and native plugins.
       Loading skin/font files is deliberately skipped during playback because
       settings_apply_skins() stops audio to reclaim its buffers. */
    settings_apply(reload_resources);
    if (reload_resources && !(audio_status() & AUDIO_STATUS_PLAY))
        settings_apply_skins();
}

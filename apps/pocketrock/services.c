#include "config.h"
#include "audio.h"
#include "backlight.h"
#include "dir.h"
#include "file.h"
#include "kernel.h"
#include "metadata.h"
#include "misc.h"
#include "mv.h"
#include "playlist.h"
#include "playback.h"
#include "plugin.h"
#include "pocketrock.h"
#include "package.h"
#include "power.h"
#include "powermgmt.h"
#include "settings.h"
#include "string.h"
#include "usb.h"
#ifdef HAVE_TAGCACHE
#include "tagcache.h"
#endif
#include <stdarg.h>
#include <stdio.h>

#define RESPONSE_SIZE 8192
#define POCKETROCK_DISABLE_APPS ROCKBOX_DIR "/pocketrock/disable-third-party"
static char response[RESPONSE_SIZE];
static size_t used;
static int pending_exit;
static char active_package[MAX_PATH];
static struct pocketrock_request pending_request;
static sector_t cached_total_sectors;
static sector_t cached_free_sectors;
static long next_storage_refresh;

static void reset_response(void) { used = 0; response[0] = '\0'; }

static bool appendf(const char *format, ...)
{
    if (used >= sizeof(response) - 1) return false;
    va_list ap;
    va_start(ap, format);
    int count = vsnprintf(response + used, sizeof(response) - used, format, ap);
    va_end(ap);
    if (count < 0 || (size_t)count >= sizeof(response) - used) {
        used = sizeof(response) - 1;
        response[used] = '\0';
        return false;
    }
    used += (size_t)count;
    return true;
}

static bool append_json_string(const char *value)
{
    if (!appendf("\"")) return false;
    if (!value) value = "";
    for (; *value; ++value) {
        unsigned char c = (unsigned char)*value;
        if (c == '"' || c == '\\') {
            if (!appendf("\\%c", c)) return false;
        } else if (c < 0x20) {
            if (!appendf("\\u%04x", c)) return false;
        } else if (!appendf("%c", c)) return false;
    }
    return appendf("\"");
}

static long json_number(const char *payload, const char *key, long fallback)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *at = strstr(payload, needle);
    return at ? strtol(at + strlen(needle), NULL, 10) : fallback;
}

static bool json_bool(const char *payload, const char *key, bool fallback)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *at = strstr(payload, needle);
    if (!at) return fallback;
    at += strlen(needle);
    return !strncmp(at, "true", 4);
}

static bool json_string_value(
    const char *payload, const char *key, char *out, size_t out_size)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *at = strstr(payload, needle);
    if (!at || out_size == 0) return false;
    at += strlen(needle);
    size_t length = 0;
    while (*at && *at != '"') {
        char value = *at++;
        if (value == '\\') {
            value = *at++;
            if (!value) return false;
            if (value == 'n') value = '\n';
            else if (value == 'r') value = '\r';
            else if (value == 't') value = '\t';
        }
        if (length + 1 >= out_size) return false;
        out[length++] = value;
    }
    if (*at != '"') return false;
    out[length] = '\0';
    return true;
}

static int queue_paths(const char *payload, bool replace)
{
    const char *at = strstr(payload, "\"paths\":[");
    if (!at) return -1;
    at += strlen("\"paths\":[");
    if (replace && playlist_remove_all_tracks(NULL) < 0)
        return -1;
    int inserted = 0;
    while (*at && *at != ']') {
        while (*at == ' ' || *at == ',') ++at;
        if (*at == ']') break;
        if (*at++ != '"') return -1;
        char path[MAX_PATH];
        size_t length = 0;
        while (*at && *at != '"') {
            char value = *at++;
            if (value == '\\') {
                value = *at++;
                if (!value) return -1;
                if (value == 'n') value = '\n';
                else if (value == 'r') value = '\r';
                else if (value == 't') value = '\t';
            }
            if (length + 1 >= sizeof(path)) return -1;
            path[length++] = value;
        }
        if (*at++ != '"') return -1;
        path[length] = '\0';
        if (playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, true) < 0)
            return -1;
        if (++inserted >= 64) break;
    }
    if (replace && inserted > 0) {
        long start = json_number(payload, "startIndex", 0);
        if (start < 0) start = 0;
        if (start >= inserted) start = inserted - 1;
        playlist_start((int)start, 0, 0);
    }
    return inserted;
}

static bool has_suffix(const char *value, const char *suffix)
{
    size_t a = strlen(value), b = strlen(suffix);
    return a >= b && !strcmp(value + a - b, suffix);
}

static void append_native_directory(const char *directory)
{
    DIR *dir = opendir(directory);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && used < sizeof(response) - MAX_PATH - 128) {
        if (!has_suffix(entry->d_name, ".rock")) continue;
        char path[MAX_PATH], title[MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        strlcpy(title, entry->d_name, sizeof(title));
        char *dot = strrchr(title, '.');
        if (dot) *dot = '\0';
        if (response[used - 1] != '[') appendf(",");
        appendf("{\"output\":"); append_json_string(path);
        appendf(",\"id\":"); append_json_string(path);
        appendf(",\"title\":"); append_json_string(title);
        appendf(",\"kind\":\"rockbox\",\"path\":"); append_json_string(path);
        appendf("}");
    }
    closedir(dir);
}

static void append_pocket_packages(void)
{
    if (file_exists(POCKETROCK_DISABLE_APPS)) return;
    const char *directory = ROCKBOX_DIR "/pocketrock/apps";
    DIR *dir = opendir(directory);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && used < sizeof(response) - MAX_PATH - 128) {
        if (!has_suffix(entry->d_name, ".pocket")) continue;
        char path[MAX_PATH], id[192], title[192];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (pocketrock_package_identity(path, id, sizeof(id), title, sizeof(title)) < 0)
            continue;
        if (response[used - 1] != '[') appendf(",");
        appendf("{\"output\":"); append_json_string(id);
        appendf(",\"id\":"); append_json_string(id);
        appendf(",\"title\":"); append_json_string(title);
        appendf(",\"kind\":\"pocket\"}");
    }
    closedir(dir);
}

static const char *app_table(void)
{
    reset_response();
    appendf("{\"apps\":[");
    append_pocket_packages();
    append_native_directory(PLUGIN_APPS_DIR);
    append_native_directory(PLUGIN_GAMES_DIR);
    append_native_directory(PLUGIN_DEMOS_DIR);
    appendf("],\"current\":\"pocketrock-shell\",\"resume\":null}");
    return response;
}

static bool select_package(const char *id)
{
    if (file_exists(POCKETROCK_DISABLE_APPS)) return false;
    const char *directory = ROCKBOX_DIR "/pocketrock/apps";
    DIR *dir = opendir(directory);
    if (!dir) return false;
    struct dirent *entry;
    bool found = false;
    while ((entry = readdir(dir)) != NULL) {
        if (!has_suffix(entry->d_name, ".pocket")) continue;
        char path[MAX_PATH], package_id[192], title[2];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (pocketrock_package_identity(
                path, package_id, sizeof(package_id), title, sizeof(title)) == 0 &&
            !strcmp(package_id, id)) {
            strlcpy(active_package, path, sizeof(active_package));
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

static const char *playback_snapshot(void)
{
    const struct mp3entry *id3 = audio_current_track();
    int status = audio_status();
    reset_response();
    appendf("{\"status\":\"%s\",\"index\":%d,\"path\":",
        status & AUDIO_STATUS_PLAY ?
            (status & AUDIO_STATUS_PAUSE ? "paused" : "playing") : "stopped",
        playlist_next(0));
    if (id3) append_json_string(id3->path); else appendf("null");
    appendf(",\"title\":"); append_json_string(id3 ? id3->title : "");
    appendf(",\"artist\":"); append_json_string(id3 ? id3->artist : "");
    appendf(",\"album\":"); append_json_string(id3 ? id3->album : "");
    appendf(",\"elapsedMs\":%lu,\"durationMs\":%lu,\"volume\":%d,"
        "\"repeat\":\"%d\",\"shuffle\":%s}",
        id3 ? id3->elapsed : 0, id3 ? id3->length : 0,
        global_status.volume, global_settings.repeat_mode,
        global_settings.playlist_shuffle ? "true" : "false");
    return response;
}

static const char *queue_page(const char *payload)
{
    long offset = json_number(payload, "offset", 0);
    long limit = json_number(payload, "limit", 64);
    int total = playlist_amount();
    if (offset < 0) offset = 0;
    if (limit < 1) limit = 1;
    if (limit > 64) limit = 64;
    reset_response();
    appendf("{\"items\":[");
    bool first = true;
    for (long index = offset; index < total && index < offset + limit; ++index) {
        struct playlist_track_info info;
        if (playlist_get_track_info(NULL, index, &info) < 0) continue;
        if (!first) appendf(",");
        first = false;
        appendf("{\"index\":%ld,\"path\":", index);
        append_json_string(info.filename);
        appendf(",\"title\":"); append_json_string(info.filename);
        appendf("}");
    }
    appendf("],\"offset\":%ld,\"total\":%d}", offset, total);
    return response;
}

#ifdef HAVE_TAGCACHE
static int library_tag(const char *payload)
{
    if (strstr(payload, "\"kind\":\"artists\"")) return tag_artist;
    if (strstr(payload, "\"kind\":\"albums\"")) return tag_album;
    if (strstr(payload, "\"kind\":\"tracks\"")) return tag_title;
    return -1;
}

static const char *library_page(const char *payload)
{
    long offset = json_number(payload, "offset", 0);
    long limit = json_number(payload, "limit", 64);
    int tag = library_tag(payload);
    struct tagcache_stat *stat = tagcache_get_stat();
    if (offset < 0) offset = 0;
    if (limit < 1) limit = 1;
    if (limit > 64) limit = 64;
    if (tag < 0)
        return "{\"items\":[],\"offset\":0,\"total\":0}";
    struct tagcache_search search;
    if (!tagcache_search(&search, tag))
        return "{\"error\":{\"code\":\"tagcache.unavailable\",\"message\":\"database unavailable\"}}";
    int total = search.entry_count;
    char item[TAGCACHE_BUFSZ];
    long index = 0, emitted = 0;
    reset_response();
    appendf("{\"items\":[");
    bool first = true;
    while (emitted < limit && tagcache_get_next(&search, item, sizeof(item))) {
        if (index++ < offset) continue;
        if (!first) appendf(",");
        first = false;
        appendf("{\"id\":%ld,\"title\":", index - 1);
        append_json_string(item);
        appendf("}");
        ++emitted;
    }
    tagcache_search_finish(&search);
    appendf("],\"offset\":%ld,\"total\":%d,\"scanning\":%s}",
        offset, total, stat && (!stat->ready || stat->commit_step > 0) ? "true" : "false");
    return response;
}
#endif

static const char *system_snapshot(void)
{
    /* volume_size() may touch storage. Once mass storage is acknowledged the
       host must not access the disk until SYS_USB_DISCONNECTED. */
    if (!pocketrock_usb_active() &&
        (cached_total_sectors == 0 || TIME_AFTER(current_tick, next_storage_refresh))) {
        volume_size(IF_MV(0,) &cached_total_sectors, &cached_free_sectors);
        next_storage_refresh = current_tick + 5 * HZ;
    }
    const char *usb_state = pocketrock_usb_active()
        ? "mass-storage"
        : usb_inserted() ? "connected" : "disconnected";
    reset_response();
    appendf("{\"batteryPercent\":%d,\"batteryMinutes\":%d,"
        "\"charging\":%s,\"freeBytes\":%llu,\"totalBytes\":%llu,"
        "\"backlight\":%s,\"usb\":\"%s\"}",
        battery_level(), battery_time(), charging_state() ? "true" : "false",
        (unsigned long long)cached_free_sectors * SECTOR_SIZE,
        (unsigned long long)cached_total_sectors * SECTOR_SIZE,
        is_backlight_on(false) ? "true" : "false",
        usb_state);
    return response;
}

const char *pocketrock_service_call(
    const char *service, const char *method, const char *payload)
{
    if (!strcmp(service, "playback") && !strcmp(method, "snapshot"))
        return playback_snapshot();
    if (!strcmp(service, "playback")) {
        if (!strcmp(method, "play")) audio_resume();
        else if (!strcmp(method, "pause")) audio_pause();
        else if (!strcmp(method, "toggle")) {
            if (audio_status() & AUDIO_STATUS_PAUSE) audio_resume(); else audio_pause();
        } else if (!strcmp(method, "next")) audio_next();
        else if (!strcmp(method, "previous")) audio_prev();
        else if (!strcmp(method, "seek")) audio_ff_rewind(json_number(payload, "elapsedMs", 0));
        else if (!strcmp(method, "setVolume")) {
            global_status.volume = json_number(payload, "volume", global_status.volume);
            setvol();
        } else if (!strcmp(method, "setShuffle")) {
            global_settings.playlist_shuffle = json_bool(payload, "shuffle", false);
            settings_save();
        } else if (!strcmp(method, "setRepeat")) {
            char repeat[16];
            if (!json_string_value(payload, "repeat", repeat, sizeof(repeat)))
                return "{\"ok\":false}";
            if (!strcmp(repeat, "off")) global_settings.repeat_mode = REPEAT_OFF;
            else if (!strcmp(repeat, "all")) global_settings.repeat_mode = REPEAT_ALL;
            else if (!strcmp(repeat, "one")) global_settings.repeat_mode = REPEAT_ONE;
            else if (!strcmp(repeat, "shuffle")) global_settings.repeat_mode = REPEAT_SHUFFLE;
            else return "{\"ok\":false}";
            audio_flush_and_reload_tracks();
            settings_save();
        } else return "{\"error\":{\"code\":\"method.unknown\",\"message\":\"unsupported playback command\"}}";
        return "{\"ok\":true}";
    }
    if (!strcmp(service, "queue") && !strcmp(method, "page")) return queue_page(payload);
    if (!strcmp(service, "queue") && !strcmp(method, "append"))
        return queue_paths(payload, false) >= 0 ? "{\"ok\":true}" : "{\"ok\":false}";
    if (!strcmp(service, "queue") && !strcmp(method, "replace"))
        return queue_paths(payload, true) >= 0 ? "{\"ok\":true}" : "{\"ok\":false}";
    if (!strcmp(service, "queue") && !strcmp(method, "remove"))
        return playlist_delete(NULL, json_number(payload, "index", -1)) >= 0 ?
            "{\"ok\":true}" : "{\"ok\":false}";
    if (!strcmp(service, "queue") && !strcmp(method, "move"))
        return playlist_move(NULL, json_number(payload, "from", -1),
            json_number(payload, "to", -1)) >= 0 ? "{\"ok\":true}" : "{\"ok\":false}";
    if (!strcmp(service, "queue") && !strcmp(method, "play")) {
        playlist_start(json_number(payload, "index", 0), 0, 0);
        return "{\"ok\":true}";
    }
#ifdef HAVE_TAGCACHE
    if (!strcmp(service, "library") && !strcmp(method, "page")) return library_page(payload);
    if (!strcmp(service, "library") && !strcmp(method, "rescan")) {
        tagcache_start_scan();
        return "{\"ok\":true}";
    }
#endif
    if (!strcmp(service, "system") && !strcmp(method, "snapshot")) return system_snapshot();
    if (!strcmp(service, "system") && !strcmp(method, "setBacklight")) {
        if (json_bool(payload, "enabled", true)) backlight_on(); else backlight_off();
        return "{\"ok\":true}";
    }
    if (!strcmp(service, "system") && !strcmp(method, "powerOff")) {
        pending_exit = POCKETROCK_EXIT_POWEROFF;
        return "{\"ok\":true}";
    }
    if (!strcmp(service, "system") && !strcmp(method, "reboot")) {
        pending_exit = POCKETROCK_EXIT_REBOOT;
        return "{\"ok\":true}";
    }
    if (!strcmp(service, "launcher") && !strcmp(method, "restoreSessionState"))
        return "{\"state\":null}";
    if (!strcmp(service, "launcher") && !strcmp(method, "appTable"))
        return app_table();
    if (!strcmp(service, "launcher") && !strcmp(method, "launchPackage")) {
        char id[192];
        if (!json_string_value(payload, "id", id, sizeof(id)) || !select_package(id))
            return "{\"ok\":false}";
        pending_exit = POCKETROCK_EXIT_PACKAGE;
        return "{\"ok\":true}";
    }
    if (!strcmp(service, "launcher") && !strcmp(method, "exitToSystem")) {
        active_package[0] = '\0';
        pending_exit = POCKETROCK_EXIT_PACKAGE;
        return "{\"ok\":true}";
    }
    if (!strcmp(service, "launcher") && !strcmp(method, "launchNativePlugin")) {
        if (!json_string_value(payload, "path", pending_request.plugin,
                sizeof(pending_request.plugin)) ||
            strncmp(pending_request.plugin, ROCKBOX_DIR "/rocks/",
                strlen(ROCKBOX_DIR "/rocks/")) ||
            !has_suffix(pending_request.plugin, ".rock") ||
            !file_exists(pending_request.plugin))
            return "{\"ok\":false}";
        if (!json_string_value(payload, "parameter", pending_request.parameter,
                sizeof(pending_request.parameter)))
            pending_request.parameter[0] = '\0';
        pending_exit = POCKETROCK_EXIT_NATIVE;
        return "{\"ok\":true}";
    }
    if (!strcmp(service, "launcher")) return "{\"ok\":false}";
    return "{\"error\":{\"code\":\"service.unknown\",\"message\":\"service unavailable\"}}";
}

int pocketrock_service_take_exit(struct pocketrock_request *request)
{
    int result = pending_exit;
    if (result == POCKETROCK_EXIT_NATIVE && request)
        *request = pending_request;
    pending_exit = POCKETROCK_EXIT_SHELL;
    memset(&pending_request, 0, sizeof(pending_request));
    return result;
}

const char *pocketrock_service_active_package(void) { return active_package; }

void pocketrock_service_return_to_shell(void)
{
    active_package[0] = '\0';
    pending_exit = POCKETROCK_EXIT_SHELL;
    memset(&pending_request, 0, sizeof(pending_request));
}

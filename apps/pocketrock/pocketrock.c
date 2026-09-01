#include "config.h"
#include "action.h"
#include "button.h"
#include "core_alloc.h"
#include "file.h"
#include "kernel.h"
#include "lcd.h"
#include "menu.h"
#include "plugin.h"
#include "powermgmt.h"
#include "splash.h"
#include "system.h"
#include "tree.h"
#include "icon.h"
#include "gui/usb_screen.h"
#include "pocketrock.h"
#include "theme.h"

#define POCKETROCK_ARENA_MAX (16u * 1024u * 1024u)
#define POCKETROCK_DIR ROCKBOX_DIR "/pocketrock"
#define POCKETROCK_LOG_DIR POCKETROCK_DIR "/logs"
#define POCKETROCK_LOG POCKETROCK_LOG_DIR "/runtime.log"
#define POCKETROCK_LOG_OLD POCKETROCK_LOG_DIR "/runtime.log.1"
#define POCKETROCK_LOG_LIMIT (64u * 1024u)
#define POCKETROCK_DISABLE_APPS POCKETROCK_DIR "/disable-third-party"

/* PocketJS's pinned runtime supplies strong definitions in PocketRock builds.
   Keeping weak fallbacks makes the firmware adapter independently buildable
   and guarantees a usable native recovery UI if integration is missing. */
#ifndef HAVE_POCKETROCK_RUNTIME
int pocketrock_guest_create(void *arena, size_t size)
{ (void)arena; (void)size; return -1; }
int pocketrock_guest_run(struct pocketrock_request *request)
{ (void)request; return POCKETROCK_EXIT_CRASH; }
void pocketrock_guest_destroy(void) { }
const char *pocketrock_guest_error(void) { return "PocketJS runtime unavailable"; }
#endif

static int arena_handle = -1;

static void log_line(const char *message)
{
    mkdir(POCKETROCK_DIR);
    mkdir(POCKETROCK_LOG_DIR);
    int fd = open(POCKETROCK_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;
    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);
    if (size >= (off_t)POCKETROCK_LOG_LIMIT) {
        remove(POCKETROCK_LOG_OLD);
        rename(POCKETROCK_LOG, POCKETROCK_LOG_OLD);
    }
    fd = open(POCKETROCK_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        write(fd, message, strlen(message));
        write(fd, "\n", 1);
        close(fd);
    }
}

static void release_guest(void)
{
    pocketrock_guest_destroy();
    if (arena_handle >= 0) {
        core_unpin(arena_handle);
        core_free(arena_handle);
        arena_handle = -1;
    }
}

static bool create_guest(void)
{
    size_t available = core_allocatable();
    size_t wanted = available < POCKETROCK_ARENA_MAX ? available : POCKETROCK_ARENA_MAX;
    if (wanted < 1024u * 1024u)
        return false;
    arena_handle = core_alloc(wanted);
    if (arena_handle < 0)
        return false;
    core_pin(arena_handle);
    if (pocketrock_guest_create(core_get_data(arena_handle), wanted) < 0) {
        release_guest();
        return false;
    }
    return true;
}

bool pocketrock_recovery_requested(void)
{
    return (button_status() & BUTTON_MENU) != 0;
}

MENUITEM_STRINGLIST(pocketrock_recovery_menu, "PocketRock recovery", NULL,
    "Disable third-party apps", "Clear recent app", "Native file browser",
    "USB mode", "Restart", "Power off");

static void set_third_party_disabled(void)
{
    mkdir(POCKETROCK_DIR);
    int fd = open(POCKETROCK_DISABLE_APPS, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        write(fd, "disabled\n", 9);
        close(fd);
    }
    pocketrock_service_return_to_shell();
    splash(HZ, "Pocket apps disabled");
}

static void recovery_menu(void)
{
    pocketrock_apply_native_theme(!(audio_status() & AUDIO_STATUS_PLAY));
    while (true) {
        switch (do_menu(&pocketrock_recovery_menu, NULL, NULL, false)) {
        case 0:
            set_third_party_disabled();
            break;
        case 1:
            pocketrock_service_return_to_shell();
            splash(HZ, "Recent app cleared");
            break;
        case 2:
        {
            struct browse_context browse = {
                .dirfilter = SHOW_ALL,
                .title = "Files",
                .icon = Icon_Folder,
                .root = "/",
            };
            rockbox_browse(&browse);
            break;
        }
        case 3:
            gui_usb_screen_run(false, 0);
            break;
        case 4:
            sys_reboot();
            break;
        case 5:
            sys_poweroff();
            break;
        default:
            return;
        }
    }
}

void pocketrock_main(void)
{
    struct pocketrock_request request;
    int crash_count = 0;
    /* Standard Rockbox may have loaded a user theme from config.cfg during
       early boot. PocketRock never inherits it: our fixed compatibility theme
       becomes the native state before recovery or any .rock plugin can draw. */
    pocketrock_apply_native_theme(true);
    if (pocketrock_recovery_requested()) {
        log_line("boot: Menu held; native recovery");
        recovery_menu();
    }
    while (true) {
        memset(&request, 0, sizeof(request));
        if (!create_guest()) {
            log_line("shell: runtime arena/create failed; native recovery");
            splash(HZ * 2, "PocketRock recovery");
            recovery_menu();
        }
        int result = pocketrock_guest_run(&request);
        bool was_package = pocketrock_service_active_package()[0] != '\0';
        release_guest();
        if (result == POCKETROCK_EXIT_REBOOT || result == POCKETROCK_EXIT_POWEROFF) {
            log_line(result == POCKETROCK_EXIT_REBOOT ?
                "system: reboot after JS arena release" :
                "system: poweroff after JS arena release");
            if (result == POCKETROCK_EXIT_REBOOT) sys_reboot();
            else sys_poweroff();
            while (true) sleep(HZ);
        }
        if (result == POCKETROCK_EXIT_NATIVE && request.plugin[0] != '\0') {
            log_line("native plugin: JS arena released");
            pocketrock_apply_native_theme(false);
            plugin_load(request.plugin, request.parameter[0] ? request.parameter : NULL);
            pocketrock_apply_native_theme(false);
            crash_count = 0;
            continue;
        }
        if (result == POCKETROCK_EXIT_CRASH) {
            log_line(was_package ? "app: QuickJS exception" : "shell: QuickJS exception");
            if (pocketrock_guest_error()[0] != '\0')
                log_line(pocketrock_guest_error());
            pocketrock_service_return_to_shell();
            if (was_package) {
                crash_count = 0;
                continue;
            }
            if (++crash_count >= 3) {
                splash(HZ * 2, "PocketRock recovery: 3 crashes");
                recovery_menu();
            }
            continue;
        }
        crash_count = 0;
    }
}

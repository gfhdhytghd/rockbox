#include "config.h"
#include "button.h"
#include "core_alloc.h"
#include "file.h"
#include "kernel.h"
#include "lcd.h"
#include "plugin.h"
#include "powermgmt.h"
#include "system.h"
#include "gui/usb_screen.h"
#include "pocketrock.h"
#include "theme.h"

#ifndef POCKETROCK_ARENA_MIB
#define POCKETROCK_ARENA_MIB 12u
#endif
#define POCKETROCK_ARENA_MAX (POCKETROCK_ARENA_MIB * 1024u * 1024u)
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
size_t pocketrock_guest_heap_peak(void) { return 0; }
size_t pocketrock_guest_heap_size(void) { return 0; }
#endif

static int arena_handle = -1;

static void recovery_message(const char *title, const char *detail)
{
    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    lcd_putsxy(8, 8, "PocketRock recovery");
    lcd_hline(8, LCD_WIDTH - 9, 26);
    lcd_putsxy(8, 42, title);
    if (detail && detail[0] != '\0')
        lcd_putsxy(8, 62, detail);
    lcd_update();
}

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
    char memory[96];
    snprintf(memory, sizeof(memory), "memory: arena=%luKiB heap-peak=%luKiB/%luKiB",
        (unsigned long)(POCKETROCK_ARENA_MAX / 1024u),
        (unsigned long)(pocketrock_guest_heap_peak() / 1024u),
        (unsigned long)(pocketrock_guest_heap_size() / 1024u));
    log_line(memory);
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

static void set_third_party_disabled(void)
{
    mkdir(POCKETROCK_DIR);
    int fd = open(POCKETROCK_DISABLE_APPS, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        write(fd, "disabled\n", 9);
        close(fd);
    }
    pocketrock_service_return_to_shell();
    recovery_message("Third-party apps disabled", "Returning to recovery");
    sleep(HZ);
}

static void recovery_menu(void)
{
    static const char *const entries[] = {
        "Disable third-party apps",
        "Clear recent app",
        "USB mode",
        "Restart",
        "Power off",
    };
    int selected = 0;
    while (true) {
        lcd_clear_display();
        lcd_setfont(FONT_SYSFIXED);
        lcd_putsxy(8, 6, "PocketRock recovery");
        lcd_hline(8, LCD_WIDTH - 9, 23);
        for (int i = 0; i < (int)ARRAYLEN(entries); ++i) {
            int y = 36 + i * 24;
            if (i == selected) {
                lcd_set_foreground(LCD_RGBPACK(35, 120, 212));
                lcd_fillrect(4, y - 4, LCD_WIDTH - 8, 21);
                lcd_set_foreground(LCD_WHITE);
            } else {
                lcd_set_foreground(LCD_BLACK);
            }
            lcd_putsxy(10, y, entries[i]);
        }
        lcd_set_foreground(LCD_BLACK);
        lcd_putsxy(8, LCD_HEIGHT - 18, "Wheel: move  Select: choose");
        lcd_update();
        long button = button_get(true);
        if (button == BUTTON_SCROLL_BACK) {
            if (--selected < 0) selected = ARRAYLEN(entries) - 1;
            continue;
        }
        if (button == BUTTON_SCROLL_FWD) {
            selected = (selected + 1) % ARRAYLEN(entries);
            continue;
        }
        if (button != BUTTON_SELECT)
            continue;
        switch (selected) {
        case 0:
            set_third_party_disabled();
            break;
        case 1:
            pocketrock_service_return_to_shell();
            recovery_message("Recent app cleared", "Returning to recovery");
            sleep(HZ);
            break;
        case 2:
            gui_usb_screen_run(false, 0);
            break;
        case 3:
            sys_reboot();
            break;
        case 4:
            sys_poweroff();
            break;
        }
    }
}

void pocketrock_main(void)
{
    struct pocketrock_request request;
    int crash_count = 0;
    /* Standard Rockbox may have loaded a user theme from config.cfg during
       early boot. PocketRock never inherits it: the fixed compatibility theme
       is used only by recovery and native .rock plugins. The system shell is
       always rendered by PocketJS. */
    pocketrock_apply_native_theme(true);
    if (pocketrock_recovery_requested()) {
        log_line("boot: Menu held; native recovery");
        recovery_menu();
    }
    while (true) {
        memset(&request, 0, sizeof(request));
        if (!create_guest()) {
            log_line("shell: runtime arena/create failed; native recovery");
            recovery_message("QuickJS could not start", "12 MiB arena unavailable");
            sleep(HZ * 2);
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
                recovery_message("QuickJS crashed three times", "Entering recovery");
                sleep(HZ * 2);
                recovery_menu();
            }
            continue;
        }
        crash_count = 0;
    }
}

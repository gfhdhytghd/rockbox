#include "config.h"
#include "backlight.h"
#include "button.h"
#include "kernel.h"
#include "lcd.h"
#include "misc.h"
#include "file.h"
#include "system.h"
#include "string.h"
#include "string-extra.h"
#include "pocketrock.h"
#include "package.h"
#include "pocket_runtime.h"
#include "pocket_spec.h"
#include <tlsf.h>

#if LCD_WIDTH != 320 || LCD_HEIGHT != 240 || LCD_DEPTH != 16
#error PocketRock requires a 320x240 RGB565 framebuffer
#endif

#define POCKETROCK_THREAD_STACK (512u * 1024u)
#define POCKETROCK_ANALOG_CENTER 0x80008000u

extern const unsigned char pocketrock_shell_bytecode[];
extern const unsigned int pocketrock_shell_bytecode_len;
extern const unsigned char pocketrock_shell_pak[];
extern const unsigned int pocketrock_shell_pak_len;

static fb_data display[LCD_WIDTH * LCD_HEIGHT] MEM_ALIGN_ATTR;
static unsigned char *thread_stack;
static void *guest_heap;
static size_t guest_heap_size;
static size_t guest_heap_peak;
static int guest_result;
static bool runtime_ready;
static bool boosted;
static struct pocketrock_request *active_request;
static unsigned char *package_bytecode;
static unsigned char *package_pak;
static char guest_error[256];

void *pocket_host_alloc(size_t size) { return tlsf_malloc(size); }
void *pocket_host_realloc(void *pointer, size_t size) { return tlsf_realloc(pointer, size); }
void pocket_host_free(void *pointer) { tlsf_free(pointer); }
void pocket_host_boot_stage(int stage) { (void)stage; }

static uint32_t input_buttons(int held, int event)
{
    int physical = held | event;
    uint32_t buttons = 0;
    if (physical & BUTTON_SELECT) buttons |= POCKET_BTN_CIRCLE;
    if (physical & BUTTON_MENU) buttons |= POCKET_BTN_TRIANGLE;
    if (physical & BUTTON_LEFT) buttons |= POCKET_BTN_LEFT;
    if (physical & BUTTON_RIGHT) buttons |= POCKET_BTN_RIGHT;
    if (physical & BUTTON_PLAY) buttons |= POCKET_BTN_START;
    if (event & BUTTON_SCROLL_FWD) buttons |= POCKET_BTN_DOWN;
    if (event & BUTTON_SCROLL_BACK) buttons |= POCKET_BTN_UP;
    return buttons;
}

static void set_boost(bool enabled)
{
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (boosted != enabled) {
        if (enabled) trigger_cpu_boost();
        else cancel_cpu_boost();
        boosted = enabled;
    }
#else
    (void)enabled;
#endif
}

static void runtime_thread(void)
{
    int high_load_frames = 0;
    int pending_event = BUTTON_NONE;
    long last_tick = current_tick;
    int frame_credit = HZ;
    guest_result = POCKETROCK_EXIT_CRASH;
    const unsigned char *bytecode = pocketrock_shell_bytecode;
    size_t bytecode_len = pocketrock_shell_bytecode_len;
    const unsigned char *pak = pocketrock_shell_pak;
    size_t pak_len = pocketrock_shell_pak_len;
    const char *package_path = pocketrock_service_active_package();
    if (package_path && *package_path) {
        struct pocketrock_package package;
        if (pocketrock_package_open(package_path, &package) < 0)
            return;
        package_bytecode = tlsf_malloc(package.bytecode.length);
        package_pak = tlsf_malloc(package.pak.length ? package.pak.length : 1);
        if (!package_bytecode || !package_pak)
            return;
        int fd = open(package_path, O_RDONLY);
        if (fd < 0 || lseek(fd, package.bytecode.offset, SEEK_SET) < 0 ||
            read(fd, package_bytecode, package.bytecode.length) != (ssize_t)package.bytecode.length ||
            lseek(fd, package.pak.offset, SEEK_SET) < 0 ||
            read(fd, package_pak, package.pak.length) != (ssize_t)package.pak.length) {
            if (fd >= 0) close(fd);
            return;
        }
        close(fd);
        bytecode = package_bytecode;
        bytecode_len = package.bytecode.length;
        pak = package_pak;
        pak_len = package.pak.length;
    }
    runtime_ready = pocket_runtime_boot_bytecode(
        bytecode, bytecode_len, pak, pak_len,
        LCD_WIDTH, LCD_HEIGHT);
    if (!runtime_ready) {
        strlcpy(guest_error, pocket_runtime_error(), sizeof(guest_error));
        return;
    }
    backlight_on();
    while (true) {
        const int rate = high_load_frames > 0 ? 60 : 30;
        int timeout = (HZ - frame_credit + rate - 1) / rate;
        if (timeout < 1) timeout = 1;
        long event = button_get_w_tmo(timeout > 0 ? timeout : 1);
        long now = current_tick;
        long elapsed = now - last_tick;
        last_tick = now;
        if (elapsed > 0) {
            frame_credit += (int)elapsed * rate;
            if (frame_credit > HZ * 2) frame_credit = HZ * 2;
        }
        if (event != BUTTON_NONE) {
            if ((event & BUTTON_MENU) && (event & BUTTON_REPEAT)) {
                pocketrock_service_return_to_shell();
                guest_result = POCKETROCK_EXIT_SHELL;
                break;
            }
            if (default_event_handler(event) == SYS_USB_CONNECTED) {
                guest_result = POCKETROCK_EXIT_SHELL;
                break;
            }
            pending_event |= (int)event;
            high_load_frames = (event & (BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK)) ? 20 : 4;
            if (frame_credit < HZ) frame_credit = HZ;
        }
        if (frame_credit < HZ)
            continue;
        frame_credit -= HZ;
        set_boost(high_load_frames > 0);
        uint32_t buttons = input_buttons(button_status(), pending_event);
        pending_event = BUTTON_NONE;
        if (!pocket_runtime_frame_analog(
                buttons, POCKETROCK_ANALOG_CENTER, rate == 30 ? 2u : 1u)) {
            strlcpy(guest_error, pocket_runtime_error(), sizeof(guest_error));
            break;
        }
        int requested = pocketrock_service_take_exit(active_request);
        if (requested != POCKETROCK_EXIT_SHELL) {
            guest_result = requested;
            break;
        }
        if (!pocket_runtime_render_rgb565((uint16_t *)display, LCD_WIDTH * LCD_HEIGHT))
            break;
        int damage[4];
        if (pocket_runtime_damage_bounds(damage)) {
            if (damage[0] < 0) damage[0] = 0;
            if (damage[1] < 0) damage[1] = 0;
            if (damage[2] > LCD_WIDTH) damage[2] = LCD_WIDTH;
            if (damage[3] > LCD_HEIGHT) damage[3] = LCD_HEIGHT;
            int width = damage[2] - damage[0], height = damage[3] - damage[1];
            if (width > 0 && height > 0) {
                lcd_bitmap_part(display, damage[0], damage[1], LCD_WIDTH,
                    damage[0], damage[1], width, height);
                lcd_update_rect(damage[0], damage[1], width, height);
                if (high_load_frames == 0)
                    high_load_frames = 2;
            }
        }
        if (high_load_frames > 0)
            --high_load_frames;
    }
    set_boost(false);
}

int pocketrock_guest_create(void *arena, size_t size)
{
    if (!arena || size <= POCKETROCK_THREAD_STACK + 1024u * 1024u)
        return -1;
    thread_stack = arena;
    void *heap = (unsigned char *)arena + POCKETROCK_THREAD_STACK;
    guest_heap = heap;
    guest_heap_size = size - POCKETROCK_THREAD_STACK;
    guest_heap_peak = 0;
    return init_memory_pool(guest_heap_size, heap) == (size_t)-1 ? -1 : 0;
}

int pocketrock_guest_run(struct pocketrock_request *request)
{
    active_request = request;
    guest_result = POCKETROCK_EXIT_CRASH;
    runtime_ready = false;
    guest_error[0] = '\0';
    unsigned int id = create_thread(
        runtime_thread, thread_stack, POCKETROCK_THREAD_STACK, 0, "pocketrock"
        IF_PRIO(, PRIORITY_USER_INTERFACE) IF_COP(, CPU));
    if (id == 0)
        return POCKETROCK_EXIT_CRASH;
    thread_wait(id);
    return guest_result;
}

const char *pocketrock_guest_error(void) { return guest_error; }

size_t pocketrock_guest_heap_peak(void)
{
    if (guest_heap) {
        size_t peak = get_max_size(guest_heap);
        if (peak > guest_heap_peak) guest_heap_peak = peak;
    }
    return guest_heap_peak;
}

size_t pocketrock_guest_heap_size(void) { return guest_heap_size; }

void pocketrock_guest_destroy(void)
{
    set_boost(false);
    if (runtime_ready)
        pocket_runtime_shutdown();
    runtime_ready = false;
    package_bytecode = NULL;
    package_pak = NULL;
    active_request = NULL;
    thread_stack = NULL;
    guest_heap = NULL;
    guest_heap_size = 0;
}

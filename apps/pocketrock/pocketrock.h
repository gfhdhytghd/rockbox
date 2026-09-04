/***************************************************************************
 * PocketRock firmware shell entry point.
 ***************************************************************************/
#ifndef POCKETROCK_H
#define POCKETROCK_H

#include <stdbool.h>
#include "file.h"

enum pocketrock_exit {
    POCKETROCK_EXIT_SHELL = 0,
    POCKETROCK_EXIT_NATIVE = 1,
    POCKETROCK_EXIT_CRASH = 2,
    POCKETROCK_EXIT_PACKAGE = 3,
    POCKETROCK_EXIT_REBOOT = 4,
    POCKETROCK_EXIT_POWEROFF = 5,
};

struct pocketrock_request {
    char plugin[MAX_PATH];
    char parameter[MAX_PATH];
};

bool pocketrock_recovery_requested(void);
void pocketrock_main(void) NORETURN_ATTR;
bool pocketrock_usb_active(void);

int pocketrock_guest_create(void *arena, size_t size);
int pocketrock_guest_run(struct pocketrock_request *request);
void pocketrock_guest_destroy(void);
const char *pocketrock_guest_error(void);
size_t pocketrock_guest_heap_peak(void);
size_t pocketrock_guest_heap_size(void);
int pocketrock_service_take_exit(struct pocketrock_request *request);
const char *pocketrock_service_active_package(void);
void pocketrock_service_return_to_shell(void);

#endif

#ifndef POCKETROCK_PACKAGE_H
#define POCKETROCK_PACKAGE_H

#include <stdint.h>
#include <stddef.h>

#define POCKETROCK_TARGET "rockbox-ip6g"
#define POCKETROCK_HOST_ABI 10u
#define POCKETROCK_PAGE_MAX 64u

enum pocketrock_section_kind {
    POCKETROCK_SECTION_IDENTITY = 1,
    POCKETROCK_SECTION_PLAN = 2,
    POCKETROCK_SECTION_JS = 3,
    POCKETROCK_SECTION_PAK = 4,
    POCKETROCK_SECTION_COVER = 5,
    POCKETROCK_SECTION_BYTECODE = 6,
};

struct pocketrock_section {
    uint32_t offset;
    uint32_t length;
};

struct pocketrock_package {
    struct pocketrock_section identity;
    struct pocketrock_section plan;
    struct pocketrock_section javascript;
    struct pocketrock_section pak;
    struct pocketrock_section bytecode;
    uint64_t variant_hash;
    uint32_t file_size;
};

/* Validates the footer, target, ABI, section bounds, variant hash and plan. */
int pocketrock_package_open(const char *path, struct pocketrock_package *out);
int pocketrock_package_identity(
    const char *path, char *id, size_t id_size, char *title, size_t title_size);

#endif

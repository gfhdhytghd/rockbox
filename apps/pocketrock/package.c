#ifdef POCKETROCK_PACKAGE_TEST
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#else
#include "config.h"
#include "file.h"
#include "string.h"
#endif
#include "package.h"

#define PCKT_MAGIC 0x544b4350u
#define PCKT_VERSION 1u
#define PCKT_HEADER_SIZE 16u
#define PCKT_VARIANT_SIZE 40u
#define PCKT_SECTION_SIZE 16u
#define PCKT_ALIGN 16u
#define PCKT_MAX_VARIANTS 64u
#define PCKT_MAX_SECTIONS 64u
#define PCKT_PLAN_MAX 16384u

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const unsigned char *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static uint64_t fnv_update(uint64_t hash, const unsigned char *p, size_t n)
{
    while (n-- > 0) {
        hash ^= *p++;
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

static int read_at(int fd, uint32_t off, void *dst, size_t length)
{
    return lseek(fd, off, SEEK_SET) == (off_t)off &&
           read(fd, dst, length) == (ssize_t)length ? 0 : -1;
}

static bool range_ok(uint32_t off, uint32_t length, uint32_t file_size)
{
    return off <= file_size && length <= file_size - off &&
           off + length <= file_size - 8u;
}

static int hash_range(int fd, uint32_t off, uint32_t length, uint64_t *hash)
{
    unsigned char buffer[1024];
    if (lseek(fd, off, SEEK_SET) != (off_t)off)
        return -1;
    while (length > 0) {
        size_t count = length > sizeof(buffer) ? sizeof(buffer) : length;
        ssize_t got = read(fd, buffer, count);
        if (got != (ssize_t)count)
            return -1;
        *hash = fnv_update(*hash, buffer, count);
        length -= count;
    }
    return 0;
}

static int validate_plan(int fd, const struct pocketrock_section *plan)
{
    char data[PCKT_PLAN_MAX + 1];
    if (plan->length == 0 || plan->length > PCKT_PLAN_MAX)
        return -1;
    if (read_at(fd, plan->offset, data, plan->length) < 0)
        return -1;
    data[plan->length] = '\0';
    /* canonicalJson is whitespace-free and both facts are independently
       checked against the variant table before QuickJS sees any bytes. */
    return strstr(data, "\"id\":\"rockbox-ip6g\"") != NULL &&
           strstr(data, "\"hostAbi\":10") != NULL ? 0 : -1;
}

int pocketrock_package_open(const char *path, struct pocketrock_package *out)
{
    unsigned char header[PCKT_HEADER_SIZE], variant[PCKT_VARIANT_SIZE];
    unsigned char section[PCKT_SECTION_SIZE], footer[8], buffer[1024];
    uint32_t manifest_len, variant_count, table_off, file_size;
    uint64_t file_hash = UINT64_C(0xcbf29ce484222325);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < (off_t)(PCKT_HEADER_SIZE + 8) || (uint64_t)end > UINT32_MAX)
        goto fail;
    file_size = (uint32_t)end;
    if (read_at(fd, 0, header, sizeof(header)) < 0 ||
        le32(header) != PCKT_MAGIC || le32(header + 4) != PCKT_VERSION)
        goto fail;
    manifest_len = le32(header + 8);
    variant_count = le32(header + 12);
    if (variant_count == 0 || variant_count > PCKT_MAX_VARIANTS ||
        manifest_len > file_size - PCKT_HEADER_SIZE - 8u)
        goto fail;
    table_off = (PCKT_HEADER_SIZE + manifest_len + PCKT_ALIGN - 1u) & ~(PCKT_ALIGN - 1u);
    if (!range_ok(table_off, variant_count * PCKT_VARIANT_SIZE, file_size))
        goto fail;

    if (lseek(fd, 0, SEEK_SET) != 0)
        goto fail;
    uint32_t remain = file_size - 8u;
    while (remain > 0) {
        size_t count = remain > sizeof(buffer) ? sizeof(buffer) : remain;
        if (read(fd, buffer, count) != (ssize_t)count)
            goto fail;
        file_hash = fnv_update(file_hash, buffer, count);
        remain -= count;
    }
    if (read_at(fd, file_size - 8u, footer, sizeof(footer)) < 0 ||
        file_hash != le64(footer))
        goto fail;

    memset(out, 0, sizeof(*out));
    bool found = false;
    for (uint32_t i = 0; i < variant_count; ++i) {
        if (read_at(fd, table_off + i * PCKT_VARIANT_SIZE,
                    variant, sizeof(variant)) < 0)
            goto fail;
        if (memcmp(variant, POCKETROCK_TARGET, sizeof(POCKETROCK_TARGET)) != 0)
            continue;
        uint32_t count = le32(variant + 20);
        uint32_t sections_off = le32(variant + 24);
        if (found || le32(variant + 16) != POCKETROCK_HOST_ABI ||
            count == 0 || count > PCKT_MAX_SECTIONS ||
            !range_ok(sections_off, count * PCKT_SECTION_SIZE, file_size))
            goto fail;
        found = true;
        out->variant_hash = le64(variant + 32);
        uint64_t variant_hash = UINT64_C(0xcbf29ce484222325);
        uint32_t seen = 0;
        for (uint32_t j = 0; j < count; ++j) {
            if (read_at(fd, sections_off + j * PCKT_SECTION_SIZE,
                        section, sizeof(section)) < 0)
                goto fail;
            uint32_t kind = le32(section), off = le32(section + 8), len = le32(section + 12);
            if (kind < 32u && (seen & (1u << kind)) != 0)
                goto fail;
            if (kind < 32u)
                seen |= 1u << kind;
            if (!range_ok(off, len, file_size) || hash_range(fd, off, len, &variant_hash) < 0)
                goto fail;
            struct pocketrock_section value = { off, len };
            switch (kind) {
            case POCKETROCK_SECTION_IDENTITY: out->identity = value; break;
            case POCKETROCK_SECTION_PLAN: out->plan = value; break;
            case POCKETROCK_SECTION_JS: out->javascript = value; break;
            case POCKETROCK_SECTION_PAK: out->pak = value; break;
            case POCKETROCK_SECTION_BYTECODE: out->bytecode = value; break;
            default: break; /* append-only forward compatibility */
            }
        }
        if (variant_hash != out->variant_hash || out->identity.length == 0 ||
            out->plan.length == 0 || out->bytecode.length == 0 ||
            validate_plan(fd, &out->plan) < 0)
            goto fail;
    }
    if (!found)
        goto fail;
    out->file_size = file_size;
    close(fd);
    return 0;
fail:
    close(fd);
    return -1;
}

int pocketrock_package_identity(
    const char *path, char *id, size_t id_size, char *title, size_t title_size)
{
    struct pocketrock_package package;
    unsigned char data[1024];
    if (pocketrock_package_open(path, &package) < 0 ||
        package.identity.length > sizeof(data) || id_size == 0 || title_size == 0)
        return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || read_at(fd, package.identity.offset, data, package.identity.length) < 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    size_t off = 0;
    char *outputs[3] = { NULL, id, title };
    size_t sizes[3] = { 0, id_size, title_size };
    for (int field = 0; field < 3; ++field) {
        if (off + 2 > package.identity.length) return -1;
        size_t length = data[off] | (size_t)data[off + 1] << 8;
        off += 2;
        if (off + length > package.identity.length) return -1;
        if (outputs[field]) {
            size_t copy = length < sizes[field] - 1 ? length : sizes[field] - 1;
            memcpy(outputs[field], data + off, copy);
            outputs[field][copy] = '\0';
        }
        off += length;
    }
    return 0;
}

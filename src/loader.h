#ifndef EXWORD_JVM_LOADER_H
#define EXWORD_JVM_LOADER_H
#include <stddef.h>
#include <stdint.h>

/* The OS/uploader may enforce 8.3; the VM uses the actual JVM this_class
 * name, parsed from class bytes, rather than the filename.
 *
 * At most 128 class files, 96 KiB per class, 256 byte paths/names.
 */
#define JVM_FILES_MAX 128
#define JVM_PATH_MAX 256
#define JVM_CLASSNAME_MAX 192
#define JVM_CLASSFILE_MAX (96 * 1024)

typedef struct {
    char path[JVM_PATH_MAX];
    char internal_name[JVM_CLASSNAME_MAX];
    uint32_t bytes;
    int has_main;
} jvm_file_t;

typedef struct {
    jvm_file_t entries[JVM_FILES_MAX];
    unsigned count;
    char directory[JVM_PATH_MAX];
    char status[128];
} jvm_loader_t;

/* Returns 0 on success, negative on failure.
 * size output is always set to 0 on failure. Buffer belongs to caller. */
int jvm_read_file(const char *path, uint8_t **out, size_t *size);
void jvm_loader_init(jvm_loader_t *loader);
int jvm_loader_scan(jvm_loader_t *loader, const char *directory);
int jvm_loader_autodetect(jvm_loader_t *loader);
int jvm_loader_fetch(jvm_loader_t *loader, const char *internal_name,
                     uint8_t **bytes, size_t *length);
int jvm_loader_find(jvm_loader_t *loader, const char *internal_name);

/* One-pass class header + constant-pool inspection.
 * This validates the magic and extracts this_class and main() information.
 * Full validation happens again in the classfile parser. */
int jvm_peek_class(const uint8_t *data, size_t length,
                   char internal_name[JVM_CLASSNAME_MAX], int *main_method);
#endif

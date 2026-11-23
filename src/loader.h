#ifndef EXWORD_JVM_LOADER_H
#define EXWORD_JVM_LOADER_H
#include <stddef.h>
#include <stdint.h>

/* EXWord native paths can be long. Store filenames once (directory is shared)
 * and allocate each JVM internal name to its actual length.
 * No enormous class index malloc; no 96 KiB read buffer for 1 KiB classes.
 * At most 64 class files, 96 KiB each, 256-byte full paths.
 */
#define JVM_FILES_MAX 64
#define JVM_FILENAME_MAX 24
#define JVM_PATH_MAX 256
#define JVM_CLASSNAME_MAX 192
#define JVM_CLASSFILE_MAX (96 * 1024)

typedef struct {
    char filename[JVM_FILENAME_MAX];
    char *internal_name; /* owned by loader; tiny per-class allocation */
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
/* Initialize zeroed storage, or release names before reusing a loader. */
void jvm_loader_init(jvm_loader_t *loader);
void jvm_loader_destroy(jvm_loader_t *loader);
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

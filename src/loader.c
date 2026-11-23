#include "loader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifndef JVM_HOST
#include <gint/exword.h>
#include <gint/gint.h>
#endif

typedef struct {
    const uint8_t *data;
    size_t size, at;
    int error;
} reader_t;
static unsigned u1(reader_t *r)
{
    if(r->at >= r->size) { r->error = 1; return 0; }
    return r->data[r->at++];
}
static unsigned u2(reader_t *r) { unsigned a = u1(r); return (a << 8) | u1(r); }
static uint32_t u4(reader_t *r)
{
    uint32_t a = u2(r);
    return (a << 16) | u2(r);
}
static void skip(reader_t *r, size_t n)
{
    if(n > r->size - r->at) { r->error = 1; r->at = r->size; }
    else r->at += n;
}
typedef struct {
    const uint8_t *utf;
    uint16_t len, target;
    unsigned char tag;
} mini_cp_t;
static int utf_equal(mini_cp_t *cp, unsigned count, unsigned idx, const char *s)
{
    if(!idx || idx >= count || cp[idx].tag != 1) return 0;
    size_t n = strlen(s);
    return n == cp[idx].len && !memcmp(cp[idx].utf, s, n);
}
static int class_name(mini_cp_t *cp, unsigned count, unsigned idx,
                      char name[JVM_CLASSNAME_MAX])
{
    if(!idx || idx >= count || cp[idx].tag != 7) return -1;
    idx = cp[idx].target;
    if(!idx || idx >= count || cp[idx].tag != 1 ||
       !cp[idx].len || cp[idx].len >= JVM_CLASSNAME_MAX) return -1;
    for(unsigned i = 0; i < cp[idx].len; i++) {
        unsigned char c = cp[idx].utf[i];
        if(c < 0x21 || c > 0x7e || c == '\\') return -1;
    }
    memcpy(name, cp[idx].utf, cp[idx].len);
    name[cp[idx].len] = '\0';
    return 0;
}
static void skip_attributes(reader_t *r)
{
    unsigned n = u2(r);
    for(unsigned i = 0; i < n && !r->error; ++i) {
        (void)u2(r);
        uint32_t len = u4(r);
        skip(r, len);
    }
}
int jvm_peek_class(const uint8_t *data, size_t length,
                   char name[JVM_CLASSNAME_MAX], int *main_method)
{
    reader_t r = {data, length, 0, 0};
    if(main_method) *main_method = 0;
    name[0] = 0;
    if(u4(&r) != 0xcafebabe || r.error) return -1;
    (void)u2(&r); (void)u2(&r); /* minor, major */
    unsigned cp_count = u2(&r);
    if(cp_count < 1 || cp_count > 8192 || r.error) return -2;
    mini_cp_t *cp = calloc(cp_count, sizeof *cp);
    if(!cp) return -3;
    int rc = -4;
    for(unsigned i = 1; i < cp_count && !r.error; i++) {
        unsigned tag = u1(&r);
        cp[i].tag = tag;
        switch(tag) {
        case 1: {
            unsigned len = u2(&r);
            if(len > r.size - r.at) { r.error = 1; break; }
            cp[i].len = len;
            cp[i].utf = r.data + r.at;
            skip(&r, len);
            break;
        }
        case 3: case 4: skip(&r, 4); break;
        case 5: case 6: skip(&r, 8); ++i; break;
        case 7: case 8: case 16: case 19: case 20:
            cp[i].target = u2(&r); break;
        case 9: case 10: case 11: case 12: case 17: case 18:
            skip(&r, 4); break;
        case 15: skip(&r, 3); break;
        default: r.error = 1; break;
        }
    }
    if(r.error) goto out;
    (void)u2(&r); /* access */
    unsigned this_class = u2(&r);
    (void)u2(&r); /* super */
    if(class_name(cp, cp_count, this_class, name) < 0) goto out;
    unsigned interfaces = u2(&r);
    if(interfaces > 1024) goto out;
    skip(&r, (size_t)interfaces * 2);
    unsigned fields = u2(&r);
    if(fields > 8192) goto out;
    for(unsigned i = 0; i < fields && !r.error; ++i) {
        skip(&r, 6);
        skip_attributes(&r);
    }
    unsigned methods = u2(&r);
    if(methods > 8192) goto out;
    for(unsigned i = 0; i < methods && !r.error; ++i) {
        unsigned access = u2(&r);
        unsigned method_name = u2(&r);
        unsigned descriptor = u2(&r);
        if(main_method && (access & 0x0008) &&
           utf_equal(cp, cp_count, method_name, "main") &&
           utf_equal(cp, cp_count, descriptor, "([Ljava/lang/String;)V"))
            *main_method = 1;
        skip_attributes(&r);
    }
    if(r.error) goto out;
    skip_attributes(&r);
    if(!r.error && r.at == r.size) rc = 0;
out:
    free(cp);
    if(rc) name[0] = 0;
    return rc;
}

static int suffix_is_class(const char *s)
{
    size_t n = strlen(s);
    const char *ext = strrchr(s, '.');
    if(!ext || ext == s) return 0;
    char small[8] = {0};
    if(strlen(ext) >= sizeof small) return 0;
    for(unsigned i = 0; ext[i]; ++i) small[i] = (char)tolower((unsigned char)ext[i]);
    (void)n;
    return !strcmp(small, ".cls") || !strcmp(small, ".cla") ||
           !strcmp(small, ".class");
}
static int join(char path[JVM_PATH_MAX], const char *dir, const char *file)
{
    size_t n = strlen(dir), m = strlen(file);
    if(!n || !m || n + 1 + m >= JVM_PATH_MAX ||
       strchr(file, '/') || strchr(file, '\\')) return -1;
    memcpy(path, dir, n);
#ifdef JVM_HOST
    path[n++] = '/';
#else
    path[n++] = '\\';
#endif
    memcpy(path+n, file, m+1);
    return 0;
}
#ifndef JVM_HOST
typedef struct {
    const char *path;
    size_t size;
} size_op_t;

/* Two short world switches: ask the native FS for the actual length first,
 * then allocate only as many bytes as this file needs. Previous code asked
 * malloc() for 96 KiB per read, even for a 5-byte main.txt. */
static int native_size(size_op_t *op)
{
    int fd = exword_sys_open(op->path, 1);
    if(fd < 0) return fd;
    int len = exword_sys_get_filesize(fd);
    int close_rc = exword_sys_close(fd);
    if(len < 0) return len;
    if(close_rc < 0) return close_rc;
    op->size = (size_t)len;
    return 0;
}
typedef struct {
    const char *path;
    uint8_t *out;
    size_t max, size;
} read_op_t;
static int native_read(read_op_t *op)
{
    int fd = exword_sys_open(op->path, 1);
    if(fd < 0) return fd;
    int len = exword_sys_get_filesize(fd);
    if(len < 0 || (size_t)len > op->max) {
        exword_sys_close(fd);
        return -100;
    }
    int at = 0;
    while(at < len) {
        int chunk = len - at;
        if(chunk > 4096) chunk = 4096;
        int n = exword_sys_read(fd, op->out + at, chunk);
        if(n <= 0 || n > chunk) {
            exword_sys_close(fd);
            return -101;
        }
        at += n;
    }
    int rc = exword_sys_close(fd);
    op->size = (size_t)len;
    return rc < 0 ? rc : 0;
}
#else
static int host_read_alloc(const char *path, uint8_t **out, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if(!f) return -1;
    if(fseek(f, 0, SEEK_END)) { fclose(f); return -2; }
    long len = ftell(f);
    if(len < 0 || len > JVM_CLASSFILE_MAX) { fclose(f); return -3; }
    rewind(f);
    uint8_t *data = malloc(len ? (size_t)len : 1);
    if(!data) { fclose(f); return -200; }
    size_t count = fread(data, 1, (size_t)len, f);
    int close_rc = fclose(f);
    if(count != (size_t)len || close_rc) { free(data); return -4; }
    *out = data;
    *size = count;
    return 0;
}
#endif
int jvm_read_file(const char *path, uint8_t **out, size_t *size)
{
    if(!out || !size || !path) return -2;
    *out = NULL;
    *size = 0;
#ifdef JVM_HOST
    return host_read_alloc(path, out, size);
#else
    size_op_t query = {path, 0};
    int rc = gint_world_switch(GINT_CALL(native_size, (void *)&query));
    if(rc < 0) return rc;
    if(query.size > JVM_CLASSFILE_MAX) return -100;

    uint8_t *data = malloc(query.size ? query.size : 1);
    if(!data) return -200;
    read_op_t op = {path, data, query.size, 0};
    rc = gint_world_switch(GINT_CALL(native_read, (void *)&op));
    if(rc < 0) { free(data); return rc; }
    *out = data;
    *size = op.size;
    return 0;
#endif
}
void jvm_loader_init(jvm_loader_t *loader)
{
    memset(loader, 0, sizeof *loader);
}
void jvm_loader_destroy(jvm_loader_t *loader)
{
    unsigned count = loader->count;
    if(count > JVM_FILES_MAX) count = JVM_FILES_MAX;
    for(unsigned i = 0; i < count; ++i)
        free(loader->entries[i].internal_name);
    memset(loader, 0, sizeof *loader);
}
int jvm_loader_find(jvm_loader_t *loader, const char *internal_name)
{
    for(unsigned i = 0; i < loader->count; ++i)
        if(loader->entries[i].internal_name &&
           !strcmp(loader->entries[i].internal_name, internal_name))
            return (int)i;
    return -1;
}
#ifndef JVM_HOST
typedef struct {
    jvm_loader_t *loader;
    const char *directory;
} scan_op_t;

static int native_scan(scan_op_t *op)
{
    char pattern[JVM_PATH_MAX];
    if(join(pattern, op->directory, "*")) return -3;
    int handle = -1;
    char name[512] = {0};
    unsigned long type = 0;
    int rc = exword_sys_findfirst(pattern, &handle, name, &type);
    if(rc == -16 || rc == -1) return 0;
    if(rc < 0) return rc;
    do {
        name[sizeof name - 1] = 0;
        if(type != 5 && suffix_is_class(name) &&
           op->loader->count < JVM_FILES_MAX) {
            jvm_file_t *e = &op->loader->entries[op->loader->count];
            if(strlen(name) < sizeof e->filename) {
                strcpy(e->filename, name);
                ++op->loader->count;
            }
        }
        name[0] = 0; type = 0;
        rc = exword_sys_findnext(handle, name, &type);
    } while(rc >= 0);
    int close_rc = exword_sys_findclose(handle);
    if(rc != -16 && rc != -1) return rc;
    return close_rc < 0 ? close_rc : 0;
}
#else
#include <dirent.h>
static int host_scan(jvm_loader_t *loader, const char *dir)
{
    DIR *d = opendir(dir);
    if(!d) return -1;
    struct dirent *e;
    while((e = readdir(d))) {
        if(loader->count >= JVM_FILES_MAX) break;
        if(!suffix_is_class(e->d_name)) continue;
        jvm_file_t *file = &loader->entries[loader->count];
        if(strlen(e->d_name) < sizeof file->filename) {
            strcpy(file->filename, e->d_name);
            ++loader->count;
        }
    }
    closedir(d);
    return 0;
}
#endif
int jvm_loader_scan(jvm_loader_t *loader, const char *directory)
{
    /* Release the previous directory's short allocated names. The caller
     * provides zero-initialized loader storage on its first call. */
    jvm_loader_destroy(loader);
    if(strlen(directory) >= JVM_PATH_MAX) return -1;
    strcpy(loader->directory, directory);
#ifdef JVM_HOST
    int rc = host_scan(loader, directory);
#else
    scan_op_t op = {loader, directory};
    int rc = gint_world_switch(GINT_CALL(native_scan, (void *)&op));
#endif
    if(rc < 0) {
        snprintf(loader->status, sizeof loader->status,
                 "Scan %.65s: error %d", directory, rc);
        return rc;
    }
    unsigned valid = 0;
    for(unsigned i = 0; i < loader->count; ++i) {
        jvm_file_t *entry = &loader->entries[i];
        char path[JVM_PATH_MAX];
        if(join(path, directory, entry->filename)) continue;
        uint8_t *data = NULL;
        size_t length = 0;
        if(jvm_read_file(path, &data, &length) < 0) continue;
        char actual_name[JVM_CLASSNAME_MAX];
        int is_main = 0;
        int ok = jvm_peek_class(data, length, actual_name, &is_main);
        free(data);
        if(ok) continue;
        int duplicate = 0;
        for(unsigned j = 0; j < valid; ++j) {
            if(!strcmp(loader->entries[j].internal_name, actual_name)) {
                duplicate = 1; break;
            }
        }
        if(duplicate) continue;
        size_t name_len = strlen(actual_name);
        char *copy = malloc(name_len + 1);
        if(!copy) {
            loader->count = valid;
            snprintf(loader->status, sizeof loader->status,
                     "Out of RAM indexing JVM class name");
            return -200;
        }
        memcpy(copy, actual_name, name_len + 1);
        entry->internal_name = copy;
        entry->bytes = length;
        entry->has_main = is_main;
        if(valid != i) loader->entries[valid] = *entry;
        ++valid;
    }
    loader->count = valid;
    snprintf(loader->status, sizeof loader->status, "%u class files in %.65s",
             loader->count, directory);
    return (int)loader->count;
}
int jvm_loader_autodetect(jvm_loader_t *loader)
{
#ifdef JVM_HOST
    return jvm_loader_scan(loader, ".");
#else
    const char *roots[] = {
        "\\drv0\\BTPRF", "\\crd0\\BTPRF", "\\drv0\\Java",
        "\\drv0\\JAVA", "\\drv0", "\\crd0"
    };
    for(unsigned i = 0; i < sizeof roots / sizeof roots[0]; ++i) {
        int n = jvm_loader_scan(loader, roots[i]);
        if(n > 0) return n;
    }
    snprintf(loader->status, sizeof loader->status,
             "No classes found in BTPRF/Java/drive root");
    return 0;
#endif
}
int jvm_loader_fetch(jvm_loader_t *loader, const char *internal_name,
                     uint8_t **bytes, size_t *length)
{
    int index = jvm_loader_find(loader, internal_name);
    if(index < 0) return -1;
    char path[JVM_PATH_MAX];
    if(join(path, loader->directory, loader->entries[index].filename))
        return -3;
    return jvm_read_file(path, bytes, length);
}

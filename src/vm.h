#ifndef EXWORD_JVM_VM_H
#define EXWORD_JVM_VM_H
#include "loader.h"
#include "classfile.h"
#include "btu.h"

#define JVM_MAX_CLASSES 96
#define JVM_MAX_OBJECTS 256
#define JVM_MAX_RECURSION 128
#define JVM_STEP_LIMIT 30000000u

typedef struct jvm_runtime_class jvm_runtime_class_t;
typedef struct jvm_vm jvm_vm_t;
typedef void (*jvm_write_fn)(void *ctx, const char *text, size_t bytes);

typedef struct {
    uint32_t btu_entries, sw_steps, allocations, classes_loaded;
} jvm_vm_stats_t;

struct jvm_runtime_class {
    jvm_class_t file;
    uint32_t *statics;
    uint16_t *field_offsets;
    uint16_t static_words, instance_words;
    uint8_t initialized;
    /* Each method gets an aligned executable copy with a 4-byte lead-in.
     * Unsupported instructions are patched to BREAKPOINT (0xCA).
     */
    uint8_t **exec_code;
};

typedef struct {
    uint8_t kind, array_type;
    jvm_runtime_class_t *klass;
    char *text;
    size_t text_len, text_capacity;
    uint32_t *words; /* instance slots or primitive/reference array storage */
    uint32_t count;
} jvm_object_t;

struct jvm_vm {
    jvm_loader_t *loader;
    btu_t *btu;
    jvm_write_fn output;
    void *output_ctx;
    jvm_runtime_class_t *classes[JVM_MAX_CLASSES];
    unsigned class_count;
    jvm_object_t *objects[JVM_MAX_OBJECTS];
    unsigned object_count;
    unsigned depth;
    unsigned steps;
    int aborted;
    char error[196];
    jvm_vm_stats_t stats;
};

/* An interpreter with optional real SH7305 BTU acceleration. Supports
 * ordinary Java classfile loading and a growing set of Java 8 operations.
 * Unsupported bytecodes/methods return a diagnostic, never fake output. */
void jvm_vm_init(jvm_vm_t *vm, jvm_loader_t *loader, btu_t *btu,
                 jvm_write_fn output, void *ctx);
int jvm_vm_run(jvm_vm_t *vm, const char *main_class);
void jvm_vm_destroy(jvm_vm_t *vm);
#endif

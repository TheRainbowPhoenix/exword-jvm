#ifndef EXWORD_JVM_CLASSFILE_H
#define EXWORD_JVM_CLASSFILE_H
#include <stddef.h>
#include <stdint.h>
#include "loader.h"

/* The parser keeps references into its owned class file buffer.
 * Bytecode, method/field names and constant-pool UTF8 strings are not copied.
 */
typedef struct {
    uint8_t tag;
    uint32_t a, b;
    const uint8_t *utf;
    uint16_t len;
} jvm_cp_t;
typedef struct {
    uint16_t access, name, desc, const_value;
} jvm_field_t;
typedef struct {
    uint16_t start_pc, end_pc, handler_pc, catch_type;
} jvm_handler_t;
typedef struct {
    uint16_t access, name, desc, max_stack, max_locals;
    const uint8_t *code;
    uint32_t code_length;
    jvm_handler_t *handlers;
    uint16_t handler_count;
} jvm_method_t;
typedef struct {
    uint8_t *bytes;       /* ownership transferred to parse on success */
    size_t length;
    uint16_t major, minor, access;
    uint16_t cp_count;
    jvm_cp_t *cp;
    uint16_t fields_count, methods_count;
    jvm_field_t *fields;
    jvm_method_t *methods;
    char name[JVM_CLASSNAME_MAX], super[JVM_CLASSNAME_MAX];
} jvm_class_t;

int jvm_class_parse(jvm_class_t *c, uint8_t *bytes, size_t length);
void jvm_class_free(jvm_class_t *c);
int jvm_utf_eq(const jvm_class_t *c, uint16_t index, const char *literal);
int jvm_utf_copy(const jvm_class_t *c, uint16_t index,
                 char *out, size_t cap);
int jvm_cp_class_name(const jvm_class_t *c, uint16_t index,
                      char out[JVM_CLASSNAME_MAX]);
const jvm_method_t *jvm_find_method(const jvm_class_t *c, const char *name,
                                     const char *desc);
int jvm_cp_resolve_member(const jvm_class_t *c, uint16_t index,
                          char owner[JVM_CLASSNAME_MAX],
                          char *name, size_t name_size,
                          char *desc, size_t desc_size);

#endif

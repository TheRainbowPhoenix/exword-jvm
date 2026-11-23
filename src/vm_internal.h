#ifndef EXWORD_JVM_INTERNAL_H
#define EXWORD_JVM_INTERNAL_H
#include "vm.h"

enum { JVM_OBJ_STRING = 1, JVM_OBJ_BUILDER = 2, JVM_OBJ_ARRAY = 3,
       JVM_OBJ_INSTANCE = 4, JVM_OBJ_PRINT = 5 };
enum { JVM_CALL_STATIC = 0, JVM_CALL_SPECIAL = 1, JVM_CALL_VIRTUAL = 2,
       JVM_CALL_INTERFACE = 3 };

int jvm_fail(jvm_vm_t *vm, const char *format, ...);
jvm_runtime_class_t *jvm_get_class(jvm_vm_t *vm, const char *name);
int jvm_initialize_class(jvm_vm_t *vm, jvm_runtime_class_t *c);
int jvm_call(jvm_vm_t *vm, const char *owner, const char *name,
             const char *desc, int kind,
             const uint32_t *args, unsigned argc,
             uint32_t ret[2], unsigned *retc);
int jvm_native_call(jvm_vm_t *vm, const char *owner, const char *name,
                    const char *desc, int kind,
                    const uint32_t *args, unsigned argc,
                    uint32_t ret[2], unsigned *retc);
int jvm_object_new(jvm_vm_t *vm, int kind, jvm_runtime_class_t *klass);
int jvm_string(jvm_vm_t *vm, const char *utf8, size_t n);
int jvm_concat(jvm_vm_t *vm, unsigned handle, const char *bytes, size_t length);
jvm_object_t *jvm_object_get(jvm_vm_t *vm, uint32_t handle);
int jvm_field_find(jvm_vm_t *vm, jvm_runtime_class_t *c,
                   const char *name, const char *desc, int is_static,
                   jvm_runtime_class_t **owner, unsigned *word, int *wide);
unsigned jvm_descriptor_args(const char *desc, int has_this, int *valid);
unsigned jvm_descriptor_return(const char *desc);
#endif

#include "vm_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

int jvm_fail(jvm_vm_t *vm, const char *format, ...)
{
    if(!vm->error[0]) {
        va_list ap;
        va_start(ap, format);
        vsnprintf(vm->error, sizeof vm->error, format, ap);
        va_end(ap);
    }
    return -1;
}
static int name_is_builtin(const char *name)
{
    return !strncmp(name, "java/", 5);
}
void jvm_vm_init(jvm_vm_t *vm, jvm_loader_t *loader, btu_t *btu,
                 jvm_write_fn output, void *ctx)
{
    memset(vm, 0, sizeof *vm);
    vm->loader = loader;
    vm->btu = btu;
    vm->output = output;
    vm->output_ctx = ctx;
    vm->object_count = 1; /* handle 0 is Java null */
    jvm_object_new(vm, JVM_OBJ_PRINT, NULL); /* System.out handle 1 */
}

jvm_object_t *jvm_object_get(jvm_vm_t *vm, uint32_t handle)
{
    if(handle == 0 || handle >= vm->object_count) return NULL;
    return vm->objects[handle];
}
int jvm_object_new(jvm_vm_t *vm, int kind, jvm_runtime_class_t *klass)
{
    if(vm->object_count >= JVM_MAX_OBJECTS) {
        jvm_fail(vm, "Java heap handles exhausted (no GC yet)");
        return 0;
    }
    jvm_object_t *obj = calloc(1, sizeof *obj);
    if(!obj) {
        jvm_fail(vm, "out of memory (object)");
        return 0;
    }
    obj->kind = kind; obj->klass = klass;
    if(kind == JVM_OBJ_INSTANCE && klass && klass->instance_words) {
        obj->count = klass->instance_words;
        obj->words = calloc(obj->count, sizeof *obj->words);
        if(!obj->words) {
            free(obj); jvm_fail(vm, "out of memory (fields)"); return 0;
        }
    }
    unsigned handle = vm->object_count++;
    vm->objects[handle] = obj;
    ++vm->stats.allocations;
    return (int)handle;
}
int jvm_string(jvm_vm_t *vm, const char *utf8, size_t n)
{
    int h = jvm_object_new(vm, JVM_OBJ_STRING, NULL);
    if(!h) return 0;
    jvm_object_t *o = jvm_object_get(vm, (unsigned)h);
    o->text = malloc(n + 1);
    if(!o->text) { jvm_fail(vm, "out of memory (string)"); return 0; }
    memcpy(o->text, utf8, n); o->text[n] = '\0';
    o->text_len = n; o->text_capacity = n + 1;
    return h;
}
int jvm_concat(jvm_vm_t *vm, unsigned handle, const char *bytes, size_t len)
{
    jvm_object_t *o = jvm_object_get(vm, handle);
    if(!o || o->kind != JVM_OBJ_BUILDER)
        return jvm_fail(vm, "StringBuilder invalid receiver %u", handle);
    if(len > 65536 || o->text_len > 65536 - len)
        return jvm_fail(vm, "StringBuilder capacity exceeded");
    size_t total = o->text_len + len + 1;
    if(total > o->text_capacity) {
        size_t cap = total * 2;
        char *p = realloc(o->text, cap);
        if(!p) return jvm_fail(vm, "StringBuilder allocation failed");
        o->text = p; o->text_capacity = cap;
    }
    memcpy(o->text + o->text_len, bytes, len);
    o->text_len += len;
    o->text[o->text_len] = 0;
    return 0;
}
unsigned jvm_descriptor_args(const char *desc, int has_this, int *valid)
{
    unsigned slots = has_this ? 1 : 0;
    if(desc[0] != '(') { *valid = 0; return 0; }
    const char *p = desc + 1;
    while(*p && *p != ')') {
        if(*p == '[') {
            while(*p == '[') ++p;
            if(*p == 'L') {
                while(*p && *p != ';') ++p;
                if(!*p) { *valid = 0; return 0; }
                ++p;
            } else if(*p) ++p;
            else { *valid = 0; return 0; }
            slots++;
        } else if(*p == 'L') {
            while(*p && *p != ';') ++p;
            if(!*p) { *valid = 0; return 0; }
            ++p; ++slots;
        } else {
            slots += (*p == 'D' || *p == 'J') ? 2 : 1;
            ++p;
        }
        if(slots > 255) { *valid = 0; return 0; }
    }
    *valid = *p == ')';
    return slots;
}
unsigned jvm_descriptor_return(const char *desc)
{
    const char *p = strchr(desc, ')');
    if(!p || !p[1]) return 0;
    if(p[1] == 'V') return 0;
    return p[1] == 'D' || p[1] == 'J' ? 2 : 1;
}

jvm_runtime_class_t *jvm_get_class(jvm_vm_t *vm, const char *name)
{
    for(unsigned i = 0; i < vm->class_count; ++i)
        if(!strcmp(vm->classes[i]->file.name, name))
            return vm->classes[i];
    if(vm->class_count >= JVM_MAX_CLASSES) {
        jvm_fail(vm, "too many classes");
        return NULL;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    if(jvm_loader_fetch(vm->loader, name, &bytes, &length) < 0) {
        jvm_fail(vm, "class not found: %.130s", name);
        return NULL;
    }
    jvm_runtime_class_t *c = calloc(1, sizeof *c);
    if(!c) { free(bytes); jvm_fail(vm, "out of memory (class)"); return NULL; }
    if(jvm_class_parse(&c->file, bytes, length) < 0) {
        free(c); jvm_fail(vm, "invalid classfile: %.120s", name);
        return NULL;
    }
    if(strcmp(c->file.name, name)) {
        jvm_class_free(&c->file); free(c);
        jvm_fail(vm, "classfile name mismatch: %.120s", name);
        return NULL;
    }
    vm->classes[vm->class_count++] = c;
    ++vm->stats.classes_loaded;
    if(c->file.super[0] && !name_is_builtin(c->file.super) &&
       !jvm_get_class(vm, c->file.super)) return NULL;

    c->field_offsets = calloc(c->file.fields_count ?
                              c->file.fields_count : 1, sizeof *c->field_offsets);
    c->exec_code = calloc(c->file.methods_count ?
                          c->file.methods_count : 1, sizeof *c->exec_code);
    if(!c->field_offsets || !c->exec_code) {
        jvm_fail(vm, "out of memory (class descriptors)"); return NULL;
    }
    if(c->file.super[0] && !name_is_builtin(c->file.super)) {
        for(unsigned i = 0; i < vm->class_count; ++i)
            if(!strcmp(vm->classes[i]->file.name, c->file.super)) {
                c->instance_words = vm->classes[i]->instance_words;
                break;
            }
    }
    for(unsigned i = 0; i < c->file.fields_count; ++i) {
        const jvm_field_t *f = &c->file.fields[i];
        char desc[160];
        if(jvm_utf_copy(&c->file, f->desc, desc, sizeof desc)) {
            jvm_fail(vm, "field descriptor too long"); return NULL;
        }
        unsigned width = (desc[0] == 'J' || desc[0] == 'D') ? 2 : 1;
        if(f->access & 8) {
            c->field_offsets[i] = c->static_words;
            c->static_words += width;
        } else {
            c->field_offsets[i] = c->instance_words;
            c->instance_words += width;
        }
    }
    c->statics = calloc(c->static_words ? c->static_words : 1, sizeof *c->statics);
    if(!c->statics) { jvm_fail(vm, "out of memory (statics)"); return NULL; }
    for(unsigned i = 0; i < c->file.fields_count; ++i) {
        const jvm_field_t *f = &c->file.fields[i];
        if(!(f->access & 8) || !f->const_value ||
           f->const_value >= c->file.cp_count) continue;
        const jvm_cp_t *cp = &c->file.cp[f->const_value];
        unsigned off = c->field_offsets[i];
        if(cp->tag == 3 || cp->tag == 4) c->statics[off] = cp->a;
        if((cp->tag == 5 || cp->tag == 6) && off + 1 < c->static_words) {
            c->statics[off] = cp->a;
            c->statics[off + 1] = cp->b;
        }
    }
    return c;
}
int jvm_field_find(jvm_vm_t *vm, jvm_runtime_class_t *c,
                   const char *name, const char *desc, int is_static,
                   jvm_runtime_class_t **owner, unsigned *word, int *wide)
{
    (void)vm;
    for(; c; ) {
        for(unsigned i = 0; i < c->file.fields_count; ++i) {
            const jvm_field_t *f = &c->file.fields[i];
            if(((f->access & 8) != 0) == is_static &&
               jvm_utf_eq(&c->file, f->name, name) &&
               jvm_utf_eq(&c->file, f->desc, desc)) {
                *owner = c; *word = c->field_offsets[i];
                *wide = desc[0] == 'J' || desc[0] == 'D';
                return 0;
            }
        }
        if(!c->file.super[0] || name_is_builtin(c->file.super)) break;
        jvm_runtime_class_t *next = NULL;
        for(unsigned i = 0; i < vm->class_count; ++i)
            if(!strcmp(vm->classes[i]->file.name, c->file.super))
                next = vm->classes[i];
        c = next;
    }
    return -1;
}
int jvm_initialize_class(jvm_vm_t *vm, jvm_runtime_class_t *c)
{
    if(c->initialized) return 0;
    c->initialized = 1; /* also protects recursive <clinit> */
    if(c->file.super[0] && !name_is_builtin(c->file.super)) {
        jvm_runtime_class_t *sup = jvm_get_class(vm, c->file.super);
        if(!sup || jvm_initialize_class(vm, sup)) return -1;
    }
    const jvm_method_t *cl = jvm_find_method(&c->file, "<clinit>", "()V");
    if(!cl) return 0;
    uint32_t result[2] = {0};
    unsigned n = 0;
    return jvm_call(vm, c->file.name, "<clinit>", "()V", JVM_CALL_STATIC,
                    NULL, 0, result, &n);
}
int jvm_vm_run(jvm_vm_t *vm, const char *main_class)
{
    jvm_runtime_class_t *c = jvm_get_class(vm, main_class);
    if(!c) return -1;
    if(!jvm_find_method(&c->file, "main", "([Ljava/lang/String;)V"))
        return jvm_fail(vm, "%.100s has no main(String[])", main_class);
    if(jvm_initialize_class(vm, c)) return -1;
    int arg = jvm_object_new(vm, JVM_OBJ_ARRAY, NULL);
    if(!arg) return -1;
    jvm_object_get(vm, (unsigned)arg)->array_type = 'L';
    uint32_t args[1] = {(uint32_t)arg}, ret[2] = {0};
    unsigned retc = 0;
    return jvm_call(vm, main_class, "main", "([Ljava/lang/String;)V",
                    JVM_CALL_STATIC, args, 1, ret, &retc);
}
void jvm_vm_destroy(jvm_vm_t *vm)
{
    for(unsigned i = 1; i < vm->object_count; ++i) {
        jvm_object_t *o = vm->objects[i];
        if(!o) continue;
        free(o->text); free(o->words); free(o);
    }
    for(unsigned i = 0; i < vm->class_count; ++i) {
        jvm_runtime_class_t *c = vm->classes[i];
        if(!c) continue;
        if(c->exec_code) {
            for(unsigned j = 0; j < c->file.methods_count; ++j)
                free(c->exec_code[j]);
        }
        free(c->exec_code); free(c->field_offsets); free(c->statics);
        jvm_class_free(&c->file); free(c);
    }
}

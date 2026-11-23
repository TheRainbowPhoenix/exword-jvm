#include "vm_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Native builtins are deliberately explicit. Unknown Java library calls
 * are ERRORS; returning success would mask broken classfile programs. */
static const char *str_of(jvm_vm_t *vm, uint32_t ref)
{
    if(!ref) return "null";
    jvm_object_t *obj = jvm_object_get(vm, ref);
    if(!obj) return "<invalid object>";
    if((obj->kind == JVM_OBJ_STRING || obj->kind == JVM_OBJ_BUILDER) && obj->text)
        return obj->text;
    if(obj->kind == JVM_OBJ_PRINT) return "java.io.PrintStream";
    return "<object>";
}
static int stringify(jvm_vm_t *vm, const char *type, const uint32_t *args,
                     char *buf, size_t n)
{
    uint64_t wide;
    switch(*type) {
    case 'Z': snprintf(buf, n, "%s", args[0] ? "true" : "false"); break;
    case 'B': snprintf(buf, n, "%d", (int)(int8_t)args[0]); break;
    case 'C':
        buf[0] = (char)(args[0] & 0x7f);
        buf[1] = 0; break;
    case 'S': snprintf(buf, n, "%d", (int)(int16_t)args[0]); break;
    case 'I': snprintf(buf, n, "%d", (int32_t)args[0]); break;
    case 'J':
        wide = ((uint64_t)args[0] << 32) | args[1];
        snprintf(buf, n, "%lld", (long long)(int64_t)wide);
        break;
    case 'F': {
        float f;
        uint32_t raw = args[0];
        memcpy(&f, &raw, sizeof f);
        snprintf(buf, n, "%.7g", (double)f);
        break;
    }
    case 'D': {
        double d;
        wide = ((uint64_t)args[0] << 32) | args[1];
        memcpy(&d, &wide, sizeof d);
        snprintf(buf, n, "%.16g", d);
        break;
    }
    case 'L': case '[':
        snprintf(buf, n, "%s", str_of(vm, args[0])); break;
    default: return -1;
    }
    return 0;
}
static void emit(jvm_vm_t *vm, const char *s, size_t len)
{
    if(vm->output && s) vm->output(vm->output_ctx, s, len);
}
static int native_print(jvm_vm_t *vm, const char *name, const char *desc,
                        const uint32_t *args, uint32_t out[2], unsigned *outc)
{
    (void)out; *outc = 0;
    char buf[128];
    if(desc[0] != '(') return jvm_fail(vm, "PrintStream descriptor invalid");
    if(desc[1] != ')') {
        if(stringify(vm, desc + 1, args + 1, buf, sizeof buf))
            return jvm_fail(vm, "PrintStream unsupported descriptor %.50s", desc);
        emit(vm, buf, strlen(buf));
    }
    if(!strcmp(name, "println")) emit(vm, "\n", 1);
    return 1;
}
int jvm_native_call(jvm_vm_t *vm, const char *owner, const char *name,
                    const char *desc, int kind, const uint32_t *args,
                    unsigned argc, uint32_t ret[2], unsigned *retc)
{
    (void)kind;
    *retc = 0;
    if(!strcmp(owner, "java/io/PrintStream")) {
        if((!strcmp(name, "println") || !strcmp(name, "print")) && argc >= 1)
            return native_print(vm, name, desc, args, ret, retc);
        return 0;
    }
    if(!strcmp(owner, "java/lang/Object")) {
        if(!strcmp(name, "<init>") && !strcmp(desc, "()V")) return 1;
        if(!strcmp(name, "toString") && !strcmp(desc, "()Ljava/lang/String;")) {
            const char *v = str_of(vm, args[0]);
            int h = jvm_string(vm, v, strlen(v));
            if(!h) return -1;
            ret[0] = h; *retc = 1;
            return 1;
        }
        return 0;
    }
    if(!strcmp(owner, "java/lang/StringBuilder") ||
       !strcmp(owner, "java/lang/StringBuffer")) {
        jvm_object_t *o = argc ? jvm_object_get(vm, args[0]) : NULL;
        if(!o || o->kind != JVM_OBJ_BUILDER)
            return jvm_fail(vm, "StringBuilder receiver invalid: %u", argc ? args[0] : 0);
        if(!strcmp(name, "<init>")) {
            if(!strcmp(desc, "()V")) return 1;
            if(!strcmp(desc, "(Ljava/lang/String;)V") && argc == 2) {
                const char *text = str_of(vm, args[1]);
                return jvm_concat(vm, args[0], text, strlen(text)) ? -1 : 1;
            }
            return 0;
        }
        if(!strcmp(name, "append")) {
            char buf[128];
            if(argc < 2 || stringify(vm, desc + 1, args + 1, buf, sizeof buf))
                return jvm_fail(vm, "StringBuilder append: %.40s", desc);
            if(jvm_concat(vm, args[0], buf, strlen(buf))) return -1;
            ret[0] = args[0]; *retc = 1;
            return 1;
        }
        if(!strcmp(name, "toString") && !strcmp(desc, "()Ljava/lang/String;")) {
            int h = jvm_string(vm, o->text ? o->text : "", o->text_len);
            if(!h) return -1;
            ret[0] = h; *retc = 1;
            return 1;
        }
        if(!strcmp(name, "length") && !strcmp(desc, "()I")) {
            ret[0] = (uint32_t)o->text_len; *retc = 1;
            return 1;
        }
        return 0;
    }
    if(!strcmp(owner, "java/lang/String")) {
        jvm_object_t *o = argc ? jvm_object_get(vm, args[0]) : NULL;
        if(!strcmp(name, "valueOf") && !strncmp(desc, "(", 1)) {
            char buf[128];
            if(stringify(vm, desc + 1, args, buf, sizeof buf))
                return 0;
            int h = jvm_string(vm, buf, strlen(buf));
            if(!h) return -1;
            ret[0] = h; *retc = 1;
            return 1;
        }
        if(!o || o->kind != JVM_OBJ_STRING) return 0;
        if(!strcmp(name, "length") && !strcmp(desc, "()I")) {
            ret[0] = (uint32_t)o->text_len; *retc = 1;
            return 1;
        }
        if(!strcmp(name, "toString") && !strcmp(desc, "()Ljava/lang/String;")) {
            ret[0] = args[0]; *retc = 1;
            return 1;
        }
        if(!strcmp(name, "charAt") && !strcmp(desc, "(I)C")) {
            unsigned n = args[1];
            if(n >= o->text_len) return jvm_fail(vm, "String.charAt out of bounds");
            ret[0] = (unsigned char)o->text[n]; *retc = 1;
            return 1;
        }
        return 0;
    }
    if(!strcmp(owner, "java/lang/System")) {
        if(!strcmp(name, "currentTimeMillis") && !strcmp(desc, "()J")) {
            ret[0] = 0; ret[1] = 0; *retc = 2; return 1;
        }
        if(!strcmp(name, "nanoTime") && !strcmp(desc, "()J")) {
            ret[0] = 0; ret[1] = 0; *retc = 2; return 1;
        }
        return 0;
    }
    if(!strcmp(owner, "java/lang/Math") ||
       !strcmp(owner, "java/lang/StrictMath")) {
        if(!strcmp(desc, "(II)I") && (!strcmp(name, "min") || !strcmp(name, "max"))) {
            ret[0] = !strcmp(name, "min") ?
                (int32_t)args[0] < (int32_t)args[1] ? args[0] : args[1] :
                (int32_t)args[0] > (int32_t)args[1] ? args[0] : args[1];
            *retc = 1; return 1;
        }
        if(!strcmp(name, "abs") && !strcmp(desc, "(I)I")) {
            ret[0] = (int32_t)args[0] < 0 ? 0u-args[0] : args[0];
            *retc = 1; return 1;
        }
        if(!strcmp(desc, "(D)D") && !strcmp(name, "sqrt")) {
            uint64_t bits = ((uint64_t)args[0] << 32) | args[1];
            double d;
            memcpy(&d, &bits, 8);
            d = sqrt(d);
            memcpy(&bits, &d, 8);
            ret[0] = bits >> 32; ret[1] = bits;
            *retc = 2; return 1;
        }
        return 0;
    }
    return 0;
}

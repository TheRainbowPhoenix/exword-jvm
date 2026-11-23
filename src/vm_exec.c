#include "vm_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Java slots are 32-bit big-endian pairs for long/double, as in cpjvm-rs.
 * Each Java method has one contiguous locals + operand stack array, so the
 * same frame can be seen by the SH7305 BTU and the C software interpreter. */
typedef struct {
    jvm_vm_t *vm;
    jvm_runtime_class_t *klass;
    const jvm_method_t *method;
    uint8_t *executable;
    uint32_t *slots;
    unsigned locals, capacity, sp, pc;
    uint32_t result[2];
    unsigned result_count;
    int finished;
} frame_t;

static uint32_t be4(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static int16_t be2s(const uint8_t *p)
{
    return (int16_t)((p[0] << 8) | p[1]);
}
static uint64_t join64(uint32_t hi, uint32_t lo)
{
    return ((uint64_t)hi << 32) | lo;
}
static int valid_frame(frame_t *f, unsigned pop, unsigned push)
{
    if(f->sp < pop || f->sp - pop + push > f->capacity)
        return jvm_fail(f->vm, "JVM operand stack range error PC=%u", f->pc);
    return 0;
}
static int push(frame_t *f, uint32_t v)
{
    if(valid_frame(f, 0, 1)) return -1;
    f->slots[f->locals + f->sp++] = v;
    return 0;
}
static uint32_t pop(frame_t *f)
{
    return f->slots[f->locals + --f->sp];
}
static int push64(frame_t *f, uint64_t v)
{
    if(valid_frame(f, 0, 2)) return -1;
    push(f, (uint32_t)(v >> 32));
    push(f, (uint32_t)v);
    return 0;
}
static uint64_t pop64(frame_t *f)
{
    uint32_t lo = pop(f), hi = pop(f);
    return join64(hi, lo);
}
static uint32_t get_local(frame_t *f, unsigned index)
{
    return f->slots[index];
}
static unsigned read_len(const uint8_t *code, unsigned pc, unsigned n)
{
    uint8_t op = code[pc];
    if(op == 0xaa || op == 0xab) {
        unsigned p = (pc + 4) & ~3u;
        if(p + 8 > n) return 0;
        if(op == 0xaa) {
            if(p + 12 > n) return 0;
            int32_t low = (int32_t)be4(code + p + 4);
            int32_t high = (int32_t)be4(code + p + 8);
            if(high < low || (uint32_t)(high - low) > 16384) return 0;
            return (p - pc) + 12 + 4 * ((uint32_t)(high - low) + 1);
        }
        int32_t count = (int32_t)be4(code + p + 4);
        if(count < 0 || count > 16384) return 0;
        return (p - pc) + 8 + 8 * count;
    }
    if(op == 0xc4) {
        if(pc + 2 > n) return 0;
        return code[pc + 1] == 0x84 ? 6 : 4;
    }
    if(op == 0xc5) return 4;
    if(op == 0xb9 || op == 0xba || op == 0xc8 || op == 0xc9) return 5;
    if(op == 0x10 || op == 0x12 || (op >= 0x15 && op <= 0x19) ||
       (op >= 0x36 && op <= 0x3a) || op == 0xa9 || op == 0xbc)
        return 2;
    if(op == 0x11 || op == 0x13 || op == 0x14 || op == 0x84 ||
       (op >= 0x99 && op <= 0xa8) ||
       (op >= 0xb2 && op <= 0xb8) ||
       op == 0xbb || op == 0xbd || op == 0xc0 || op == 0xc1 ||
       op == 0xc6 || op == 0xc7) return 3;
    return 1;
}
/* Conservative whitelist. In particular do NOT allow hardware array/field
 * access to software object handles (they are not physical addresses).
 * Arithmetic, numeric locals and conditional branches do use real BTU. */
static int btu_safe(unsigned op)
{
    if(op <= 0x11 && op != 0x12 && op != 0x13 && op != 0x14) return 1;
    if(op >= 0x15 && op <= 0x1e) return 1; /* iload and lload */
    if(op >= 0x1f && op <= 0x21) return 1;
    if(op >= 0x36 && op <= 0x41) return 1; /* istore, lstore */
    if(op >= 0x42 && op <= 0x44) return 1;
    if(op >= 0x57 && op <= 0x61) return 1; /* pop, dup, integer/long add */
    if(op >= 0x64 && op <= 0x65) return 1;
    if(op >= 0x68 && op <= 0x69) return 1;
    if(op == 0x6c || op == 0x70 || op == 0x74 || op == 0x75) return 1;
    if(op >= 0x78 && op <= 0x83) return 1;
    if(op == 0x84 || op == 0x85 || op == 0x88) return 1;
    if(op >= 0x91 && op <= 0x94) return 1;
    if(op >= 0x99 && op <= 0xa4) return 1;
    if(op == 0xa7) return 1;
    return 0;
}
#ifndef JVM_HOST
static void cache_sync(void *ptr, unsigned len)
{
    uintptr_t a = (uintptr_t)ptr & ~(uintptr_t)31;
    uintptr_t end = (uintptr_t)ptr + len;
    for(; a < end; a += 32) {
        __asm__ volatile("ocbwb @%0\n\ticbi @%0"
                         : : "r"(a) : "memory");
    }
}
#endif
static int executable(frame_t *f)
{
    jvm_runtime_class_t *c = f->klass;
    unsigned index = (unsigned)(f->method - c->file.methods);
    if(index >= c->file.methods_count) return -1;
    if(!c->exec_code[index]) {
        size_t length = f->method->code_length;
        uint8_t *copy = malloc(length + 8 + 32);
        if(!copy) return jvm_fail(f->vm, "out of memory (exec code)");
        /* malloc alignment at least 4; 4-byte gap before bytecode */
        memset(copy, 0, length + 8 + 32);
        memcpy(copy + 4, f->method->code, length);
        unsigned pc = 0;
        while(pc < length) {
            unsigned width = read_len(f->method->code, pc, length);
            if(!width || width > length - pc) {
                free(copy); return jvm_fail(f->vm, "invalid bytecode length");
            }
            if(!btu_safe(f->method->code[pc]))
                copy[pc + 4] = 0xca; /* escape to C step */
            pc += width;
        }
#ifndef JVM_HOST
        cache_sync(copy, length + 8);
#endif
        c->exec_code[index] = copy;
    }
    f->executable = c->exec_code[index] + 4;
    return 0;
}
static int get_constant(frame_t *f, unsigned idx, int wide)
{
    jvm_class_t *c = &f->klass->file;
    if(idx == 0 || idx >= c->cp_count)
        return jvm_fail(f->vm, "invalid constant pool index %u", idx);
    const jvm_cp_t *cp = &c->cp[idx];
    if(wide && (cp->tag == 5 || cp->tag == 6))
        return push64(f, join64(cp->a, cp->b));
    if(!wide && (cp->tag == 3 || cp->tag == 4))
        return push(f, cp->a);
    if(!wide && cp->tag == 8) {
        unsigned str = cp->a;
        if(str >= c->cp_count || c->cp[str].tag != 1)
            return jvm_fail(f->vm, "bad string constant");
        int handle = jvm_string(f->vm, (const char *)c->cp[str].utf,
                                c->cp[str].len);
        if(!handle) return -1;
        return push(f, (uint32_t)handle);
    }
    return jvm_fail(f->vm, "unsupported ldc constant tag %u", cp->tag);
}
static int invoke(frame_t *f, unsigned opcode, unsigned index)
{
    char owner[JVM_CLASSNAME_MAX], name[160], desc[192];
    if(jvm_cp_resolve_member(&f->klass->file, index, owner, name,
                             sizeof name, desc, sizeof desc))
        return jvm_fail(f->vm, "bad methodref %u", index);
    int valid = 0;
    int kind = opcode == 0xb8 ? JVM_CALL_STATIC :
               opcode == 0xb7 ? JVM_CALL_SPECIAL :
               opcode == 0xb9 ? JVM_CALL_INTERFACE : JVM_CALL_VIRTUAL;
    unsigned argc = jvm_descriptor_args(desc, kind != JVM_CALL_STATIC, &valid);
    if(!valid) return jvm_fail(f->vm, "invalid method descriptor %.90s", desc);
    if(valid_frame(f, argc, 0)) return -1;
    uint32_t *args = f->slots + f->locals + f->sp - argc;
    uint32_t ret[2] = {0}; unsigned retc = 0;
    if(jvm_call(f->vm, owner, name, desc, kind, args, argc, ret, &retc))
        return -1;
    f->sp -= argc;
    if(valid_frame(f, 0, retc)) return -1;
    for(unsigned i = 0; i < retc; ++i) push(f, ret[i]);
    return 0;
}
static int field_access(frame_t *f, unsigned op, unsigned index)
{
    char owner[JVM_CLASSNAME_MAX], name[160], desc[160];
    if(jvm_cp_resolve_member(&f->klass->file, index, owner, name,
                             sizeof name, desc, sizeof desc))
        return jvm_fail(f->vm, "bad fieldref %u", index);
    if(!strcmp(owner, "java/lang/System") && op == 0xb2 &&
       (!strcmp(name, "out") || !strcmp(name, "err")))
        return push(f, 1);
    int is_static = op == 0xb2 || op == 0xb3;
    jvm_runtime_class_t *c = jvm_get_class(f->vm, owner);
    if(!c) return -1;
    if(is_static && jvm_initialize_class(f->vm, c)) return -1;
    jvm_runtime_class_t *holder = NULL;
    unsigned word = 0; int wide = 0;
    if(jvm_field_find(f->vm, c, name, desc, is_static, &holder, &word, &wide))
        return jvm_fail(f->vm, "field missing: %.48s.%.48s", owner, name);
    unsigned w = wide ? 2 : 1;
    if(is_static) {
        if(op == 0xb2) {
            if(valid_frame(f, 0, w)) return -1;
            for(unsigned i = 0; i < w; ++i)
                push(f, holder->statics[word + i]);
        } else {
            if(valid_frame(f, w, 0)) return -1;
            for(unsigned i = 0; i < w; ++i)
                holder->statics[word + w - 1 - i] = pop(f);
        }
    } else {
        if(valid_frame(f, op == 0xb4 ? 1 : w + 1, 0)) return -1;
        uint32_t ref = op == 0xb4 ?
                       f->slots[f->locals + f->sp - 1] :
                       f->slots[f->locals + f->sp - w - 1];
        jvm_object_t *obj = jvm_object_get(f->vm, ref);
        if(!obj || obj->kind != JVM_OBJ_INSTANCE ||
           word + w > obj->count)
            return jvm_fail(f->vm, "invalid field receiver %u", ref);
        if(op == 0xb4) {
            pop(f);
            for(unsigned i = 0; i < w; ++i) push(f, obj->words[word + i]);
        } else {
            for(unsigned i = 0; i < w; ++i)
                obj->words[word + w - 1 - i] = pop(f);
            pop(f);
        }
    }
    return 0;
}
static int arr_access(frame_t *f, unsigned op)
{
    int store = op >= 0x4f && op <= 0x56;
    int wide = op == 0x2f || op == 0x31 || op == 0x50 || op == 0x52;
    unsigned v = wide ? 2 : 1;
    if(valid_frame(f, store ? v + 2 : 2, store ? 0 : v)) return -1;
    uint32_t ref = f->slots[f->locals + f->sp - (store ? v + 2 : 2)];
    uint32_t index = f->slots[f->locals + f->sp - (store ? v + 1 : 1)];
    jvm_object_t *a = jvm_object_get(f->vm, ref);
    if(!a || a->kind != JVM_OBJ_ARRAY || index >= a->count)
        return jvm_fail(f->vm, "invalid array ref %u index %d", ref, (int)index);
    unsigned width = (a->array_type == 'J' || a->array_type == 'D') ? 2 : 1;
    if(width != v) return jvm_fail(f->vm, "array element width mismatch");
    if(store) {
        for(unsigned i = 0; i < v; ++i)
            a->words[index * width + v - 1 - i] = pop(f);
        pop(f); pop(f);
    } else {
        pop(f); pop(f);
        for(unsigned i = 0; i < v; ++i)
            push(f, a->words[index * width + i]);
    }
    return 0;
}

static int step(frame_t *f)
{
    const uint8_t *code = f->method->code;
    unsigned n = f->method->code_length, pc = f->pc;
    if(pc >= n) return jvm_fail(f->vm, "method PC out of range");
    unsigned len = read_len(code, pc, n);
    if(!len || len > n-pc) return jvm_fail(f->vm, "invalid instruction size at %u", pc);
    uint8_t op = code[pc];
    unsigned next = pc + len;
    uint32_t a, b, v;
    uint64_t aw, bw;
    int32_t sa, sb;
    unsigned idx = 0;
    switch(op) {
    case 0x00: break; /* nop */
    case 0x01: if(push(f, 0)) return -1; break;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
    case 0x07: case 0x08:
        if(push(f, op - 3)) return -1; break;
    case 0x09: case 0x0a:
        if(push64(f, op - 9)) return -1; break;
    case 0x0b: case 0x0c: case 0x0d: {
        float x = (float)(op - 0x0b);
        memcpy(&v, &x, 4);
        if(push(f, v)) return -1;
        break;
    }
    case 0x0e: case 0x0f: {
        double x = (double)(op - 0x0e);
        memcpy(&aw, &x, 8);
        if(push64(f, aw)) return -1;
        break;
    }
    case 0x10: if(push(f, (uint32_t)(int32_t)(int8_t)code[pc+1])) return -1; break;
    case 0x11: if(push(f, (uint32_t)(int32_t)be2s(code+pc+1))) return -1; break;
    case 0x12: if(get_constant(f, code[pc+1], 0)) return -1; break;
    case 0x13: if(get_constant(f, (unsigned)((code[pc+1] << 8)|code[pc+2]), 0)) return -1; break;
    case 0x14: if(get_constant(f, (unsigned)((code[pc+1] << 8)|code[pc+2]), 1)) return -1; break;
    case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
    case 0x1a: case 0x1b: case 0x1c: case 0x1d:
    case 0x1e: case 0x1f: case 0x20: case 0x21:
    case 0x22: case 0x23: case 0x24: case 0x25:
    case 0x26: case 0x27: case 0x28: case 0x29:
    case 0x2a: case 0x2b: case 0x2c: case 0x2d: {
        unsigned wide;
        if(op <= 0x19) { idx = code[pc+1]; wide = op == 0x16 || op == 0x18; }
        else {
            unsigned x = op - 0x1a;
            idx = x % 4; wide = x / 4 == 1 || x / 4 == 3;
        }
        if(idx + wide >= f->locals) return jvm_fail(f->vm, "iload index %u", idx);
        if(push(f, get_local(f, idx))) return -1;
        if(wide && push(f, get_local(f, idx + 1))) return -1;
        break;
    }
    case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a:
    case 0x3b: case 0x3c: case 0x3d: case 0x3e:
    case 0x3f: case 0x40: case 0x41: case 0x42:
    case 0x43: case 0x44: case 0x45: case 0x46:
    case 0x47: case 0x48: case 0x49: case 0x4a:
    case 0x4b: case 0x4c: case 0x4d: case 0x4e: {
        unsigned wide;
        if(op <= 0x3a) { idx = code[pc+1]; wide = op == 0x37 || op == 0x39; }
        else {
            unsigned x = op - 0x3b;
            idx = x % 4; wide = x / 4 == 1 || x / 4 == 3;
        }
        if(idx + wide >= f->locals || valid_frame(f, wide ? 2 : 1, 0))
            return jvm_fail(f->vm, "istore index %u", idx);
        if(wide) f->slots[idx+1] = pop(f);
        f->slots[idx] = pop(f);
        break;
    }
    case 0x2e: case 0x2f: case 0x30: case 0x31:
    case 0x32: case 0x33: case 0x34: case 0x35:
    case 0x4f: case 0x50: case 0x51: case 0x52:
    case 0x53: case 0x54: case 0x55: case 0x56:
        if(arr_access(f, op)) return -1;
        break;
    case 0x57: case 0x58: {
        unsigned words = op == 0x57 ? 1 : 2;
        if(valid_frame(f, words, 0)) return -1;
        f->sp -= words; break;
    }
    case 0x59: /* dup */
        if(valid_frame(f, 1, 2)) return -1;
        if(push(f, f->slots[f->locals + f->sp - 1])) return -1;
        break;
    case 0x5a: /* dup_x1 */
        if(valid_frame(f, 2, 3)) return -1;
        a = pop(f); b = pop(f);
        push(f, a); push(f, b); push(f, a); break;
    case 0x5b: /* dup_x2 */
        if(valid_frame(f, 3, 4)) return -1;
        a = pop(f); b = pop(f); v = pop(f);
        push(f, a); push(f, v); push(f, b); push(f, a); break;
    case 0x5c: /* dup2 */
        if(valid_frame(f, 2, 4)) return -1;
        a = f->slots[f->locals + f->sp - 2];
        b = f->slots[f->locals + f->sp - 1];
        push(f, a); push(f, b); break;
    case 0x5f: /* swap */
        if(valid_frame(f, 2, 2)) return -1;
        a = pop(f); b = pop(f); push(f, a); push(f, b); break;
    case 0x60: case 0x64: case 0x68: case 0x6c: case 0x70:
    case 0x78: case 0x7a: case 0x7c: case 0x7e: case 0x80: case 0x82: {
        if(valid_frame(f, 2, 1)) return -1;
        b = pop(f); a = pop(f); sa = (int32_t)a; sb = (int32_t)b;
        switch(op) {
        case 0x60: v = a+b; break;
        case 0x64: v = a-b; break;
        case 0x68: v = a*b; break;
        case 0x6c: case 0x70:
            if(!b) return jvm_fail(f->vm, "ArithmeticException: / by zero");
            if(a == 0x80000000 && b == 0xffffffff) v = op == 0x6c ? a : 0;
            else v = op == 0x6c ? (uint32_t)(sa/sb) : (uint32_t)(sa%sb);
            break;
        case 0x78: v = a << (b & 31); break;
        case 0x7a: v = (uint32_t)(sa >> (b & 31)); break;
        case 0x7c: v = a >> (b & 31); break;
        case 0x7e: v = a & b; break;
        case 0x80: v = a | b; break;
        default: v = a ^ b; break;
        }
        if(push(f, v)) return -1; break;
    }
    case 0x61: case 0x65: case 0x69:
    case 0x7f: case 0x81: case 0x83: {
        if(valid_frame(f, 4, 2)) return -1;
        bw = pop64(f); aw = pop64(f);
        uint64_t result;
        if(op == 0x61) result = aw + bw;
        else if(op == 0x65) result = aw - bw;
        else if(op == 0x69) result = aw * bw;
        else if(op == 0x7f) result = aw & bw;
        else if(op == 0x81) result = aw | bw;
        else result = aw ^ bw;
        if(push64(f, result)) return -1;
        break;
    }
    case 0x74: if(valid_frame(f, 1, 1)) return -1;
        a = pop(f); push(f, 0u - a); break;
    case 0x75: if(valid_frame(f, 2, 2)) return -1;
        aw = pop64(f); push64(f, 0ull - aw); break;
    case 0x84:
        idx = code[pc+1];
        if(idx >= f->locals) return jvm_fail(f->vm, "iinc index");
        f->slots[idx] += (uint32_t)(int32_t)(int8_t)code[pc+2];
        break;
    case 0x85:
        if(valid_frame(f, 1, 2)) return -1;
        sa = (int32_t)pop(f); push64(f, (uint64_t)(int64_t)sa); break;
    case 0x88:
        if(valid_frame(f, 2, 1)) return -1;
        aw = pop64(f); push(f, (uint32_t)aw); break;
    case 0x91: case 0x92: case 0x93:
        if(valid_frame(f, 1, 1)) return -1;
        a = pop(f); v = op == 0x91 ? (uint32_t)(int32_t)(int8_t)a :
                         op == 0x92 ? (uint32_t)(uint16_t)a :
                                      (uint32_t)(int32_t)(int16_t)a;
        push(f, v); break;
    case 0x94:
        if(valid_frame(f, 4, 1)) return -1;
        bw = pop64(f); aw = pop64(f);
        push(f, (int64_t)aw == (int64_t)bw ? 0 :
                (int64_t)aw > (int64_t)bw ? 1 : (uint32_t)-1);
        break;
    case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e:
    case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4:
    case 0xa5: case 0xa6: case 0xc6: case 0xc7: {
        int cmp;
        unsigned two = op >= 0x9f && op <= 0xa6;
        if(valid_frame(f, two ? 2 : 1, 0)) return -1;
        b = two ? pop(f) : 0;
        a = pop(f);
        sa = (int32_t)a; sb = (int32_t)b;
        switch(op) {
        case 0x99: case 0x9f: case 0xa5: case 0xc6: cmp = a == b; break;
        case 0x9a: case 0xa0: case 0xa6: case 0xc7: cmp = a != b; break;
        case 0x9b: case 0xa1: cmp = sa < sb; break;
        case 0x9c: case 0xa2: cmp = sa >= sb; break;
        case 0x9d: case 0xa3: cmp = sa > sb; break;
        default: cmp = sa <= sb; break;
        }
        if(cmp) next = (unsigned)((int32_t)pc + be2s(code+pc+1));
        break;
    }
    case 0xa7: next = (unsigned)((int32_t)pc + be2s(code+pc+1)); break;
    case 0xc8: next = (unsigned)((int32_t)pc + (int32_t)be4(code+pc+1)); break;
    case 0xb1: f->finished = 1; f->result_count = 0; break;
    case 0xac: case 0xae: case 0xb0:
        if(valid_frame(f, 1, 0)) return -1;
        f->result[0] = pop(f); f->result_count = 1; f->finished = 1; break;
    case 0xad: case 0xaf:
        if(valid_frame(f, 2, 0)) return -1;
        f->result[1] = pop(f); f->result[0] = pop(f);
        f->result_count = 2; f->finished = 1; break;
    case 0xb2: case 0xb3: case 0xb4: case 0xb5:
        if(field_access(f, op, (code[pc+1] << 8) | code[pc+2])) return -1;
        break;
    case 0xb6: case 0xb7: case 0xb8: case 0xb9:
        if(invoke(f, op, (code[pc+1] << 8) | code[pc+2])) return -1;
        break;
    case 0xbb: {
        char name[JVM_CLASSNAME_MAX];
        if(jvm_cp_class_name(&f->klass->file,
            (code[pc+1] << 8) | code[pc+2], name))
            return jvm_fail(f->vm, "bad new class");
        int kind = (!strcmp(name, "java/lang/StringBuilder") ||
                    !strcmp(name, "java/lang/StringBuffer"))
                   ? JVM_OBJ_BUILDER : JVM_OBJ_INSTANCE;
        jvm_runtime_class_t *c = NULL;
        if(kind == JVM_OBJ_INSTANCE) {
            c = jvm_get_class(f->vm, name);
            if(!c || jvm_initialize_class(f->vm, c)) return -1;
        }
        int handle = jvm_object_new(f->vm, kind, c);
        if(!handle || push(f, (uint32_t)handle)) return -1;
        break;
    }
    case 0xbc: case 0xbd: {
        if(valid_frame(f, 1, 1)) return -1;
        int32_t count = (int32_t)pop(f);
        if(count < 0 || count > 65536)
            return jvm_fail(f->vm, "NegativeArraySizeException or huge array %d", count);
        int handle = jvm_object_new(f->vm, JVM_OBJ_ARRAY, NULL);
        if(!handle) return -1;
        jvm_object_t *a = jvm_object_get(f->vm, handle);
        static const char types[] = "----ZCFDBSIJ";
        a->array_type = op == 0xbd ? 'L' :
                        code[pc+1] >= 4 && code[pc+1] <= 11 ?
                        types[code[pc+1]] : 0;
        if(!a->array_type)
            return jvm_fail(f->vm, "bad newarray type");
        a->count = (uint32_t)count;
        unsigned width = a->array_type == 'J' || a->array_type == 'D' ? 2 : 1;
        a->words = calloc((size_t)count * width + 1, sizeof(uint32_t));
        if(!a->words) return jvm_fail(f->vm, "out of memory (array)");
        push(f, handle);
        break;
    }
    case 0xbe: {
        if(valid_frame(f, 1, 1)) return -1;
        jvm_object_t *a = jvm_object_get(f->vm, pop(f));
        if(!a || a->kind != JVM_OBJ_ARRAY)
            return jvm_fail(f->vm, "NullPointerException in arraylength");
        push(f, a->count); break;
    }
    case 0xc0: break; /* checkcast: type verification TODO */
    case 0xc1: /* instanceof */
        if(valid_frame(f, 1, 1)) return -1;
        a = pop(f); push(f, a ? 1 : 0); break;
    case 0xbf: return jvm_fail(f->vm, "athrow not implemented (exceptions TODO)");
    case 0xc4: {
        unsigned sub = code[pc+1];
        idx = (code[pc+2] << 8) | code[pc+3];
        if(idx >= f->locals) return jvm_fail(f->vm, "wide local index");
        if(sub == 0x84) {
            f->slots[idx] += (uint32_t)(int32_t)be2s(code+pc+4);
        } else if(sub == 0x15 || sub == 0x19) push(f, f->slots[idx]);
        else if(sub == 0x36 || sub == 0x3a) {
            if(valid_frame(f, 1, 0)) return -1;
            f->slots[idx] = pop(f);
        } else return jvm_fail(f->vm, "unsupported wide opcode %02x", sub);
        break;
    }
    default:
        return jvm_fail(f->vm, "unsupported bytecode %02x PC=%u in %.70s",
                        op, pc, f->klass->file.name);
    }
    if(next >= n && !f->finished)
        return jvm_fail(f->vm, "bytecode PC escaped method at %u", next);
    f->pc = next;
    return 0;
}

int jvm_call(jvm_vm_t *vm, const char *owner, const char *name,
             const char *desc, int kind, const uint32_t *args,
             unsigned argc, uint32_t ret[2], unsigned *retc)
{
    if(vm->depth >= JVM_MAX_RECURSION)
        return jvm_fail(vm, "Java stack overflow (depth %u)", vm->depth);
    /* JVM library native methods run without classfiles. */
    int native = jvm_native_call(vm, owner, name, desc, kind,
                                 args, argc, ret, retc);
    if(native != 0) return native < 0 ? -1 : 0;

    if(kind == JVM_CALL_VIRTUAL || kind == JVM_CALL_INTERFACE) {
        if(argc == 0 || args[0] == 0)
            return jvm_fail(vm, "NullPointerException in invokevirtual");
        jvm_object_t *o = jvm_object_get(vm, args[0]);
        if(o && o->klass) owner = o->klass->file.name;
    }
    jvm_runtime_class_t *c = jvm_get_class(vm, owner);
    if(!c) return -1;
    if(kind == JVM_CALL_STATIC && strcmp(name, "<clinit>") &&
       jvm_initialize_class(vm, c)) return -1;
    const jvm_method_t *method = NULL;
    jvm_runtime_class_t *declaring = c;
    for(;;) {
        method = jvm_find_method(&declaring->file, name, desc);
        if(method) break;
        if(!declaring->file.super[0] ||
           !strncmp(declaring->file.super, "java/", 5)) break;
        declaring = jvm_get_class(vm, declaring->file.super);
        if(!declaring) return -1;
    }
    if(!method) return jvm_fail(vm, "method missing: %.60s.%.45s", owner, name);
    if(!method->code)
        return jvm_fail(vm, "native/abstract method unsupported: %.50s", name);
    if(method->max_locals < argc)
        return jvm_fail(vm, "bad method max_locals %.50s", name);

    frame_t f = {0};
    f.vm = vm; f.klass = declaring; f.method = method;
    f.locals = method->max_locals;
    f.capacity = method->max_stack + 8;
    f.slots = calloc((size_t)f.locals + f.capacity + 4, sizeof(uint32_t));
    if(!f.slots) return jvm_fail(vm, "out of memory (stack frame)");
    if(argc) memcpy(f.slots, args, argc * sizeof(uint32_t));
    ++vm->depth;
    int rc = 0;
    if(vm->btu && executable(&f)) rc = -1;

    while(!rc && !f.finished) {
        if(++vm->steps > JVM_STEP_LIMIT) {
            rc = jvm_fail(vm, "instruction budget exceeded"); break;
        }
#ifndef JVM_HOST
        if(vm->btu && f.executable) {
            btu_context_t ex = btu_execute(vm->btu,
                f.executable + f.pc, f.slots,
                f.slots + f.locals + f.sp);
            ++vm->stats.btu_entries;
            if(ex.kind == BTU_FALLTHROUGH) {
                rc = jvm_fail(vm, "BTU fell through at PC=%u", f.pc);
                break;
            }
            uintptr_t base = (uintptr_t)f.executable;
            if(ex.out_r11 < base || ex.out_r11 - base >= method->code_length) {
                rc = jvm_fail(vm, "BTU exit PC out of range"); break;
            }
            uintptr_t sb = (uintptr_t)(f.slots + f.locals);
            uintptr_t top = ex.out_r13;
            if(top < sb || top - sb > f.capacity * 4 ||
               ((top - sb) & 3u)) {
                rc = jvm_fail(vm, "BTU stack pointer invalid"); break;
            }
            f.pc = ex.out_r11 - base;
            f.sp = (top - sb) / 4;
            if(ex.kind == BTU_EVENT && ex.out_r0 == 0x10)
                continue; /* countdown, resume at this bytecode */
            if(ex.kind != BTU_TRAP && ex.kind != BTU_EVENT) {
                rc = jvm_fail(vm, "unexpected BTU exit"); break;
            }
        }
#endif
        ++vm->stats.sw_steps;
        rc = step(&f);
    }
    --vm->depth;
    if(!rc) {
        *retc = f.result_count;
        for(unsigned i = 0; i < f.result_count; ++i)
            ret[i] = f.result[i];
    }
    free(f.slots);
    return rc;
}

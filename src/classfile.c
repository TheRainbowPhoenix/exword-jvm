#include "classfile.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *bytes;
    size_t length, pos;
    int error;
} rd_t;
static uint8_t read8(rd_t *r)
{
    if(r->pos >= r->length) { r->error = 1; return 0; }
    return r->bytes[r->pos++];
}
static uint16_t read16(rd_t *r)
{
    unsigned a = read8(r);
    return (uint16_t)((a << 8) | read8(r));
}
static uint32_t read32(rd_t *r)
{
    uint32_t a = read16(r);
    return (a << 16) | read16(r);
}
static const uint8_t *take(rd_t *r, size_t n)
{
    if(n > r->length - r->pos) { r->error = 1; return NULL; }
    const uint8_t *result = r->bytes + r->pos;
    r->pos += n;
    return result;
}
static void skip(rd_t *r, size_t n) { (void)take(r, n); }
int jvm_utf_eq(const jvm_class_t *c, uint16_t index, const char *literal)
{
    if(!index || index >= c->cp_count || c->cp[index].tag != 1)
        return 0;
    const jvm_cp_t *cp = &c->cp[index];
    size_t len = strlen(literal);
    return cp->len == len && !memcmp(cp->utf, literal, len);
}
int jvm_utf_copy(const jvm_class_t *c, uint16_t idx,
                 char *out, size_t cap)
{
    if(!idx || idx >= c->cp_count || c->cp[idx].tag != 1 ||
       c->cp[idx].len >= cap || !cap) return -1;
    const jvm_cp_t *cp = &c->cp[idx];
    memcpy(out, cp->utf, cp->len);
    out[cp->len] = 0;
    return 0;
}
int jvm_cp_class_name(const jvm_class_t *c, uint16_t idx,
                      char out[JVM_CLASSNAME_MAX])
{
    if(!idx || idx >= c->cp_count || c->cp[idx].tag != 7)
        return -1;
    return jvm_utf_copy(c, (uint16_t)c->cp[idx].a,
                        out, JVM_CLASSNAME_MAX);
}
static void attributes(rd_t *r, const jvm_class_t *c, uint16_t *constant)
{
    uint16_t n = read16(r);
    for(unsigned i = 0; i < n && !r->error; ++i) {
        uint16_t name = read16(r);
        uint32_t len = read32(r);
        const uint8_t *bytes = take(r, len);
        if(!r->error && constant && jvm_utf_eq(c, name, "ConstantValue") &&
           len == 2)
            *constant = (uint16_t)((bytes[0] << 8) | bytes[1]);
    }
}
static void parse_code(rd_t *parent, uint32_t len, jvm_method_t *m)
{
    const uint8_t *buf = take(parent, len);
    if(parent->error) return;
    rd_t r = {buf, len, 0, 0};
    m->max_stack = read16(&r);
    m->max_locals = read16(&r);
    m->code_length = read32(&r);
    if(m->code_length > 65535) { parent->error = 1; return; }
    m->code = take(&r, m->code_length);
    m->handler_count = read16(&r);
    if(m->handler_count > 4096) { parent->error = 1; return; }
    if(m->handler_count) {
        m->handlers = calloc(m->handler_count, sizeof *m->handlers);
        if(!m->handlers) { parent->error = 1; return; }
    }
    for(unsigned i = 0; i < m->handler_count && !r.error; ++i) {
        jvm_handler_t *h = &m->handlers[i];
        h->start_pc = read16(&r);
        h->end_pc = read16(&r);
        h->handler_pc = read16(&r);
        h->catch_type = read16(&r);
        if(h->end_pc > m->code_length || h->handler_pc >= m->code_length)
            r.error = 1;
    }
    unsigned attributes_count = read16(&r);
    for(unsigned i = 0; i < attributes_count && !r.error; ++i) {
        (void)read16(&r);
        uint32_t n = read32(&r);
        skip(&r, n);
    }
    if(r.error || r.pos != r.length) parent->error = 1;
}
int jvm_class_parse(jvm_class_t *c, uint8_t *bytes, size_t length)
{
    memset(c, 0, sizeof *c);
    c->bytes = bytes; c->length = length;
    rd_t r = {bytes, length, 0, 0};
    if(read32(&r) != 0xcafebabe) goto bad;
    c->minor = read16(&r); c->major = read16(&r);
    c->cp_count = read16(&r);
    if(c->cp_count < 1 || c->cp_count > 8192 || r.error) goto bad;
    c->cp = calloc(c->cp_count, sizeof *c->cp);
    if(!c->cp) goto bad;
    for(unsigned i = 1; i < c->cp_count && !r.error; ++i) {
        jvm_cp_t *cp = &c->cp[i];
        cp->tag = read8(&r);
        switch(cp->tag) {
        case 1:
            cp->len = read16(&r);
            cp->utf = take(&r, cp->len);
            break;
        case 3: case 4:
            cp->a = read32(&r);
            break;
        case 5: case 6:
            cp->a = read32(&r); cp->b = read32(&r);
            ++i; break;
        case 7: case 8: case 16: case 19: case 20:
            cp->a = read16(&r); break;
        case 9: case 10: case 11: case 12: case 17: case 18:
            cp->a = read16(&r); cp->b = read16(&r); break;
        case 15:
            cp->a = read8(&r); cp->b = read16(&r); break;
        default:
            r.error = 1; break;
        }
    }
    if(r.error) goto bad;
    c->access = read16(&r);
    uint16_t this_class = read16(&r);
    uint16_t super_class = read16(&r);
    if(jvm_cp_class_name(c, this_class, c->name)) goto bad;
    if(super_class && jvm_cp_class_name(c, super_class, c->super)) goto bad;
    uint16_t interfaces = read16(&r);
    if(interfaces > 1024) goto bad;
    skip(&r, (size_t)interfaces * 2);
    c->fields_count = read16(&r);
    if(c->fields_count > 4096 || r.error) goto bad;
    if(c->fields_count) {
        c->fields = calloc(c->fields_count, sizeof *c->fields);
        if(!c->fields) goto bad;
    }
    for(unsigned i = 0; i < c->fields_count && !r.error; ++i) {
        jvm_field_t *f = &c->fields[i];
        f->access = read16(&r);
        f->name = read16(&r);
        f->desc = read16(&r);
        attributes(&r, c, &f->const_value);
    }
    c->methods_count = read16(&r);
    if(c->methods_count > 8192 || r.error) goto bad;
    if(c->methods_count) {
        c->methods = calloc(c->methods_count, sizeof *c->methods);
        if(!c->methods) goto bad;
    }
    for(unsigned i = 0; i < c->methods_count && !r.error; ++i) {
        jvm_method_t *m = &c->methods[i];
        m->access = read16(&r);
        m->name = read16(&r);
        m->desc = read16(&r);
        uint16_t attrs = read16(&r);
        for(unsigned j = 0; j < attrs && !r.error; ++j) {
            uint16_t name = read16(&r);
            uint32_t len = read32(&r);
            if(jvm_utf_eq(c, name, "Code"))
                parse_code(&r, len, m);
            else skip(&r, len);
        }
    }
    attributes(&r, c, NULL);
    if(r.error || r.pos != r.length) goto bad;
    return 0;
bad:
    jvm_class_free(c); /* class_parse always takes ownership, including error */
    return -1;
}
void jvm_class_free(jvm_class_t *c)
{
    if(c->methods)
        for(unsigned i = 0; i < c->methods_count; ++i)
            free(c->methods[i].handlers);
    free(c->methods); free(c->fields); free(c->cp); free(c->bytes);
    memset(c, 0, sizeof *c);
}
const jvm_method_t *jvm_find_method(const jvm_class_t *c, const char *name,
                                    const char *desc)
{
    for(unsigned i = 0; i < c->methods_count; ++i) {
        const jvm_method_t *m = &c->methods[i];
        if(jvm_utf_eq(c, m->name, name) &&
           (!desc || jvm_utf_eq(c, m->desc, desc)))
            return m;
    }
    return NULL;
}
int jvm_cp_resolve_member(const jvm_class_t *c, uint16_t idx,
                          char owner[JVM_CLASSNAME_MAX],
                          char *name, size_t name_size,
                          char *desc, size_t desc_size)
{
    if(!idx || idx >= c->cp_count) return -1;
    const jvm_cp_t *cp = &c->cp[idx];
    if(cp->tag != 9 && cp->tag != 10 && cp->tag != 11) return -1;
    if(cp->b >= c->cp_count || c->cp[cp->b].tag != 12) return -1;
    const jvm_cp_t *nt = &c->cp[cp->b];
    return jvm_cp_class_name(c, (uint16_t)cp->a, owner) ||
           jvm_utf_copy(c, (uint16_t)nt->a, name, name_size) ||
           jvm_utf_copy(c, (uint16_t)nt->b, desc, desc_size) ? -1 : 0;
}

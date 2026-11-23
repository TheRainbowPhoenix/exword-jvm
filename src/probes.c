#include "probes.h"
#include <stdint.h>

/* Real Java bytecodes, no synthetic opcode or software interpretation.
 * Cases derived from cpjvm-rs's Hello / Arith / Micro semantics plus its
 * btu_selftest() and exception/event handoff contract. */

/* nop, iconst_1, breakpoint (same as cpjvm-rs btu_selftest) */
static const uint8_t entry[] = {0x00, 0x04, 0xCA};

/* bipush 6; bipush 7; imul; breakpoint => Hello: 6 * 7 = 42 */
static const uint8_t multiply[] = {0x10, 6, 0x10, 7, 0x68, 0xCA};

/* iconst_5; istore_0; iload_0; bipush 7; imul; breakpoint */
static const uint8_t locals[] = {0x08, 0x3B, 0x1A, 0x10, 7, 0x68, 0xCA};

/* int sum=0, i=0; do { sum+=i; ++i; } while(i<10); iload_0; trap
 * Jump at PC 14 has signed bytecode-relative displacement -10 -> PC 4. */
static const uint8_t branch[] = {
    0x03, 0x3B, 0x03, 0x3C,
    0x1A, 0x1B, 0x60, 0x3B, 0x84, 0x01, 0x01,
    0x1B, 0x10, 10, 0xA1, 0xFF, 0xF6, 0x1A, 0xCA
};

/* iconst_m1; bipush 15; iand; iconst_1; ishl => 30 */
static const uint8_t bitwise[] = {0x02, 0x10, 15, 0x7E, 0x04, 0x78, 0xCA};

/* lconst_1; lconst_1; ladd => (high,low)=(0,2) in BTU BE slots */
static const uint8_t wide[] = {0x0A, 0x0A, 0x61, 0xCA};

/* iconst_1; iconst_0; idiv => BTU event 4, original operands intact */
static const uint8_t arithmetic_event[] = {0x04, 0x03, 0x6C, 0xCA};

/* bipush 5; ireturn => software trap at ireturn, not interpreted by BTU */
static const uint8_t software_exit[] = {0x10, 5, 0xAC, 0xCA};

#define CASE(n,code,stop,words,tos) \
    {n, code, sizeof(code), stop, words, tos, 0, 0, 0, BTU_TRAP, 0, 0}

const probe_case_t probe_cases[] = {
    CASE("J_JP entry", entry, 2, 1, 1),
    CASE("Hello 6x7", multiply, 5, 1, 42),
    {"locals", locals, sizeof(locals), 6, 1, 35, 0, 5, 0, BTU_TRAP, 1, 0},
    {"branch loop", branch, sizeof(branch), 18, 1, 45, 0, 45, 0, BTU_TRAP, 1, 0},
    CASE("bit ops", bitwise, 6, 1, 30),
    {"64bit add", wide, sizeof(wide), 3, 2, 2, 0, 0, 0, BTU_TRAP, 0, 1},
    {"div/zero", arithmetic_event, sizeof(arithmetic_event), 2, 2,
     0, 1, 0, 4, BTU_EVENT, 0, 1},
    CASE("SW handoff", software_exit, 2, 1, 5),
};

const unsigned probe_case_count = sizeof(probe_cases) / sizeof(probe_cases[0]);

probe_result_t probe_run(btu_t *b, const probe_case_t *tc)
{
    /* Reserve four local words, with operand stack starting at word four;
     * exactly the pointer convention used in cpjvm-rs. */
    uint32_t slots[32] = {0};
    uint32_t *sp = slots + 4;
    probe_result_t r = {0};
    r.ctx = btu_execute(b, tc->bytes, slots, sp);
    unsigned n = (unsigned)(r.ctx.out_r13 - (uint32_t)(uintptr_t)sp);
    r.top = slots[4 + tc->stack_words - 1];
    r.next_top = tc->stack_words >= 2 ? slots[4 + tc->stack_words - 2] : 0;
    r.local0 = slots[0];
    r.ok =
        r.ctx.kind == tc->kind &&
        r.ctx.out_r11 == (uint32_t)(uintptr_t)(tc->bytes + tc->stop_pc) &&
        n == 4u * tc->stack_words &&
        r.ctx.out_r0 == tc->event_code &&
        r.top == tc->top &&
        (!tc->check_next_top || r.next_top == tc->next_top) &&
        (!tc->check_local0 || r.local0 == tc->local0);
    return r;
}

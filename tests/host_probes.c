/* Portable fixture check only. This deliberately does NOT prove hardware BTU.
 * It verifies that the C test bytecodes and expected outcomes are coherent.
 * Hardware proof comes exclusively from the EXWord J_JP trampoline. */
#include "probes.h"
#include <stdint.h>
#include <stdio.h>

btu_context_t btu_execute(btu_t *unused, const uint8_t *code,
                          uint32_t *locals, uint32_t *sp)
{
    (void)unused;
    btu_context_t r = {0};
    const uint8_t *pc = code;
    uint32_t *top = sp;
    for(unsigned budget = 0; budget < 1024; ++budget) {
        uint32_t a, b;
        int16_t displacement;
        switch(*pc) {
        case 0x00: ++pc; break;                           /* nop */
        case 0x02: *top++ = UINT32_MAX; ++pc; break;        /* iconst_m1 */
        case 0x03: *top++ = 0; ++pc; break;
        case 0x04: *top++ = 1; ++pc; break;
        case 0x08: *top++ = 5; ++pc; break;
        case 0x0A: *top++ = 0; *top++ = 1; ++pc; break;    /* lconst_1 */
        case 0x10: *top++ = (uint32_t)(int32_t)(int8_t)pc[1]; pc += 2; break;
        case 0x1A: *top++ = locals[0]; ++pc; break;        /* iload_0 */
        case 0x1B: *top++ = locals[1]; ++pc; break;
        case 0x3B: locals[0] = *--top; ++pc; break;
        case 0x3C: locals[1] = *--top; ++pc; break;
        case 0x60: b = *--top; a = *--top; *top++ = a + b; ++pc; break;
        case 0x61: {                                     /* ladd, hi/lo slots */
            uint64_t rhs = ((uint64_t)top[-2] << 32) | top[-1];
            uint64_t lhs = ((uint64_t)top[-4] << 32) | top[-3];
            uint64_t sum = lhs + rhs;
            top -= 4;
            *top++ = (uint32_t)(sum >> 32);
            *top++ = (uint32_t)sum;
            ++pc;
            break;
        }
        case 0x68: b = *--top; a = *--top; *top++ = a * b; ++pc; break;
        case 0x6C:                                       /* divide by zero event */
            if(top[-1] == 0) {
                r.kind = BTU_EVENT;
                r.out_r0 = 4;
                goto done;
            }
            b = *--top; a = *--top; *top++ = (uint32_t)((int32_t)a / (int32_t)b);
            ++pc;
            break;
        case 0x78: b = *--top; a = *--top; *top++ = a << (b & 31); ++pc; break;
        case 0x7E: b = *--top; a = *--top; *top++ = a & b; ++pc; break;
        case 0x84: locals[pc[1]] += (uint32_t)(int32_t)(int8_t)pc[2]; pc += 3; break;
        case 0xA1:
            displacement = (int16_t)(((unsigned)pc[1] << 8) | pc[2]);
            b = *--top; a = *--top;
            pc += (int32_t)a < (int32_t)b ? displacement : 3;
            break;
        case 0xAC: case 0xCA:                            /* software trap */
            r.kind = BTU_TRAP;
            r.out_r0 = 0;
            goto done;
        default:
            fprintf(stderr, "unimplemented fixture opcode %02x\n", *pc);
            r.kind = BTU_FALLTHROUGH;
            goto done;
        }
    }
    fprintf(stderr, "fixture instruction budget exceeded\n");
    r.kind = BTU_FALLTHROUGH;
done:
    r.out_r11 = (uint32_t)(uintptr_t)pc;
    r.out_r13 = (uint32_t)(uintptr_t)top;
    return r;
}

int main(void)
{
    unsigned passed = 0;
    btu_t dummy = {0};
    for(unsigned i = 0; i < probe_case_count; ++i) {
        probe_result_t r = probe_run(&dummy, &probe_cases[i]);
        printf("%s: %s\n", probe_cases[i].name, r.ok ? "PASS" : "FAIL");
        passed += (unsigned)r.ok;
    }
    printf("Fixture simulation: %u/%u (NOT hardware)\n",
           passed, probe_case_count);
    return passed == probe_case_count ? 0 : 1;
}

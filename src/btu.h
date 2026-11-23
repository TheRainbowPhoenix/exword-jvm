#ifndef EXWORD_BTU_H
#define EXWORD_BTU_H

#include <stdint.h>

/* SH7305 BTU register layout and J_JP contract from cpjvm-rs/core/src/btu.rs.
 * This is a physical-device driver, NOT a software emulation of the BTU. */
enum { BTU_TRAP = 0, BTU_EVENT = 1, BTU_FALLTHROUGH = 2 };

typedef struct {
    uint32_t entry;      /*  0: bytecode address (R1) */
    uint32_t locals;     /*  4: R14 */
    uint32_t sp;         /*  8: R13, next-free word */
    uint32_t table;      /* 12: R12, &handler[1] */
    uint32_t r10;        /* 16: zero at entry */
    uint32_t out_r0;     /* 20: operand or event code */
    uint32_t out_r11;    /* 24: Java PC on exit */
    uint32_t out_r13;    /* 28: stack pointer on exit */
    uint32_t kind;       /* 32: trap, event or fallthrough */
} btu_context_t;

typedef struct {
    uint32_t original_mstpcr2;
    uint32_t original_regs[10];
    uint32_t handlers[257];   /* event at index 0; opcode b at index b+1 */
    btu_context_t ctx;
    int active;
} btu_t;

/* IMPORTANT: may access privileged/unmapped MMIO on an unsupported EXWord.
 * Call only after explicit user confirmation, not automatically at boot. */
int btu_start(btu_t *btu);
void btu_stop(btu_t *btu);
btu_context_t btu_execute(btu_t *btu, const uint8_t *code,
                          uint32_t *locals, uint32_t *sp);
uint32_t btu_control(void);

#endif

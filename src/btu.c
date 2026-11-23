#include "btu.h"
#include <stddef.h>
#include <stdint.h>

#define BTU_BASE  0xFF280000u
#define MSTPCR2   0xA4150038u
#define BTU_ON    0xC0000040u /* enable, countdown, force jsr/ret to software */
#define COUNTDOWN 200000u

/* Read/write only naturally aligned 32-bit MMIO registers (SH7305). */
static uint32_t rd(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}
static void wr(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

static const uint8_t offsets[10] = {0, 4, 8, 12, 16, 20, 24, 28, 68, 72};

/* These symbols are implemented by the SH assembly trampoline. */
extern void btu_entry(btu_context_t *ctx);
extern void btu_trap(void);
extern void btu_event(void);

uint32_t btu_control(void)
{
    return rd(BTU_BASE);
}

void btu_stop(btu_t *b)
{
    if(!b->active)
        return;
    /* Disable the engine before restoring the user's register state. */
    wr(BTU_BASE, 0);
    for(unsigned i = 1; i < 10; ++i)
        wr(BTU_BASE + offsets[i], b->original_regs[i]);
    wr(BTU_BASE, b->original_regs[0]);
    wr(MSTPCR2, b->original_mstpcr2);
    b->active = 0;
}

int btu_start(btu_t *b)
{
    b->active = 0;
    b->original_mstpcr2 = rd(MSTPCR2);
    /* SH7305 MSTPCR2[31] gates the BTU clock. */
    wr(MSTPCR2, b->original_mstpcr2 & ~(1u << 31));
    for(unsigned i = 0; i < 10; ++i)
        b->original_regs[i] = rd(BTU_BASE + offsets[i]);
    b->active = 1;

    for(unsigned i = 0; i < 257; ++i)
        b->handlers[i] = (uint32_t)(uintptr_t)btu_trap;
    b->handlers[0] = (uint32_t)(uintptr_t)btu_event;

    /* Same register mapping and object layout as cpjvm-rs:
     * +04 = EDCBA -> R14 locals, R13 stack, R12 exits, R11 Java PC.
     * +0C=2, +10=1 -> array payload +8 / length +4 (word offsets). */
    wr(BTU_BASE + 0x00, 0);
    wr(BTU_BASE + 0x04, 0x000EDCBAu);
    wr(BTU_BASE + 0x08, 0x7198u);
    wr(BTU_BASE + 0x0C, 2);
    wr(BTU_BASE + 0x10, 1);
    wr(BTU_BASE + 0x14, 0);
    wr(BTU_BASE + 0x44, 0x0000FFCCu);
    wr(BTU_BASE + 0x48, 0xCFCDCECBu);
    wr(BTU_BASE + 0x18, COUNTDOWN); /* also reloads +1C */
    wr(BTU_BASE + 0x00, BTU_ON);

    if(rd(BTU_BASE) != BTU_ON ||
       rd(BTU_BASE + 0x04) != 0x000EDCBAu ||
       rd(BTU_BASE + 0x0C) != 2) {
        btu_stop(b);
        return 0;
    }
    return 1;
}

btu_context_t btu_execute(btu_t *b, const uint8_t *code,
                          uint32_t *locals, uint32_t *sp)
{
    b->ctx.entry = (uint32_t)(uintptr_t)code;
    b->ctx.locals = (uint32_t)(uintptr_t)locals;
    b->ctx.sp = (uint32_t)(uintptr_t)sp;
    b->ctx.table = (uint32_t)(uintptr_t)&b->handlers[1];
    b->ctx.r10 = 0;
    b->ctx.out_r0 = b->ctx.out_r11 = b->ctx.out_r13 = 0;
    b->ctx.kind = BTU_FALLTHROUGH;
    btu_entry(&b->ctx);
    return b->ctx;
}

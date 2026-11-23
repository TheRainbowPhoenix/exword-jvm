/* Port of cpjvm-rs/core/src/btu.rs global_asm! J_JP trampoline.
 * Executed in privileged SH4A mode. Interrupts are masked only while in BTU
 * bytecode mode; SR, GBR, PR and callee-saved registers are restored.
 * The raw opcode 0x413B is J_JP @R1 (not supported by ordinary SH assemblers).
 */
    .text
    .balign 4
    .global btu_entry
    .global _btu_entry
btu_entry:
_btu_entry:
    mov.l r8,@-r15
    mov.l r9,@-r15
    mov.l r10,@-r15
    mov.l r11,@-r15
    mov.l r12,@-r15
    mov.l r13,@-r15
    mov.l r14,@-r15
    sts.l pr,@-r15
    stc.l gbr,@-r15
    stc sr,r0
    mov.l r0,@-r15
    ldc r4,gbr
    or #0xF0,r0          /* mask IRQs in bytecode mode */
    mov #1,r1
    shll8 r1
    shll2 r1
    shll2 r1            /* SR.DSP = 0x1000 */
    or r1,r0
    ldc r0,sr
    mov.l @(4,r4),r14
    mov.l @(8,r4),r13
    mov.l @(12,r4),r12
    mov.l @(16,r4),r10
    mov.l @r4,r1
    .word 0x413B        /* J_JP @R1, runs real BTU bytecodes */
    nop
    bra 3f
    mov #2,r0           /* unexpected fallthrough: BTU not running */

    .balign 4
    .global btu_trap
    .global _btu_trap
btu_trap:
_btu_trap:
    mov.l r0,@(20,gbr)
    mov r11,r0
    mov.l r0,@(24,gbr)
    mov r13,r0
    mov.l r0,@(28,gbr)
    bra 3f
    mov #0,r0

    .balign 4
    .global btu_event
    .global _btu_event
btu_event:
_btu_event:
    mov.l r0,@(20,gbr)
    mov r11,r0
    mov.l r0,@(24,gbr)
    mov r13,r0
    mov.l r0,@(28,gbr)
    mov #1,r0
3:
    mov.l r0,@(32,gbr)
    mov.l @r15+,r0
    ldc r0,sr
    ldc.l @r15+,gbr
    lds.l @r15+,pr
    mov.l @r15+,r14
    mov.l @r15+,r13
    mov.l @r15+,r12
    mov.l @r15+,r11
    mov.l @r15+,r10
    mov.l @r15+,r9
    mov.l @r15+,r8
    rts
    nop

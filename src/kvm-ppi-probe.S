/* EL2 pre-kernel diagnostic. IRQ/FIQ remain masked throughout. */
.text
.global _start

.macro report label, reg
    adr x0, \label
    bl puts
    mov x0, \reg
    bl hex
.endm

_start:
    msr daifset, #0xf
    movz x19, #0xf8b0, lsl #16       // PL011
    movz x20, #0xf105, lsl #16       // CPU0 GICR_SGI
    mrs x21, CurrentEL
    report msg_el, x21
    cmp x21, #8
    b.ne start_linux

    sub x24, x20, #0x10, lsl #12     // CPU0 GICR_RD
    ldr x25, [x24, #8]               // GICR_TYPER
    report msg_rdist, x25
    ldr w25, [x24, #0x14]            // GICR_WAKER
    report msg_waker, x25
    ldr w25, [x20, #0x80]            // GICR_IGROUPR0
    report msg_group, x25
    ldr w25, [x20, #0xc04]           // GICR_ICFGR1
    report msg_config, x25

    mrs x21, ICC_SRE_EL2
    report msg_sre, x21
    tbz x21, #0, probe_timer
    mrs x23, ICH_HCR_EL2
    mrs x25, ICH_ELRSR_EL2
    report msg_lrs, x25
    ldr w25, [x20, #0x200]           // GICR_ISPENDR0
    report msg_maint_before, x25
    mov x0, #9                      // En | NPIE: no pending list entry
    msr ICH_HCR_EL2, x0
    bl settle
    mrs x26, ICH_MISR_EL2
    ldr w25, [x20, #0x200]
    msr ICH_HCR_EL2, x23
    bl settle
    ldr w27, [x20, #0x200]
    report msg_maint_status, x26
    report msg_maint_active, x25
    report msg_maint_after, x27

probe_timer:
    mrs x0, ID_AA64MMFR1_EL1
    ubfx x21, x0, #8, #4             // VH
    report msg_vhe, x21
    cbz x21, start_linux
    mrs x21, CNTHV_CTL_EL2
    mrs x22, CNTHV_CVAL_EL2
    msr CNTHV_CTL_EL2, xzr
    bl settle
    ldr w25, [x20, #0x200]
    report msg_timer_before, x25
    mov x0, #-1                     // Set an already-expired compare value.
    msr CNTHV_TVAL_EL2, x0
    mov x0, #1                      // ENABLE | unmasked
    msr CNTHV_CTL_EL2, x0
    bl settle
    mrs x26, CNTHV_CTL_EL2
    ldr w25, [x20, #0x200]
    msr CNTHV_CTL_EL2, xzr
    bl settle
    ldr w27, [x20, #0x200]
    msr CNTHV_CVAL_EL2, x22
    msr CNTHV_CTL_EL2, x21
    isb
    report msg_timer_status, x26
    report msg_timer_active, x25
    report msg_timer_after, x27

start_linux:
    movz x0, #0x1000
    movk x0, #0x01fe, lsl #16
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    movz x4, #0x0200, lsl #16
    br x4

settle:
    dsb sy
    isb
    mrs x9, CNTPCT_EL0
    mov x11, #24000
1:  mrs x10, CNTPCT_EL0
    sub x10, x10, x9
    cmp x10, x11
    b.lo 1b
    ret

puts:
1:  ldrb w3, [x0], #1
    cbz w3, 3f
2:  ldr w4, [x19, #0x18]
    tbnz w4, #5, 2b
    str w3, [x19]
    b 1b
3:  ret

hex:
    mov x2, x0
    mov x6, #60
1:  lsr x3, x2, x6
    and w3, w3, #15
    add w4, w3, #('a' - 10)
    add w5, w3, #'0'
    cmp w3, #10
    csel w3, w5, w4, lo
2:  ldr w4, [x19, #0x18]
    tbnz w4, #5, 2b
    str w3, [x19]
    subs x6, x6, #4
    b.ge 1b
    adr x0, newline
    b puts

newline:          .asciz "\r\n"
msg_el:           .asciz "[ppi] CurrentEL="
msg_rdist:        .asciz "[ppi] GICR_TYPER="
msg_waker:        .asciz "[ppi] GICR_WAKER="
msg_group:        .asciz "[ppi] GICR_IGROUPR0="
msg_config:       .asciz "[ppi] GICR_ICFGR1="
msg_sre:          .asciz "[ppi] ICC_SRE_EL2="
msg_lrs:          .asciz "[ppi] ICH_ELRSR_EL2="
msg_maint_status: .asciz "[ppi] ICH_MISR_EL2="
msg_maint_before: .asciz "[ppi] maintenance-before="
msg_maint_active: .asciz "[ppi] maintenance-active="
msg_maint_after:  .asciz "[ppi] maintenance-after="
msg_vhe:          .asciz "[ppi] VHE="
msg_timer_status: .asciz "[ppi] CNTHV_CTL_EL2="
msg_timer_before: .asciz "[ppi] hyp-virt-before="
msg_timer_active: .asciz "[ppi] hyp-virt-active="
msg_timer_after:  .asciz "[ppi] hyp-virt-after="

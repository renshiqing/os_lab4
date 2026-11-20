#include <riscv.h>
.text
.align 2
.globl lab4_check_switch_registers
lab4_check_switch_registers:
    addi sp, sp, -160
    STORE ra, 0(sp)
    STORE s0, 8(sp)
    STORE s1, 16(sp)
    STORE s2, 24(sp)
    STORE s3, 32(sp)
    STORE s4, 40(sp)
    STORE s5, 48(sp)
    STORE s6, 56(sp)
    STORE s7, 64(sp)
    STORE s8, 72(sp)
    STORE s9, 80(sp)
    STORE s10, 88(sp)
    STORE s11, 96(sp)
    STORE a0, 112(sp)
    STORE a1, 120(sp)
    li t0, 0x2412396
    STORE t0, 128(sp)
    li s0, 256
    add s0, s0, a0
    li s1, 257
    add s1, s1, a0
    li s2, 258
    add s2, s2, a0
    li s3, 259
    add s3, s3, a0
    li s4, 260
    add s4, s4, a0
    li s5, 261
    add s5, s5, a0
    li s6, 262
    add s6, s6, a0
    li s7, 263
    add s7, s7, a0
    li s8, 264
    add s8, s8, a0
    li s9, 265
    add s9, s9, a0
    li s10, 266
    add s10, s10, a0
    li s11, 267
    add s11, s11, a0
1:
    LOAD a0, 112(sp)
    call lab4_worker_yield
    LOAD t1, 112(sp)
    li t0, 256
    add t0, t0, t1
    bne s0, t0, 3f
    li t0, 257
    add t0, t0, t1
    bne s1, t0, 3f
    li t0, 258
    add t0, t0, t1
    bne s2, t0, 3f
    li t0, 259
    add t0, t0, t1
    bne s3, t0, 3f
    li t0, 260
    add t0, t0, t1
    bne s4, t0, 3f
    li t0, 261
    add t0, t0, t1
    bne s5, t0, 3f
    li t0, 262
    add t0, t0, t1
    bne s6, t0, 3f
    li t0, 263
    add t0, t0, t1
    bne s7, t0, 3f
    li t0, 264
    add t0, t0, t1
    bne s8, t0, 3f
    li t0, 265
    add t0, t0, t1
    bne s9, t0, 3f
    li t0, 266
    add t0, t0, t1
    bne s10, t0, 3f
    li t0, 267
    add t0, t0, t1
    bne s11, t0, 3f
    LOAD t0, 128(sp)
    li t1, 0x2412396
    bne t0, t1, 3f
    LOAD t0, 120(sp)
    addi t0, t0, -1
    STORE t0, 120(sp)
    bnez t0, 1b
    li a0, 0
    j 4f
3:
    li a0, -1
4:
    LOAD ra, 0(sp)
    LOAD s0, 8(sp)
    LOAD s1, 16(sp)
    LOAD s2, 24(sp)
    LOAD s3, 32(sp)
    LOAD s4, 40(sp)
    LOAD s5, 48(sp)
    LOAD s6, 56(sp)
    LOAD s7, 64(sp)
    LOAD s8, 72(sp)
    LOAD s9, 80(sp)
    LOAD s10, 88(sp)
    LOAD s11, 96(sp)
    addi sp, sp, 160
    ret

    .syntax unified
    .text
    .align 2
    .global _start
    .type _start, %function
_start:
    mov r7, sp
    ldr r1, [r7]
    add r2, r7, #4
    ldr r0, =main
    mov r3, #0
    sub sp, sp, #16
    str r3, [sp, #0]
    str r3, [sp, #4]
    str r7, [sp, #8]
    bl __libc_start_main
1:  b 1b

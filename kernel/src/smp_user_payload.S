.section .rodata
.code64
.align 16

.global smp_user_payload_start
.global smp_user_payload_end
.type smp_user_payload_start, @object

smp_user_payload_start:
    /* Initial RDI contains the benchmark iteration count. */
    movq %rdi, %rcx

    /* Prove that ring 3 can use the normal LatterOS syscall gate. */
    leaq 4f(%rip), %rdi
    movq $(5f - 4f), %rsi
    movq $1, %rax
    int $0x80

    movq $3, %rax
    int $0x80
    movq %rax, %r12

    movq $0x9E3779B97F4A7C15, %r8

1:
    testq %rcx, %rcx
    jz 3f

    movq %r8, %rax
    shrq $12, %rax
    xorq %rax, %r8
    movq %r8, %rax
    shlq $25, %rax
    xorq %rax, %r8
    movq %r8, %rax
    shrq $27, %rax
    xorq %rax, %r8
    imulq $0x27D4EB2D, %r8, %r8

    decq %rcx
    testq $0x3FFF, %rcx
    jne 1b

    /* Cooperative yield syscall; timer preemption also remains active. */
    movq $10, %rax
    int $0x80
    jmp 1b

3:
    xorq %rdi, %rdi
    movq $2, %rax
    int $0x80

    ud2

4:
    .ascii "LatterOS AP ring-3 process online\n"
5:

smp_user_payload_end:
.size smp_user_payload_start, . - smp_user_payload_start

.section .note.GNU-stack,"",@progbits

.section .rodata.user_program,"a",@progbits
.code64

.global user_hello_start
.global user_hello_end
.type user_hello_start, @object

user_hello_start:
    movw $0x1B, %ax
    movw %ax, %ds
    movw %ax, %es

    leaq user_hello_message(%rip), %rdi
    movq $(user_hello_message_end - user_hello_message), %rsi
    movq $1, %rax
    int $0x80

    xorl %edi, %edi
    movq $2, %rax
    int $0x80

1:
    jmp 1b

user_hello_message:
    .ascii "Hello from a ring 3 LatterOS program.\n"
user_hello_message_end:

user_hello_end:
.size user_hello_start, . - user_hello_start

.global user_spin_start
.global user_spin_end
.type user_spin_start, @object

user_spin_start:
    movw $0x1B, %ax
    movw %ax, %ds
    movw %ax, %es

    leaq user_spin_message(%rip), %rdi
    movq $(user_spin_message_end - user_spin_message), %rsi
    movq $1, %rax
    int $0x80

2:
    pause
    jmp 2b

user_spin_message:
    .ascii "Ring 3 spin process started. Use ps and kill PID.\n"
user_spin_message_end:

user_spin_end:
.size user_spin_start, . - user_spin_start

.section .note.GNU-stack,"",@progbits

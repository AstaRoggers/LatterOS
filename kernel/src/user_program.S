#include "user_runtime.inc"

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
    call user_hello_runtime_console_write

    xorl %edi, %edi
    call user_hello_runtime_exit

user_hello_message:
    .ascii "Hello from a ring 3 LatterOS program.\n"
user_hello_message_end:

LATTER_RUNTIME user_hello_runtime

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
    call user_spin_runtime_console_write

1:
    pause
    jmp 1b

user_spin_message:
    .ascii "Ring 3 spin process started. Use ps and kill PID.\n"
user_spin_message_end:

LATTER_RUNTIME user_spin_runtime

user_spin_end:
.size user_spin_start, . - user_spin_start

.global user_apitest_start
.global user_apitest_end
.type user_apitest_start, @object

user_apitest_start:
    movw $0x1B, %ax
    movw %ax, %ds
    movw %ax, %es

    leaq user_apitest_start_message(%rip), %rdi
    movq $(user_apitest_start_message_end - user_apitest_start_message), %rsi
    call user_apitest_runtime_console_write

    call user_apitest_runtime_getpid
    testq %rax, %rax
    js user_apitest_failure

    leaq user_apitest_file_path(%rip), %rdi
    movq $(LATTER_OPEN_WRITE | LATTER_OPEN_CREATE | LATTER_OPEN_TRUNCATE), %rsi
    call user_apitest_runtime_file_open
    testq %rax, %rax
    js user_apitest_failure

    movq %rax, %r12

    movq %r12, %rdi
    leaq user_apitest_file_message(%rip), %rsi
    movq $(user_apitest_file_message_end - user_apitest_file_message), %rdx
    call user_apitest_runtime_file_write
    cmpq $(user_apitest_file_message_end - user_apitest_file_message), %rax
    jne user_apitest_failure_close_write

    movq %r12, %rdi
    call user_apitest_runtime_file_close
    testq %rax, %rax
    js user_apitest_failure

    leaq user_apitest_file_path(%rip), %rdi
    movq $LATTER_OPEN_READ, %rsi
    call user_apitest_runtime_file_open
    testq %rax, %rax
    js user_apitest_failure

    movq %rax, %r12

    subq $128, %rsp

    movq %r12, %rdi
    movq %rsp, %rsi
    movq $128, %rdx
    call user_apitest_runtime_file_read
    testq %rax, %rax
    jle user_apitest_failure_read_buffer

    movq %rsp, %rdi
    movq %rax, %rsi
    call user_apitest_runtime_console_write

    addq $128, %rsp

    movq %r12, %rdi
    call user_apitest_runtime_file_close
    testq %rax, %rax
    js user_apitest_failure

    leaq user_apitest_spin_path(%rip), %rdi
    call user_apitest_runtime_spawn
    testq %rax, %rax
    js user_apitest_failure

    movq %rax, %r12

    call user_apitest_runtime_yield
    call user_apitest_runtime_yield
    call user_apitest_runtime_yield

    movq %r12, %rdi
    call user_apitest_runtime_kill
    testq %rax, %rax
    js user_apitest_failure

    leaq user_apitest_success_message(%rip), %rdi
    movq $(user_apitest_success_message_end - user_apitest_success_message), %rsi
    call user_apitest_runtime_console_write

    xorl %edi, %edi
    call user_apitest_runtime_exit

user_apitest_failure_read_buffer:
    addq $128, %rsp

    movq %r12, %rdi
    call user_apitest_runtime_file_close
    jmp user_apitest_failure

user_apitest_failure_close_write:
    movq %r12, %rdi
    call user_apitest_runtime_file_close

user_apitest_failure:
    leaq user_apitest_failure_message(%rip), %rdi
    movq $(user_apitest_failure_message_end - user_apitest_failure_message), %rsi
    call user_apitest_runtime_console_write

    movl $1, %edi
    call user_apitest_runtime_exit

user_apitest_start_message:
    .ascii "User-space runtime API test started.\n"
user_apitest_start_message_end:

user_apitest_file_path:
    .asciz "/home/user-api.txt"

user_apitest_spin_path:
    .asciz "/bin/spin"

user_apitest_file_message:
    .ascii "File API from ring 3 works.\n"
user_apitest_file_message_end:

user_apitest_success_message:
    .ascii "User runtime: file and process APIs passed.\n"
user_apitest_success_message_end:

user_apitest_failure_message:
    .ascii "User runtime API test failed.\n"
user_apitest_failure_message_end:

LATTER_RUNTIME user_apitest_runtime

user_apitest_end:
.size user_apitest_start, . - user_apitest_start

.section .note.GNU-stack,"",@progbits

.section .rodata.user_security,"a",@progbits
.code64

.global user_sectest_start
.global user_sectest_end
.type user_sectest_start, @object

user_sectest_start:
    movw $0x1B, %ax
    movw %ax, %ds
    movw %ax, %es

    /* Kernel pointers must be rejected at the syscall boundary. */
    movabsq $0xFFFFFFFF80000000, %rdi
    movq $8, %rsi
    call user_sectest_runtime_console_write
    cmpq $-1, %rax
    jne user_sectest_failure

    /* Create a small file using a readable code-page buffer. */
    leaq user_sectest_path(%rip), %rdi
    movq $(LATTER_OPEN_WRITE | LATTER_OPEN_CREATE | LATTER_OPEN_TRUNCATE), %rsi
    call user_sectest_runtime_file_open
    testq %rax, %rax
    js user_sectest_failure

    movq %rax, %r12
    movq %r12, %rdi
    leaq user_sectest_data(%rip), %rsi
    movq $(user_sectest_data_end - user_sectest_data), %rdx
    call user_sectest_runtime_file_write
    cmpq $(user_sectest_data_end - user_sectest_data), %rax
    jne user_sectest_failure_close

    movq %r12, %rdi
    call user_sectest_runtime_file_close
    testq %rax, %rax
    js user_sectest_failure

    /* Reading into the read-only executable page must be rejected. */
    leaq user_sectest_path(%rip), %rdi
    movq $LATTER_OPEN_READ, %rsi
    call user_sectest_runtime_file_open
    testq %rax, %rax
    js user_sectest_failure

    movq %rax, %r12
    movq %r12, %rdi
    leaq user_sectest_readonly_target(%rip), %rsi
    movq $4, %rdx
    call user_sectest_runtime_file_read
    cmpq $-1, %rax
    jne user_sectest_failure_close

    movq %r12, %rdi
    call user_sectest_runtime_file_close
    testq %rax, %rax
    js user_sectest_failure

    /* User processes cannot signal the kernel process. */
    xorq %rdi, %rdi
    call user_sectest_runtime_kill
    cmpq $-1, %rax
    jne user_sectest_failure

    leaq user_sectest_success(%rip), %rdi
    movq $(user_sectest_success_end - user_sectest_success), %rsi
    call user_sectest_runtime_console_write

    xorl %edi, %edi
    call user_sectest_runtime_exit

user_sectest_failure_close:
    movq %r12, %rdi
    call user_sectest_runtime_file_close

user_sectest_failure:
    leaq user_sectest_failure_message(%rip), %rdi
    movq $(user_sectest_failure_message_end - user_sectest_failure_message), %rsi
    call user_sectest_runtime_console_write

    movl $1, %edi
    call user_sectest_runtime_exit

user_sectest_path:
    .asciz "/tmp/security.txt"

user_sectest_data:
    .ascii "secure"
user_sectest_data_end:

user_sectest_readonly_target:
    .ascii "READ"

user_sectest_success:
    .ascii "Security syscall validation passed.\n"
user_sectest_success_end:

user_sectest_failure_message:
    .ascii "Security syscall validation failed.\n"
user_sectest_failure_message_end:

LATTER_RUNTIME user_sectest_runtime

user_sectest_end:
.size user_sectest_start, . - user_sectest_start

.global user_wxtest_start
.global user_wxtest_end
.type user_wxtest_start, @object

user_wxtest_start:
    movw $0x1B, %ax
    movw %ax, %ds
    movw %ax, %es

    leaq user_wxtest_message(%rip), %rdi
    movq $(user_wxtest_message_end - user_wxtest_message), %rsi
    call user_wxtest_runtime_console_write

    /* Deliberately attempt to modify the read-only executable page. */
    leaq user_wxtest_start(%rip), %rax
    movb $0x90, (%rax)

    leaq user_wxtest_failure(%rip), %rdi
    movq $(user_wxtest_failure_end - user_wxtest_failure), %rsi
    call user_wxtest_runtime_console_write

    movl $1, %edi
    call user_wxtest_runtime_exit

user_wxtest_message:
    .ascii "W^X test: attempting a code-page write.\n"
user_wxtest_message_end:

user_wxtest_failure:
    .ascii "W^X test failed: code page was writable.\n"
user_wxtest_failure_end:

LATTER_RUNTIME user_wxtest_runtime

user_wxtest_end:
.size user_wxtest_start, . - user_wxtest_start

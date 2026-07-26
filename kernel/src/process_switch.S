.section .text
.code64

.extern process_exit_current

.global process_task_bootstrap
.type process_task_bootstrap, @function
process_task_bootstrap:
    cld

    /*
     * The initial CPU context stores the thread entry in R12
     * and its argument in R13.
     */
    movq %r13, %rdi
    call *%r12

    call process_exit_current

1:
    cli
    hlt
    jmp 1b

.section .note.GNU-stack,"",@progbits

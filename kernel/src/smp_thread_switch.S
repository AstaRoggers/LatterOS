.section .text
.code64

.extern smp_scheduler_exit_current

.global smp_thread_bootstrap
.type smp_thread_bootstrap, @function
smp_thread_bootstrap:
    cld

    /*
     * New kernel-thread contexts carry the entry point in R12 and the
     * argument in R13. The return value becomes the recorded job result.
     */
    movq %r13, %rdi
    call *%r12

    movq %rax, %rdi
    call smp_scheduler_exit_current

1:
    cli
    hlt
    jmp 1b

.section .note.GNU-stack,"",@progbits

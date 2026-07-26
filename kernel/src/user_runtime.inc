/*
 * Minimal LatterOS ring-3 runtime.
 *
 * Emit one private copy inside each flat user-program image:
 *
 *     LATTER_RUNTIME program_prefix
 *
 * Runtime calling convention follows System V AMD64:
 * - arguments in RDI, RSI, RDX
 * - return value in RAX
 */

.set LATTER_SYS_CONSOLE_WRITE, 1
.set LATTER_SYS_EXIT,          2
.set LATTER_SYS_GETPID,        3
.set LATTER_SYS_FILE_OPEN,     4
.set LATTER_SYS_FILE_READ,     5
.set LATTER_SYS_FILE_WRITE,    6
.set LATTER_SYS_FILE_CLOSE,    7
.set LATTER_SYS_SPAWN,         8
.set LATTER_SYS_KILL,          9
.set LATTER_SYS_YIELD,         10

.set LATTER_OPEN_READ,         0x01
.set LATTER_OPEN_WRITE,        0x02
.set LATTER_OPEN_CREATE,       0x04
.set LATTER_OPEN_TRUNCATE,     0x08

.macro LATTER_RUNTIME prefix
\prefix\()_console_write:
    movq $LATTER_SYS_CONSOLE_WRITE, %rax
    int $0x80
    ret

\prefix\()_exit:
    movq $LATTER_SYS_EXIT, %rax
    int $0x80
1:
    pause
    jmp 1b

\prefix\()_getpid:
    movq $LATTER_SYS_GETPID, %rax
    int $0x80
    ret

\prefix\()_file_open:
    movq $LATTER_SYS_FILE_OPEN, %rax
    int $0x80
    ret

\prefix\()_file_read:
    movq $LATTER_SYS_FILE_READ, %rax
    int $0x80
    ret

\prefix\()_file_write:
    movq $LATTER_SYS_FILE_WRITE, %rax
    int $0x80
    ret

\prefix\()_file_close:
    movq $LATTER_SYS_FILE_CLOSE, %rax
    int $0x80
    ret

\prefix\()_spawn:
    movq $LATTER_SYS_SPAWN, %rax
    int $0x80
    ret

\prefix\()_kill:
    movq $LATTER_SYS_KILL, %rax
    int $0x80
    ret

\prefix\()_yield:
    movq $LATTER_SYS_YIELD, %rax
    int $0x80
    ret
.endm

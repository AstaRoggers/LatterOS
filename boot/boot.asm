bits 16
org 0x7C00

mov si, welcome
call print_string

mov si, prompt
call print_string

mov di, buffer

keyboard:
    mov ah, 0
    int 0x16

    cmp al, 13
    je enter_pressed

    cmp al, 8
    je backspace_pressed

    mov [di], al
    inc di

    mov ah, 0x0E
    int 0x10

    jmp keyboard


backspace_pressed:
    cmp di, buffer
    je keyboard

    dec di
    mov byte [di], 0

    mov ah, 0x0E
    mov al, 8
    int 0x10

    mov al, ' '
    int 0x10

    mov al, 8
    int 0x10

    jmp keyboard


enter_pressed:
    mov byte [di], 0

    mov si, new_line
    call print_string

    mov si, buffer
    mov di, help_command
    call compare_strings

    cmp ax, 1
    je show_help

    mov si, buffer
    mov di, clear_command
    call compare_strings

    cmp ax, 1
    je clear_screen

    mov si, unknown_command
    call print_string
    jmp reset_input

clear_screen:
    mov ax, 0x0003
    int 0x10

    jmp reset_input

show_help:
    mov si, help_text
    call print_string

reset_input:
    mov si, prompt
    call print_string

    mov di, buffer
    mov cx, 64
    xor al, al
    rep stosb

    mov di, buffer
    jmp keyboard

print_string:
    lodsb
    cmp al, 0
    je print_done

    mov ah, 0x0E
    int 0x10
    jmp print_string

print_done:
    ret

compare_strings:
compare_loop:
    mov al, [si]
    mov bl, [di]

    cmp al, bl
    jne not_equal

    cmp al, 0
    je equal

    inc si
    inc di
    jmp compare_loop

equal:
    mov ax, 1
    ret

not_equal:
    mov ax, 0
    ret

welcome db "LatterOS v0.0.1", 13, 10, 0
prompt db "LatterOS> ", 0
new_line db 13, 10, 0

help_command db "help", 0
clear_command db "clear", 0

help_text db "Commands:", 13, 10
          db "help", 13, 10, 0

unknown_command db "Unknown command", 13, 10, 0

buffer times 64 db 0

times 510 - ($ - $$) db 0
dw 0xAA55
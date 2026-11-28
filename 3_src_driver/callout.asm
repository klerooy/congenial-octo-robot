; callout.asm — IRETQ-based callout mechanism for rr_exec v2
; Compile: ml64 /c /Cx callout.asm
;
; Fixed: A4000, A2008, A2206, A2006, A2220 (syntax)
; Fixed: frame2 placement (+08h above return addr, not -28h below)
; Fixed: a4 offset (0E0h = 28h + 40h pushes + 78h alloc)
; Fixed: frame2.RSP = current rsp (not rsp+78h) so _resume add/pop restores correctly
; Fixed: g_kernel_stack_base from mapper (not approximate RSP+offset)

.DATA
EXTERN g_real_stack:QWORD
EXTERN g_iret_gadget:QWORD
EXTERN g_ret_gadget:QWORD
EXTERN g_kernel_stack_base:QWORD
EXTERN worker_loop:PROC

.CODE

callout_bootstrap PROC
    mov     rsp, g_real_stack
    add     rsp, 03F00h
    cli

    push    rdi
    mov     rdi, g_kernel_stack_base
    sub     rdi, 0200h
    mov     rcx, 040h
    xor     eax, eax
    rep stosq
    pop     rdi

    jmp     worker_loop
callout_bootstrap ENDP

callout_invoke PROC
    push    rbp
    push    rbx
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 078h

    mov     r12, rcx
    mov     r13, rdx
    mov     r14, r8
    mov     r15, r9
    mov     rbx, [rsp+0E0h]     ; a4

    ; IRET frame 1 on real stack
    mov     [rsp+00h], r12
    mov     qword ptr [rsp+08h], 010h
    mov     qword ptr [rsp+10h], 0202h
    mov     rax, g_kernel_stack_base
    sub     rax, 0100h
    mov     [rsp+18h], rax
    mov     qword ptr [rsp+20h], 018h

    ; Return chain on kernel stack
    mov     rdi, [rsp+18h]

    mov     rax, g_iret_gadget
    mov     [rdi+00h], rax      ; target_fn RET → IRETQ gadget

    lea     rax, _resume
    mov     [rdi+08h], rax      ; frame2.RIP
    mov     qword ptr [rdi+10h], 010h
    mov     qword ptr [rdi+18h], 000h
    mov     [rdi+20h], rsp      ; frame2.RSP = CURRENT rsp (no +78h!)
    mov     qword ptr [rdi+28h], 018h

    mov     rcx, r13
    mov     rdx, r14
    mov     r8, r15
    mov     r9, rbx

    mov     rax, g_iret_gadget
    jmp     rax

_resume:
    add     rsp, 078h
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rdi
    pop     rsi
    pop     rbx
    pop     rbp
    ret
callout_invoke ENDP

END
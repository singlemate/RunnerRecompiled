# Entry/exit trampolines of the differential harness (i686, mingw: C symbols get a leading '_').
# State block (see harness.c, NativeState): +0 eax ecx edx ebx esp ebp esi edi, +32 fcw, +36 mxcsr,
# +48 xmm0..xmm7 (16 bytes each), +176 fnsave image (108 bytes).
        .intel_syntax noprefix
        .text

# int run_native(uint32_t target, NativeState *st)
        .globl _run_native
_run_native:
        push ebp
        push ebx
        push esi
        push edi
        mov [_saved_esp], esp
        mov eax, [esp + 20]
        mov [_target], eax
        mov eax, [esp + 24]
        mov [_regs_ptr], eax
        fninit
        fldcw word ptr [eax + 32]
        ldmxcsr dword ptr [eax + 36]
        movups xmm0, [eax + 48]
        movups xmm1, [eax + 64]
        movups xmm2, [eax + 80]
        movups xmm3, [eax + 96]
        movups xmm4, [eax + 112]
        movups xmm5, [eax + 128]
        movups xmm6, [eax + 144]
        movups xmm7, [eax + 160]
        mov ecx, [eax + 4]
        mov edx, [eax + 8]
        mov ebx, [eax + 12]
        mov ebp, [eax + 20]
        mov esi, [eax + 24]
        mov edi, [eax + 28]
        mov esp, [eax + 16]
        mov eax, [eax + 0]
        cld
        jmp dword ptr [_target]   # [esp] already holds _native_ret (written by the harness)

        .globl _native_ret
_native_ret:
        mov [_tmp_eax], eax
        mov eax, [_regs_ptr]
        mov [eax + 4], ecx
        mov [eax + 8], edx
        mov [eax + 12], ebx
        mov [eax + 16], esp
        mov [eax + 20], ebp
        mov [eax + 24], esi
        mov [eax + 28], edi
        mov ecx, [_tmp_eax]
        mov [eax + 0], ecx
        stmxcsr dword ptr [eax + 36]
        movups [eax + 48], xmm0
        movups [eax + 64], xmm1
        movups [eax + 80], xmm2
        movups [eax + 96], xmm3
        movups [eax + 112], xmm4
        movups [eax + 128], xmm5
        movups [eax + 144], xmm6
        movups [eax + 160], xmm7
        fnsave [eax + 176]
        mov esp, [_saved_esp]
        call restore_host_fp
        pop edi
        pop esi
        pop ebx
        pop ebp
        xor eax, eax
        ret

# int run_lifted(void (*fn)(void *), void *ctx)
        .globl _run_lifted
_run_lifted:
        push ebp
        push ebx
        push esi
        push edi
        mov [_saved_esp], esp
        mov eax, [esp + 20]
        push dword ptr [esp + 24]
        call eax
        add esp, 4
        pop edi
        pop esi
        pop ebx
        pop ebp
        xor eax, eax
        ret

# Resumed here (from the exception handler, the watchdog or x86_trap) to abandon a run.
        .globl _fault_landing
_fault_landing:
        cld
        mov esp, [_saved_esp]
        call restore_host_fp
        pop edi
        pop esi
        pop ebx
        pop ebp
        mov eax, 1
        ret

# The harness itself computes with SSE; give it a clean x87 and the default MXCSR back.
restore_host_fp:
        fninit
        ldmxcsr dword ptr [_host_mxcsr]
        ret

        .data
        .globl _saved_esp
_saved_esp:  .long 0
_target:     .long 0
_regs_ptr:   .long 0
_tmp_eax:    .long 0
_host_mxcsr: .long 0x1F80

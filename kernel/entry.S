/* ===========================================================================
 * AnkaOS - çekirdek giriş kodu ve düşük seviye yardımcılar (Intel sözdizimi)
 *   _start          : önyükleyiciden gelen ilk kod; yığın kur, BSS'yi sıfırla
 *   gdt/idt/tss_flush : yazmaç yükleme yardımcıları
 *   isr_stub_table  : 48 donanım/istisna girişi + 0x80 sistem çağrısı
 *   context_switch  : görev değiştirme (callee-saved yazmaçlar + esp)
 *   enter_user      : iret ile ring 3'e geçiş
 * =========================================================================== */
    .intel_syntax noprefix
    .code32

    .section .text.entry,"ax",@progbits
    .global _start
_start:
    cli
    cld
    mov   esp, offset boot_stack_top
    xor   ebp, ebp
    mov   edi, offset __bss_start
    mov   ecx, offset __bss_end
    sub   ecx, edi
    shr   ecx, 2
    xor   eax, eax
    rep   stosd                         /* BSS'yi sıfırla */
    call  kmain
1:  cli
    hlt
    jmp   1b

    .section .text,"ax",@progbits

/* void gdt_flush(const void *gdtr) */
    .global gdt_flush
gdt_flush:
    mov   eax, [esp + 4]
    lgdt  [eax]
    mov   ax, 0x10
    mov   ds, ax
    mov   es, ax
    mov   fs, ax
    mov   gs, ax
    mov   ss, ax
    push  0x08                          /* yeni CS */
    push  offset gdt_flush_done
    retf                                /* CS'yi yeniden yükle */
gdt_flush_done:
    ret

/* void idt_flush(const void *idtr) */
    .global idt_flush
idt_flush:
    mov   eax, [esp + 4]
    lidt  [eax]
    ret

/* void tss_flush(void)  -- TSS seçicisi 0x28 */
    .global tss_flush
tss_flush:
    mov   ax, 0x28
    ltr   ax
    ret

/* void context_switch(u32 *old_esp, u32 new_esp) */
    .global context_switch
context_switch:
    mov   eax, [esp + 4]
    mov   edx, [esp + 8]
    push  ebp
    push  ebx
    push  esi
    push  edi
    mov   [eax], esp
    mov   esp, edx
    pop   edi
    pop   esi
    pop   ebx
    pop   ebp
    ret

/* void enter_user(u32 entry, u32 user_esp)  -- ring 3'e iret ile atlar */
    .global enter_user
enter_user:
    mov   eax, [esp + 4]
    mov   ecx, [esp + 8]
    mov   edx, 0x23                     /* kullanıcı veri seçicisi (RPL=3) */
    mov   ds, dx
    mov   es, dx
    mov   fs, dx
    mov   gs, dx
    push  0x23                          /* ss     */
    push  ecx                           /* esp    */
    push  0x202                         /* eflags (IF=1) */
    push  0x1B                          /* cs     */
    push  eax                           /* eip    */
    iret

/* --- Kesme/istisna girişleri ------------------------------------------- */
.macro ISR_NOERR n
    .global isr\n
isr\n:
    push  0                             /* sahte hata kodu */
    push  \n
    jmp   isr_common
.endm

.macro ISR_ERR n
    .global isr\n
isr\n:
    push  \n                            /* hata kodunu CPU zaten itti */
    jmp   isr_common
.endm

.irp n, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ISR_NOERR \n
.endr
.irp n, 8,10,11,12,13,14,17
    ISR_ERR \n
.endr
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,128
    ISR_NOERR \n
.endr

/* struct regs ile birebir aynı sıra (isr.c / kernel.h) */
isr_common:
    pusha
    push  ds
    push  es
    push  fs
    push  gs
    mov   ax, 0x10
    mov   ds, ax
    mov   es, ax
    mov   fs, ax
    mov   gs, ax
    push  esp                           /* C'ye struct regs* ver */
    call  isr_handler
    add   esp, 4
    pop   gs
    pop   fs
    pop   es
    pop   ds
    popa
    add   esp, 8                        /* int_no + err_code */
    iret

    .section .rodata
    .global isr_stub_table
isr_stub_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .long isr\n
.endr

    .section .bss
    .align 16
    .global boot_stack_bottom
boot_stack_bottom:
    .skip 16384
    .global boot_stack_top
boot_stack_top:

    .section .note.GNU-stack,"",@progbits

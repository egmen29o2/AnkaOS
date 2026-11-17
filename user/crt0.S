/* AnkaOS - kullanıcı programı başlangıcı. İmajın ilk 12 baytı exec_header'dır.
 * Çekirdek yığına [argc][argv] koyar; main(argc, argv) çağrılır, dönüş değeri exit koduna döner. */
    .intel_syntax noprefix
    .code32
    .section .text.start,"ax",@progbits
    .global _start
    .long 0x414B5845                    /* EXEC_MAGIC */
    .long __image_memsz                 /* bss dahil boyut (bağlayıcı hesaplar) */
    .long _start                        /* giriş noktası */
_start:
    pop   eax                           /* argc */
    pop   edx                           /* argv */
    push  edx
    push  eax
    call  main
    push  eax
    call  exit
1:  jmp   1b
    .section .note.GNU-stack,"",@progbits

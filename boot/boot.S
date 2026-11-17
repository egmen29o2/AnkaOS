/* ===========================================================================
 * AnkaOS - Aşama 1 önyükleyici (boot sector, tam 512 bayt)
 *
 * BIOS bu sektörü 0000:7C00'e yükler, DL = boot sürücüsü.
 *   1. Segmentleri ve yığını kurar, 80x25 metin modunu seçer
 *   2. A20 hattını açar (BIOS + hızlı A20 portu 0x92)
 *   3. BIOS INT 15h/E820 ile bellek haritasını alır  -> BOOTINFO (0x500)
 *   4. Çekirdeği LBA 1..N sektörlerinden 0x10000 adresine okur (INT 13h/42h)
 *   5. GDT kurup korumalı moda (32 bit) geçer ve çekirdeğe atlar
 *
 * BOOTINFO düzeni (çekirdek de aynısını okur):
 *   0x500: "ANKA" sihirli sayısı   0x504: boot sürücüsü
 *   0x508: E820 kayıt sayısı       0x510: E820 kayıtları (24 bayt x en çok 40)
 * =========================================================================== */
#ifndef KERNEL_SECTORS
#error "KERNEL_SECTORS tanimli degil (Makefile -DKERNEL_SECTORS=N verir)"
#endif

#define BOOTINFO        0x0500
#define BOOTINFO_E820   0x0510
#define MAX_E820        40
#define KERNEL_SEG      0x1000          /* 0x1000:0000 = 0x10000 */
#define KERNEL_ADDR     0x10000

    .code16
    .intel_syntax noprefix
    .section .text
    .global _start

_start:
    cli
    xor   ax, ax
    mov   ds, ax
    mov   es, ax
    mov   ss, ax
    mov   sp, 0x7C00
    cld
    push  ax                        /* CS'yi kesin olarak 0000'a sabitle */
    push  offset start16
    retf
start16:
    mov   byte ptr [boot_drive], dl
    sti

    mov   ax, 0x0003                /* 80x25 metin modu, ekranı temizle */
    int   0x10
    mov   si, offset msg_boot
    call  print

    /* --- A20 hattını aç ------------------------------------------------ */
    mov   ax, 0x2401                /* BIOS yöntemi (başarısız olabilir) */
    int   0x15
    in    al, 0x92                  /* hızlı A20 */
    test  al, 2
    jnz   1f
    or    al, 2
    and   al, 0xFE                  /* bit 0 = sistem reseti, dokunma! */
    out   0x92, al
1:
    /* --- BOOTINFO başlığı --------------------------------------------- */
    mov   dword ptr [BOOTINFO], 0x414B4E41        /* "ANKA" */
    movzx eax, byte ptr [boot_drive]
    mov   dword ptr [BOOTINFO + 4], eax

    /* --- E820 bellek haritası ------------------------------------------ */
    xor   ebx, ebx
    xor   ebp, ebp
    mov   di, BOOTINFO_E820
e820_next:
    mov   eax, 0xE820
    mov   edx, 0x534D4150           /* "SMAP" */
    mov   ecx, 24
    mov   dword ptr [di + 20], 1    /* ACPI 3.0 "geçerli" özniteliği varsayılanı */
    int   0x15
    jc    e820_done
    cmp   eax, 0x534D4150
    jne   e820_done
    test  ecx, ecx
    jz    1f
    add   di, 24
    inc   bp
    cmp   bp, MAX_E820
    jae   e820_done
1:
    test  ebx, ebx
    jnz   e820_next
e820_done:
    mov   dword ptr [BOOTINFO + 8], ebp

    /* --- Çekirdeği oku (her seferinde 1 sektör, LBA) -------------------- */
    mov   word ptr [sectors_left], KERNEL_SECTORS
read_loop:
    mov   si, offset dap
    mov   dl, byte ptr [boot_drive]
    mov   ah, 0x42
    int   0x13
    jc    disk_error
    add   word ptr [dap_seg], 0x20  /* 512 bayt = 0x20 paragraf */
    inc   dword ptr [dap_lba]
    dec   word ptr [sectors_left]
    jnz   read_loop

    /* --- Korumalı moda geç --------------------------------------------- */
    cli
    lgdt  [gdt_desc]
    mov   eax, cr0
    or    al, 1
    mov   cr0, eax
    .byte 0x66, 0xEA                /* jmp 0x08:pm_entry (32 bit uzak atlama) */
    .long pm_entry
    .word 0x08

disk_error:
    mov   si, offset msg_err
    call  print
1:  hlt
    jmp   1b

print:                              /* SI = NUL ile biten dizgi */
    lodsb
    test  al, al
    jz    2f
    mov   ah, 0x0E
    mov   bx, 0x0007
    int   0x10
    jmp   print
2:  ret

    .code32
pm_entry:
    mov   ax, 0x10
    mov   ds, ax
    mov   es, ax
    mov   fs, ax
    mov   gs, ax
    mov   ss, ax
    mov   esp, 0x7C00
    mov   eax, KERNEL_ADDR
    jmp   eax                       /* çekirdeğin _start'ına */

    .code16
/* --- Veriler ------------------------------------------------------------- */
boot_drive:   .byte 0
sectors_left: .word 0

    .align 4
dap:                                /* Disk Address Packet (INT 13h/42h) */
    .byte 0x10, 0
    .word 1                         /* sektör sayısı */
dap_off:
    .word 0                         /* hedef ofset  */
dap_seg:
    .word KERNEL_SEG                /* hedef segment */
dap_lba:
    .long 1, 0                      /* 64 bit LBA: sektör 1'den başla */

    .align 8
gdt:
    .quad 0x0000000000000000        /* boş            */
    .quad 0x00CF9A000000FFFF        /* 0x08: kod, 4GB */
    .quad 0x00CF92000000FFFF        /* 0x10: veri, 4GB */
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt

msg_boot: .asciz "AnkaOS: loading kernel..."
msg_err:  .asciz "Disk read error!"

    .org 510
    .word 0xAA55

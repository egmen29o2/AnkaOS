# AnkaOS 0.1

Sıfırdan yazılmış, küçük bir **x86 (32 bit) işletim sistemi** — çekirdek **C**, düşük seviye kısımlar
**assembly** (GNU as, Intel sözdizimi). BIOS ile açılır; kendi önyükleyicisi, korumalı mod, sayfalama,
önleyici çoklu görev, ring 3 kullanıcı programları ve etkileşimli bir kabuğu vardır.

```
AnkaOS 0.1.0 booting (i386, protected mode)
[ ok ] GDT + TSS loaded (ring 0/3 segments)
[ ok ] IDT installed, PIC remapped to vectors 0x20-0x2F
[ ok ] paging enabled, identity map 128 MB, page 0 unmapped
[ ok ] PIT timer at 100 Hz, preemptive scheduler ready
anka:/> hello
Hello from user mode! (pid 2, ring 3)
anka:/> crash kernel          <- ring 3 çekirdek belleğine yazmaya çalışır
[pid 3 'crash'] segmentation fault: write at 0x00100000 (protection violation)
```

## Hızlı başlangıç

```sh
qemu-system-i386 -drive format=raw,file=os.img -m 128M      # hazır imaj (dizinde os.img var)
```

Kaynaktan derlemek için `gcc` (32 bit kod üretebilen), `binutils` (ld/objcopy) ve `make` yeterli:

```sh
sudo apt install build-essential qemu-system-x86     # Debian/Ubuntu
make            # -> os.img   (V=1 make: tüm komutları göster)
make run        # QEMU penceresi
make run-nographic   # sadece terminal; seri konsoldan kullanılır (çıkış: Ctrl-A X)
make run-debug  # CPU hata günlüğü: qemu.log   |   make debug : gdb için -s -S
make test       # QEMU'suz otomatik testler (tools/x86emu ile)
```

Çapraz derleyici varsa: `make CC=i686-elf-gcc LD=i686-elf-ld OBJCOPY=i686-elf-objcopy`.
Gerçek donanım: `dd if=os.img of=/dev/sdX` ile USB'ye yazıp **BIOS/CSM (legacy)** modunda açın
(UEFI-only makinelerde açılmaz; klavye için PS/2 veya BIOS PS/2 emülasyonu gerekir).

## Neler var?

| Alan | İçerik |
|---|---|
| Önyükleme | 512 baytlık boot sektörü: A20, BIOS E820 bellek haritası, LBA ile çekirdek okuma (INT 13h/42h), GDT + korumalı mod geçişi |
| CPU | GDT (ring 0/3) + TSS, IDT (istisna/IRQ/syscall), 8259 PIC (sahte IRQ filtresi), 8254 PIT 100 Hz |
| Bellek | Bitmap fiziksel bellek yöneticisi (E820), sayfalama (çekirdek birebir eşleme ≤256 MB, **sayfa 0 eşsiz → NULL koruması**), işlem başına sayfa dizini, first-fit+birleştirmeli çekirdek heap'i |
| Görevler | Önleyici round-robin (50 ms), uyku/engelleme, zombi toplama, çekirdek yığını canary kontrolü |
| Kullanıcı modu | Ring 3 düz ikili programlar (`EXKA` başlığı, argv), `int 0x80` sistem çağrıları, kullanıcı işaretçisi doğrulama, hata yapan işlem öldürülür — çekirdek yaşar |
| Sürücüler | VGA metin (kaydırma, imleç, renk), COM1 (günlük + giriş, IRQ4), PS/2 klavye (shift/caps/ctrl, ok tuşları) |
| Dosya sistemi | RAM'de ağaç yapılı dosya sistemi (`/bin`, `/etc`, `/home`, `/tmp`); programlar `/bin`'e gömülü gelir |
| Kabuk | 28 komut, `>` `>>` yönlendirme, `&` arka plan, geçmiş (↑/↓), Ctrl-C, Ctrl-U |

### Kabukta deneyin

```
help                          komut listesi
hello a1 "b 2"                ring 3'te program (argv ile), uyku sistem çağrısı
primes 20000 &                arka planda CPU yoran program; kabuk yanıt vermeye devam eder
ps                            görevler (idle, shell, primes...)    kill <pid>
threads                       3 çekirdek iş parçacığı, önleyici zaman paylaşımı
guess                         stdin'den satır okuyan oyun (read sistem çağrısı); Ctrl-C ile çık
crash kernel|null|cli|div|hlt|badptr|stack   koruma gösterileri: işlem ölür, sistem yaşar
echo merhaba > /tmp/a.txt     cat /tmp/a.txt     cp/mv/rm -r/mkdir/cd/ls/hexdump
mem  uptime  date  cpu  uname  color 14 1  reboot  poweroff
panic / fault                 çekirdek panik ekranı testleri (kırmızı ekran + geri izleme)
```

## Mimari

**Önyükleme akışı:** BIOS → `boot/boot.S` (0x7C00) → çekirdek 0x10000'e okunur → 32 bit → `kernel/entry.S`
(`_start`: yığın, BSS sıfırlama) → `kmain()` → GDT → IDT/PIC → PMM → sayfalama → heap → görevler/PIT →
klavye/seri → ramfs → kabuk iş parçacığı; `kmain` kendisi `idle` görevi olur.

**Fiziksel bellek haritası**

| Adres | Kullanım |
|---|---|
| `0x00000500` | BOOTINFO (boot sektörü yazar: "ANKA", sürücü, E820 girdileri) |
| `0x00007C00` | boot sektörü (sonra serbest) |
| `0x00010000` | çekirdek imajı (.text/.rodata/.data/.bss, ≈80 KB) |
| `0x000B8000` | VGA metin belleği |
| `≥ 0x00100000` | PMM'in yönettiği RAM: sayfa dizin/tabloları, 16 MB çekirdek heap'i, işlem çerçeveleri |

**Sanal adres (kullanıcı):** program `0x40000000`'dan başlar, yığın `0x7FFFC000–0x80000000` (4 sayfa).
Çekirdek her adres alanında ortak ve yalnızca supervisor erişimlidir.

**Seçiciler:** `0x08` çekirdek kodu · `0x10` çekirdek verisi · `0x18` kullanıcı kodu · `0x20` kullanıcı verisi · `0x28` TSS.
**Vektörler:** 0–31 istisnalar · 32–47 IRQ (0 PIT, 1 klavye, 4 COM1) · `0x80` sistem çağrısı.

**Sistem çağrıları** (`int 0x80`; eax=no, ebx/ecx/edx=argüman, dönüş eax): `1 exit(code)` · `2 write(fd,buf,len)` ·
`3 read(0,buf,len)` (satır okur) · `4 getpid()` · `5 sleep(ms)` · `6 yield()` · `7 uptime()`.

### Dizin yapısı

```
boot/boot.S          önyükleyici (assembly)          kernel/shell.c    kabuk + komutlar
kernel/entry.S       giriş, ISR taslakları, context_switch, enter_user
kernel/*.c           gdt idt isr pic pit vga serial keyboard console pmm vmm heap task syscall ramfs kernel
user/                crt0.S ulib.c + hello primes guess crash   (ring 3 programları)
lib/                 string.c printf.c  (çekirdek ve kullanıcı ortak)
include/             kernel.h abi.h lib.h types.h
tools/x86emu.c       minimal x86 PC emülatörü (test için)     tests/  senaryolar + run.sh
```

## Genişletme

* **Yeni kullanıcı programı:** `user/foo.c` yazın (`int main(int argc, char **argv)`, `#include "ulib.h"`), `Makefile`'daki
  `USER_PROGS`'a, `kernel/progs.S`'e (`PROG foo`) ve `kernel/progtab.c`'ye ekleyin → `/bin/foo` olur.
* **Yeni sistem çağrısı:** numarayı `include/abi.h`'e, işlemeyi `kernel/syscall.c`'ye, sarmalayıcıyı `user/ulib.c`'ye ekleyin.
* **Yeni sürücü:** IRQ için `irq_install(n, handler)` + `irq_unmask(n)`; giriş için `input_push()`.
* **Yeni kabuk komutu:** `kernel/shell.c`'de fonksiyon yazıp `cmds[]` tablosuna ekleyin.

## Doğrulama hakkında dürüst not

Bu kodu yazdığım ortamda **QEMU yoktu**. Bu yüzden `tools/x86emu.c` adlı küçük bir x86 emülatörü yazıp
önyükleyiciden başlayarak her şeyi onda çalıştırdım (gerçek mod + BIOS taklidi, korumalı mod, sayfalama, ring 3,
PIC/PIT/klavye/seri). `make test` şu senaryoları koşar: boot, kabuk/ramfs, ring 3 programları + koruma,
Ctrl-C, geçmiş, seri giriş, 5 eşzamanlı işlem, bozuk işaretçiler, yığın taşması, görev tablosu, panik, reboot,
poweroff, 8 MB ve 512 MB RAM — hepsi geçiyor; çok sayıda işlem sonrası serbest bellek başlangıçtakiyle birebir aynı (sızıntı yok).
Ancak emülatör **gerçek bir PC değildir**: gerçek QEMU/donanımda ilk denemede beklenmedik bir fark çıkarsa
`make run-debug` ve `qemu.log` (CPU istisna günlüğü) ilk bakılacak yerdir. `run-serial`/`run-nographic`
hedeflerini ben çalıştıramadım.

## Bilinen sınırlamalar

* Yalnızca BIOS (UEFI yok), tek CPU, 32 bit, bellek ≤256 MB yönetilir.
* Dosya sistemi RAM'de; yeniden başlatınca silinir. Diskten okuma sadece önyükleyicide var (ATA sürücüsü yok).
* Arka plan programlar stdin'i okumamalı (kabukla tuş vuruşları için yarışırlar). Konsol çıktısı yaklaşık 80 karakterlik parçalar halinde atomiktir.
* Ekran CP437 olduğu için ekrandaki metin ASCII'dir (ş, ğ, ı gösterilmez); klavye düzeni US'dir.
* Yürütülebilir biçim: düz ikili (ELF yükleyicisi yok); `fork/exec/wait`, sinyaller, kullanıcı malloc'ı yok.

## Yol haritası (öneriler)

ATA/AHCI sürücüsü + kalıcı dosya sistemi (FAT32 veya kendi biçiminiz) · ELF yükleyici · `fork/exec/wait` ·
kullanıcı heap'i (`sbrk`) · Türkçe klavye düzeni + CP857 yazı tipi · APIC/SMP · basit ağ (RTL8139/E1000) ·
VESA grafik modu · uzun mod (x86-64) portu.

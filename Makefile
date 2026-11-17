# ============================================================================
#  AnkaOS Makefile
#  Gerekenler: gcc (32 bit üretebilen), binutils (ld/objcopy), make.  Çalıştırmak için: qemu-system-i386
#  Çapraz derleyici varsa:  make CC=i686-elf-gcc LD=i686-elf-ld OBJCOPY=i686-elf-objcopy
# ============================================================================
CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy
QEMU    ?= qemu-system-i386
QFLAGS  ?= -m 128M

ARCHFLAGS := -m32 -march=i586 -mno-sse -mno-mmx -mno-80387
CFLAGS    := $(ARCHFLAGS) -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-pic -fno-pie \
             -fno-stack-protector -fcf-protection=none -fno-asynchronous-unwind-tables \
             -fno-stack-clash-protection -fno-tree-loop-distribute-patterns \
             -fno-omit-frame-pointer -fno-common -Iinclude
ASFLAGS   := -m32 -Iinclude
LDFLAGS   := -m elf_i386 -z noexecstack --build-id=none
# yeni binutils'in "RWX segment" uyarısını sustur (düz çekirdek imajı için normal)
LDFLAGS   += $(shell $(LD) --help 2>/dev/null | grep -q -- --no-warn-rwx-segments && echo --no-warn-rwx-segments)
HOSTCC    ?= cc

# V=1 make  -> tüm komutları göster
ifeq ($(V),1)
Q :=
else
Q := @
endif

B := build

KERNEL_SRCS := console gdt heap idt isr kernel keyboard pic pit pmm ramfs serial shell syscall task vga vmm progtab
KERNEL_OBJS := $(addprefix $(B)/kernel/,$(addsuffix .o,$(KERNEL_SRCS))) $(B)/kernel/entry.o $(B)/kernel/progs.o \
               $(B)/lib/string.o $(B)/lib/printf.o

USER_PROGS  := hello primes guess crash
USER_BINS   := $(addprefix $(B)/user/,$(addsuffix .bin,$(USER_PROGS)))

.PHONY: all run run-serial run-nographic run-debug debug clean size test
.SECONDARY:

all: os.img

$(B)/kernel $(B)/lib $(B)/user:
	@mkdir -p $@

# ---- derleme kuralları -------------------------------------------------------
$(B)/kernel/%.o: kernel/%.c $(wildcard include/*.h) | $(B)/kernel
	@echo "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@
$(B)/kernel/%.o: kernel/%.S | $(B)/kernel
	@echo "  AS    $<"
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@
$(B)/lib/%.o: lib/%.c $(wildcard include/*.h) | $(B)/lib
	@echo "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@
$(B)/user/%.o: user/%.c user/ulib.h $(wildcard include/*.h) | $(B)/user
	@echo "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -Iuser -c $< -o $@
$(B)/user/%.o: user/%.S | $(B)/user
	@echo "  AS    $<"
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

# ---- kullanıcı programları: düz ikili (.bin), USER_BASE'e bağlı ------------------
$(B)/user/%.elf: $(B)/user/crt0.o $(B)/user/%.o $(B)/user/ulib.o $(B)/lib/string.o $(B)/lib/printf.o user/user.ld
	@echo "  LD    $@"
	$(Q)$(LD) $(LDFLAGS) -T user/user.ld -o $@ $(filter %.o,$^)
$(B)/user/%.bin: $(B)/user/%.elf
	@echo "  BIN   $@"
	$(Q)$(OBJCOPY) -O binary $< $@

# ---- çekirdek ---------------------------------------------------------------------
$(B)/kernel/progs.o: kernel/progs.S $(USER_BINS) | $(B)/kernel
	@echo "  AS    $<  (+ user programs)"
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

$(B)/kernel.elf: $(KERNEL_OBJS) kernel/linker.ld
	@echo "  LD    $@"
	$(Q)$(LD) $(LDFLAGS) -T kernel/linker.ld -o $@ $(KERNEL_OBJS)

$(B)/kernel.bin: $(B)/kernel.elf
	@echo "  BIN   $@"
	$(Q)$(OBJCOPY) -O binary $< $@

# ---- önyükleyici: çekirdeğin sektör sayısı derleme anında gömülür ---------------------
$(B)/boot.o: boot/boot.S $(B)/kernel.bin
	@KS=$$(( ($$(wc -c < $(B)/kernel.bin) + 511) / 512 )); \
	echo "  AS    $< (kernel = $$KS sectors)"; \
	[ $$KS -le 800 ] || { echo "ERROR: kernel too large for the 0x10000 load area"; exit 1; }; \
	$(CC) $(ASFLAGS) -DKERNEL_SECTORS=$$KS -c $< -o $@

$(B)/boot.bin: $(B)/boot.o boot/boot.ld
	@echo "  LD    $@"
	$(Q)$(LD) -m elf_i386 -T boot/boot.ld -o $@ $<
	@[ $$(wc -c < $@) -eq 512 ] || { echo "ERROR: boot sector must be exactly 512 bytes"; exit 1; }

# ---- disk imajı (1.44 MB ham imaj: sektör 0 = boot, sektör 1.. = çekirdek) -----------------
os.img: $(B)/boot.bin $(B)/kernel.bin
	@echo "  IMG   $@"
	$(Q)dd if=/dev/zero of=$@ bs=512 count=2880 2>/dev/null
	$(Q)dd if=$(B)/boot.bin of=$@ conv=notrunc 2>/dev/null
	$(Q)dd if=$(B)/kernel.bin of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null
	@echo "==> $@ ready ($$(wc -c < $(B)/kernel.bin) bytes of kernel)"

size: $(B)/kernel.elf
	@size $(B)/kernel.elf; ls -l $(B)/user/*.bin

# ---- çalıştırma ------------------------------------------------------------------
run: os.img
	$(QEMU) -drive format=raw,file=os.img $(QFLAGS)
run-serial: os.img            # pencere + terminalde seri konsol (Ctrl-C konuk'a gider; çıkmak için pencereyi kapatın)
	$(QEMU) -drive format=raw,file=os.img $(QFLAGS) -chardev stdio,id=ser0,signal=off -serial chardev:ser0
run-nographic: os.img         # yalnızca terminal (çıkış: Ctrl-A sonra X)
	$(QEMU) -drive format=raw,file=os.img $(QFLAGS) -display none \
	        -chardev stdio,id=ser0,mux=on,signal=off -serial chardev:ser0 -mon chardev=ser0,mode=readline
run-debug: os.img             # CPU hata/kesme günlüğü (qemu.log), reset olunca kapanır
	$(QEMU) -drive format=raw,file=os.img $(QFLAGS) -d int,cpu_reset -D qemu.log -no-reboot -no-shutdown
debug: os.img                 # gdb: (gdb) target remote :1234 ; symbol-file build/kernel.elf
	$(QEMU) -drive format=raw,file=os.img $(QFLAGS) -s -S

# ---- QEMU'suz test: tools/x86emu (minimal x86 emülatörü) ile boot + kabuk + ring 3 senaryoları ----
test: os.img
	@HOSTCC="$(HOSTCC)" sh tests/run.sh

clean:
	rm -rf $(B) os.img qemu.log tools/x86emu

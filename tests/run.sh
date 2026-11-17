#!/bin/sh
# AnkaOS test calistiricisi: make test  (once `make` ile os.img uretilmis olmali)
# QEMU gerekmez; tools/x86emu minimal emulatoru host derleyiciyle derlenir.
cd "$(dirname "$0")/.." || exit 1
HOSTCC=${HOSTCC:-cc}
mkdir -p build
$HOSTCC -O2 -o tools/x86emu tools/x86emu.c || exit 1
fail=0
for t in boot smoke user stress panic fault reboot poweroff; do
    printf "  %-9s " "$t"
    if ./tools/x86emu -n 900000000 os.img tests/$t.script > build/test-$t.log 2>&1; then
        echo PASS
    else
        echo FAIL
        tail -8 build/test-$t.log
        fail=1
    fi
done
for m in 8 512; do
    printf "  %-9s " "ram-${m}MB"
    if ./tools/x86emu -m $m -n 900000000 os.img tests/mem.script > build/test-mem$m.log 2>&1; then echo PASS; else echo FAIL; tail -8 build/test-mem$m.log; fail=1; fi
done
[ $fail -eq 0 ] && echo "all tests passed" || echo "SOME TESTS FAILED (see build/test-*.log)"
exit $fail

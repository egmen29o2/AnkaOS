/* AnkaOS - PS/2 klavye sürücüsü (IRQ1, scancode set 1, US düzeni) */
#include <kernel.h>

static const char kmap[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b', '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']', '\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ',
};

static const char kmap_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b', '\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|',
    'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' ',
};

static int shift, ctrl, alt, caps, e0;

static void kbd_irq(struct regs *r)
{
    (void)r;
    u8 sc = inb(0x60);

    if (sc == 0xE0) { e0 = 1; return; }
    int release = sc & 0x80;
    sc &= 0x7F;

    if (e0) {                                   /* genişletilmiş tuşlar */
        e0 = 0;
        if (sc == 0x1D) { ctrl = !release; return; }
        if (sc == 0x38) { alt = !release; return; }
        if (release) return;
        u16 key = 0;
        switch (sc) {
        case 0x48: key = KEY_UP;    break;
        case 0x50: key = KEY_DOWN;  break;
        case 0x4B: key = KEY_LEFT;  break;
        case 0x4D: key = KEY_RIGHT; break;
        case 0x47: key = KEY_HOME;  break;
        case 0x4F: key = KEY_END;   break;
        case 0x53: key = KEY_DEL;   break;
        }
        if (key) input_push(key);
        return;
    }

    switch (sc) {
    case 0x2A: case 0x36: shift = !release; return;
    case 0x1D: ctrl = !release; return;
    case 0x38: alt = !release; return;
    case 0x3A: if (!release) caps = !caps; return;
    }
    if (release || sc >= 128) return;

    char base = kmap[sc];
    if (!base) return;
    char c;
    if (base >= 'a' && base <= 'z') {
        c = (shift ^ caps) ? (char)(base - 32) : base;
        if (ctrl) c = (char)(base - 'a' + 1);              /* Ctrl-A..Z = 1..26 */
    } else {
        c = shift ? kmap_shift[sc] : base;
    }
    if (c == 3) console_raise_sigint();
    input_push((u16)(u8)c);
}

void kbd_init(void)
{
    while (inb(0x64) & 1) (void)inb(0x60);               /* çıkış tamponunu boşalt */
    irq_install(1, kbd_irq);
    irq_unmask(1);
}

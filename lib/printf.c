/* AnkaOS - minimal printf motoru (%d %i %u %x %X %o %p %c %s %%)
 * Bayraklar: '-' '0' '#', genişlik, kesinlik, '*' desteklenir. 32 bit. */
#include <lib.h>

#define F_LEFT 0x01
#define F_ZERO 0x02
#define F_ALT  0x04

struct out { char *buf; size_t size; size_t pos; };

static void out_ch(struct out *o, char c)
{
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void out_pad(struct out *o, char c, int n)
{
    while (n-- > 0) out_ch(o, c);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_ch(&o, *fmt); continue; }
        fmt++;

        int flags = 0, width = 0, prec = -1;
        for (;; fmt++) {
            if (*fmt == '-') flags |= F_LEFT;
            else if (*fmt == '0') flags |= F_ZERO;
            else if (*fmt == '#') flags |= F_ALT;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= F_LEFT; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;

        char tmp[12];
        const char *body = tmp;
        const char *prefix = "";
        int blen = 0, zeros = 0, numeric = 0;
        char c = *fmt;

        switch (c) {
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            blen = 1;
            break;
        case 's':
            body = va_arg(ap, const char *);
            if (!body) body = "(null)";
            while (body[blen] && (prec < 0 || blen < prec)) blen++;
            break;
        case '%':
            tmp[0] = '%';
            blen = 1;
            break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            u32 v;
            u32 base = 10;
            int upper = 0, neg = 0;
            if (c == 'd' || c == 'i') {
                s32 sv = va_arg(ap, s32);
                if (sv < 0) { neg = 1; v = (u32)0 - (u32)sv; } else v = (u32)sv;
            } else {
                v = va_arg(ap, u32);
                if (c == 'x' || c == 'p') base = 16;
                else if (c == 'X') { base = 16; upper = 1; }
                else if (c == 'o') base = 8;
            }
            if (c == 'p') { prec = 8; flags |= F_ALT; }
            int nonzero = (v != 0);
            int i = sizeof tmp;
            do {
                u32 d = v % base;
                tmp[--i] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
                v /= base;
            } while (v);
            body = &tmp[i];
            blen = (int)sizeof tmp - i;
            if (prec > blen) zeros = prec - blen;
            if (neg) prefix = "-";
            else if ((flags & F_ALT) && base == 16 && (nonzero || c == 'p')) prefix = upper ? "0X" : "0x";
            numeric = 1;
            break;
        }
        default:
            out_ch(&o, '%');
            if (c) out_ch(&o, c); else fmt--;
            continue;
        }

        int plen = (int)strlen(prefix);
        int total = plen + zeros + blen;
        int zpad = (flags & F_ZERO) && !(flags & F_LEFT) && numeric && prec < 0;
        if (!(flags & F_LEFT) && !zpad) out_pad(&o, ' ', width - total);
        for (const char *p = prefix; *p; p++) out_ch(&o, *p);
        if (zpad) out_pad(&o, '0', width - total);
        out_pad(&o, '0', zeros);
        for (int i = 0; i < blen; i++) out_ch(&o, body[i]);
        if (flags & F_LEFT) out_pad(&o, ' ', width - total);
    }

    if (size) buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

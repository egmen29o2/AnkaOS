/* AnkaOS - VGA metin modu sürücüsü (80x25, 0xB8000) */
#include <kernel.h>

#define VGA_W 80
#define VGA_H 25

static volatile u16 *const vga_mem = (volatile u16 *)0xB8000;
static int cx, cy;
static u8 attr = 0x07;

void vga_sync_cursor(void)
{
    u16 pos = (u16)(cy * VGA_W + cx);
    outb(0x3D4, 14);
    outb(0x3D5, (u8)(pos >> 8));
    outb(0x3D4, 15);
    outb(0x3D5, (u8)(pos & 0xFF));
}

void vga_clear(void)
{
    for (int i = 0; i < VGA_W * VGA_H; i++) vga_mem[i] = (u16)((attr << 8) | ' ');
    cx = cy = 0;
    vga_sync_cursor();
}

void vga_init(void)
{
    outb(0x3D4, 0x0A); outb(0x3D5, 14);     /* imleç: alt çizgi biçimi */
    outb(0x3D4, 0x0B); outb(0x3D5, 15);
    vga_clear();
}

static void scroll(void)
{
    for (int i = 0; i < VGA_W * (VGA_H - 1); i++) vga_mem[i] = vga_mem[i + VGA_W];
    for (int i = VGA_W * (VGA_H - 1); i < VGA_W * VGA_H; i++)
        vga_mem[i] = (u16)((attr << 8) | ' ');
    cy = VGA_H - 1;
}

void vga_putc(char c)
{
    switch (c) {
    case '\n': cx = 0; cy++; break;
    case '\r': cx = 0; break;
    case '\b': if (cx > 0) cx--; break;
    case '\t': cx = (cx + 8) & ~7; break;
    default:
        vga_mem[cy * VGA_W + cx] = (u16)((attr << 8) | (u8)c);
        cx++;
        break;
    }
    if (cx >= VGA_W) { cx = 0; cy++; }
    if (cy >= VGA_H) scroll();
}

void vga_set_color(u8 fg, u8 bg) { attr = (u8)((bg << 4) | (fg & 0x0F)); }
u8   vga_get_attr(void)          { return attr; }
void vga_set_attr(u8 a)          { attr = a; }

/* tüm ekranın rengini değiştir (metin korunur) */
void vga_recolor(u8 a)
{
    attr = a;
    for (int i = 0; i < VGA_W * VGA_H; i++)
        vga_mem[i] = (u16)((vga_mem[i] & 0x00FF) | (a << 8));
}

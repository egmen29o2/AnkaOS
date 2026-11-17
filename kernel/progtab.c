/* AnkaOS - gömülü program tablosu */
#include <kernel.h>

#define DECL(n) extern const u8 prog_##n##_start[], prog_##n##_end[]
DECL(hello); DECL(primes); DECL(guess); DECL(crash);

#define ENT(n) { #n, prog_##n##_start, prog_##n##_end }
const struct prog progs[] = { ENT(hello), ENT(primes), ENT(guess), ENT(crash) };
const int nprogs = (int)ARRAY_SIZE(progs);

/* primes [limit] - CPU'yu yoran asal sayma; önleyici çoklu görevi göstermek için ('&' ile deneyin) */
#include "ulib.h"

int main(int argc, char **argv)
{
    u32 limit = argc > 1 ? (u32)atoi(argv[1]) : 20000;
    if (limit < 2) limit = 2;
    u32 count = 0, last = 0, step = limit / 5, next = step, t0 = uptime_ms();
    if (!step) step = next = 1;

    for (u32 n = 2; n <= limit; n++) {
        int prime = 1;
        for (u32 d = 2; d * d <= n; d++)
            if (n % d == 0) { prime = 0; break; }
        if (prime) { count++; last = n; }
        if (n >= next) {
            printf("  [pid %d] n=%u, primes so far: %u\n", getpid(), n, count);
            next += step;
        }
    }
    printf("[pid %d] %u primes up to %u (largest %u) in %u ms\n",
           getpid(), count, limit, last, uptime_ms() - t0);
    return 0;
}

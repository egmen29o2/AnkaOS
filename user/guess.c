/* guess [sayı] - sayı tahmin oyunu; stdin'den satır okur (read sistem çağrısı) */
#include "ulib.h"

int main(int argc, char **argv)
{
    int secret = argc > 1 ? atoi(argv[1]) : (int)(((uptime_ms() + 7) * 2654435761u >> 16) % 100) + 1;
    printf("I am thinking of a number between 1 and 100. Type 'q' to quit.\n");
    for (int tries = 1;; tries++) {
        printf("guess #%d> ", tries);
        char buf[32];
        int n = read(0, buf, sizeof buf - 1);
        if (n <= 0) { printf("\nbye\n"); return 1; }
        buf[n] = 0;
        if (buf[0] == 'q') { printf("The number was %d. Bye!\n", secret); return 0; }
        int g = atoi(buf);
        if (g < secret)      printf("  higher\n");
        else if (g > secret) printf("  lower\n");
        else { printf("  correct! You needed %d tries.\n", tries); return 0; }
    }
}

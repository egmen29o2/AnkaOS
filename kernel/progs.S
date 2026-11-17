/* Gömülü kullanıcı programları (build/user/*.bin) - /bin altına ramfs'e kopyalanır */
    .macro PROG name
    .global prog_\name\()_start
    .global prog_\name\()_end
prog_\name\()_start:
    .incbin "build/user/\name\().bin"
prog_\name\()_end:
    .endm

    .section .rodata
    .balign 16
    PROG hello
    PROG primes
    PROG guess
    PROG crash

    .section .note.GNU-stack,"",@progbits

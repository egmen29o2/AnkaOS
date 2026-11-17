/* AnkaOS - sistem çağrıları (int 0x80). eax=numara, ebx/ecx/edx=argümanlar. */
#include <kernel.h>

static int user_ok(u32 addr, u32 len, int write)
{
    return vmm_user_ok(current->pd, addr, len, write);
}

void syscall_handler(struct regs *r)
{
    switch (r->eax) {
    case SYS_EXIT:
        task_exit((int)r->ebx);

    case SYS_WRITE: {
        u32 fd = r->ebx, buf = r->ecx, len = r->edx;
        if ((fd != 1 && fd != 2) || len > 4096 || !user_ok(buf, len, 0)) { r->eax = (u32)-1; break; }
        console_write((const char *)buf, (int)len);
        r->eax = len;
        break;
    }
    case SYS_READ: {                                   /* fd 0: satır okur (konsol, echo'lu) */
        u32 fd = r->ebx, buf = r->ecx, len = r->edx;
        if (fd != 0 || len == 0 || len > 4096 || !user_ok(buf, len, 1)) { r->eax = (u32)-1; break; }
        char line[128];
        int n = console_readline(line, sizeof line - 1, 0);
        if (n < 0) { r->eax = 0; break; }              /* Ctrl-C / EOF */
        line[n++] = '\n';
        if ((u32)n > len) n = (int)len;
        memcpy((void *)buf, line, (size_t)n);
        r->eax = (u32)n;
        break;
    }
    case SYS_GETPID: r->eax = (u32)current->pid; break;
    case SYS_SLEEP:  task_sleep(r->ebx); r->eax = 0; break;
    case SYS_YIELD:  task_yield(); r->eax = 0; break;
    case SYS_UPTIME: r->eax = uptime_ms(); break;
    default:         r->eax = (u32)-1; break;           /* ENOSYS */
    }
}

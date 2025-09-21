// 标准输出和报错机制
#include <stdarg.h>
#include "lib/print.h"
#include "lib/lock.h"
#include "dev/uart.h"

volatile int panicked = 0;

static struct {
    struct spinlock lock;
    int locking;
} pr;

static char digits[] = "0123456789abcdef";

void print_init(void)
{
    spinlock_init(&pr.lock, "pr");
    pr.locking = 1;
}

static void
printint(int xx, int base, int sign)
{
    char buf[16];
    int i;
    unsigned int x;


    if(sign && (sign = xx < 0))
        x = -xx;
    else
        x = xx;

    i = 0;
    do {
        buf[i++] = digits[x % base];
    } while((x /= base) != 0);

    if(sign)
        buf[i++] = '-';

    while(--i >= 0)
        uart_putc_sync(buf[i]);
}

// Print to the console. only understands %d, %x, %p, %s.
void printf(const char *fmt, ...)
{
    va_list ap;
    int i, c, locking;
    char *s;

    locking = pr.locking;
    if(locking)
        spinlock_acquire(&pr.lock);

    if (fmt == 0)
        panic("null fmt");

    va_start(ap, fmt);
    for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
        if(c != '%'){
            uart_putc_sync(c);
            continue;
        }
        c = fmt[++i] & 0xff;
        if(c == 0)
            break;
        switch(c){
        case 'd':
            printint(va_arg(ap, int), 10, 1);
            break;
        case 's':
            if((s = va_arg(ap, char*)) == 0)
                s = "(null)";
            for(; *s; s++)
                uart_putc_sync(*s);
            break;
        case '%':
            uart_putc_sync('%');
            break;
        default:
        // Print unknown % sequence to draw attention.
            uart_putc_sync('%');
            uart_putc_sync(c);
            break;
        }
    }
    va_end(ap);

    if(locking)
        spinlock_release(&pr.lock);
}

void panic(const char *s)
{
    pr.locking = 0;
    printf("panic: ");
    printf(s);
    printf("\n");
    panicked = 1; // freeze uart output from other CPUs
    for(;;)
        ;
}

void assert(bool condition, const char* warning)
{
    if (!condition) {
        printf("assert failed: %s", warning);
        panic("assertion failure");
    }
}


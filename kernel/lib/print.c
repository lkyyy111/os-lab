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
printint(uint64 xx, int base, int sign)
{
    char buf[32]; // 足够64位地址
    int i;
    uint64 x;

    if(sign && (sign = (int64)xx < 0))
        x = -(int64)xx;
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

// 打印指针地址，前缀 "0x"
static void
printptr(uint64 ptr)
{
    uart_putc_sync('0');
    uart_putc_sync('x');

    char buf[32];
    int i = 0;
    if (ptr == 0) {
        uart_putc_sync('0');
        return;
    }
    while (ptr) {
        buf[i++] = digits[ptr % 16];
        ptr /= 16;
    }
    while (--i >= 0)
        uart_putc_sync(buf[i]);
}

// Print to the console. supports %d, %x, %p, %s.
void printf(const char *fmt, ...) {
    va_list ap;
    int locking;
    locking = pr.locking;
    if(locking)
        spinlock_acquire(&pr.lock);
    va_start(ap, fmt);

    for (int i = 0; fmt[i]; i++) {
        char c = fmt[i];
        if (c != '%') {
            uart_putc_sync(c);
            continue;
        }

        c = fmt[++i] & 0xff;    // 取格式符

        // 新增: 支持 %lx 和 %ld
        if (c == 'l') {
            c = fmt[++i] & 0xff;  // 再取下一个
            if (c == 'x') {
                printint((uint64)va_arg(ap, uint64), 16, 0);
                continue;
            } else if (c == 'd') {
                printint((uint64)va_arg(ap, uint64), 10, 1);
                continue;
            } else {
                // 未知的 %lX 组合，原样输出
                uart_putc_sync('%');
                uart_putc_sync('l');
                uart_putc_sync(c);
                continue;
            }
        }

        // 原有的格式符支持
        switch (c) {
            case 'd':
                printint((int)va_arg(ap, int), 10, 1);
                break;
            case 'x':
                printint((uint64)va_arg(ap, uint64), 16, 0);
                break;
            case 'p':
                printptr((uint64)va_arg(ap, void*));
                break;
            case 's': {
                char *s = va_arg(ap, char*);
                if (s == NULL) s = "(null)";
                for (; *s; s++) uart_putc_sync(*s);
                break;
            }
            case '%':
                uart_putc_sync('%');
                break;
            default:
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

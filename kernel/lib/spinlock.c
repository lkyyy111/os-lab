#include <stdarg.h>

#include "lib/lock.h"
#include "lib/print.h"
#include "proc/cpu.h"
#include "riscv.h"

// 带层数叠加的关中断
void push_off(void)
{
    int old = intr_get();

    intr_off();
    cpu_t *c = mycpu();
    if (c->noff == 0)
        c->intena = old;
    c->noff += 1;
}

// 带层数叠加的开中断
void pop_off(void)
{
    cpu_t *c = mycpu();
    if (intr_get())
        panic("pop_off - interruptible");
    if (c->noff < 1)
        panic("pop_off");
    c->noff -= 1;
    if (c->noff == 0 && c->intena)
        intr_on();
}

// 是否持有自旋锁（中断应当是关闭的）
bool spinlock_holding(spinlock_t *lk)
{
    if (lk == NULL) return false;
    return (lk->locked && lk->cpu == mycpu());
}

// 自旋锁初始化
void spinlock_init(spinlock_t *lk, char *name)
{
    lk->name = name;
    lk->locked = 0;
    lk->cpu = NULL;
}

// 获取自旋锁
void spinlock_acquire(spinlock_t *lk)
{
    if (lk == NULL) {
        void *caller = __builtin_return_address(0);
        printf("panic: spinlock_acquire NULL, caller=%p\n", caller);
        panic("spinlock_acquire NULL");
    }
    push_off();
    if (spinlock_holding(lk))
        panic("spinlock_acquire");
    while (__sync_lock_test_and_set(&lk->locked, 1) != 0)
        ;
    lk->cpu = mycpu();
}

// 释放自旋锁
void spinlock_release(spinlock_t *lk)
{
    if (lk == NULL) panic("spinlock_release NULL");
    if (!spinlock_holding(lk))
        panic("spinlock_release");
    lk->cpu = NULL;
    // 原子释放
    __sync_lock_release(&lk->locked);
    pop_off();
}
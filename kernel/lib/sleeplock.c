#include "common.h"
#include "lib/lock.h"
#include "proc/proc.h" // 必须包含，因为需要 sleep 和 wakeup
#include "proc/cpu.h"

// 初始化睡眠锁
void sleeplock_init(sleeplock_t *lk, char *name)
{
    spinlock_init(&lk->lk, "sleep lock");
    lk->name = name;
    lk->locked = 0;
    lk->pid = 0;
}

// 获取睡眠锁
void sleeplock_acquire(sleeplock_t *lk)
{
    // 1. 获取内部自旋锁，保护 locked 字段
    spinlock_acquire(&lk->lk);
    
    // 2. 如果锁已经被别人持有，则睡眠等待
    while(lk->locked) {
        proc_sleep(lk, &lk->lk); 
    }
    
    // 3. 抢到锁了
    lk->locked = 1;
    // 如果当前没有进程（内核初始化阶段），不要解引用 myproc()
    if (myproc() != NULL)
        lk->pid = myproc()->pid; // 记录持有者，方便调试
    else
        lk->pid = 0;
    
    // 4. 释放内部自旋锁
    spinlock_release(&lk->lk);
}

// 释放睡眠锁
void sleeplock_release(sleeplock_t *lk)
{
    // 1. 获取内部自旋锁
    spinlock_acquire(&lk->lk);
    
    // 2. 释放逻辑
    lk->locked = 0;
    lk->pid = 0;
    
    // 3. 唤醒所有在该锁上等待的进程
    // 它们会在 acquire 的 while 循环中被唤醒，尝试再次抢锁
    proc_wakeup(lk);
    
    // 4. 释放内部自旋锁
    spinlock_release(&lk->lk);
}

// 检查当前进程是否持有睡眠锁
bool sleeplock_holding(sleeplock_t *lk)
{
    bool r;
    spinlock_acquire(&lk->lk);
    if (myproc() != NULL)
        r = lk->locked && (lk->pid == myproc()->pid);
    else
        r = lk->locked && (lk->pid == 0); // 若无进程，则只有 pid==0 表示内核持有（可选）
    spinlock_release(&lk->lk);
    return r;
}

#include "lib/lock.h"
#include "lib/print.h"
#include "dev/timer.h"
#include "memlayout.h"
#include "riscv.h"
#include "proc/cpu.h"

/*-------------------- 工作在M-mode --------------------*/

// in trap.S M-mode时钟中断处理流程()
extern void timer_vector();

// 每个CPU在时钟中断中需要的临时空间(考虑为什么可以这么写)
static uint64 mscratch[NCPU][5];

// 时钟初始化
// called in start.c
void timer_init()
{
    int hartid = mycpuid();

    // 1. 设置 M-mode trap 向量地址为 timer_vector
    w_mtvec((uint64)timer_vector);

    // 2. 计算当前 hart 的 CLINT_MTIMECMP 寄存器地址
    uint64 mtimecmp_addr = CLINT_MTIMECMP(hartid);

    // 3. 将 mtimecmp 初始值设置为 mtime + INTERVAL
    uint64 now = *(volatile uint64*)CLINT_MTIME;
    *(volatile uint64*)mtimecmp_addr = now + INTERVAL;

    // 4. 设置 mscratch 数组内容
    // trap.S 中 timer_vector 会用到这些值
    mscratch[hartid][3] = mtimecmp_addr;
    mscratch[hartid][4] = INTERVAL;

    // 5. 把 mscratch 地址写入 mscratch CSR
    w_mscratch((uint64)&mscratch[hartid][0]);

    // 6. 开启 M-mode Machine Timer Interrupt (MTIE)
    w_mie(r_mie() | MIE_MTIE);

    // 7. 开启全局 M-mode 中断 (MSTATUS_MIE)
    w_mstatus(r_mstatus() | MSTATUS_MIE);
}


/*--------------------- 工作在S-mode --------------------*/

// 系统时钟
static timer_t sys_timer;

// 时钟创建(初始化系统时钟)
void timer_create()
{
    // 初始化锁
    spinlock_init(&sys_timer.lk, "sys_timer");
    sys_timer.ticks = 0;
}

// 时钟更新(ticks++ with lock)
void timer_update()
{
    spinlock_acquire(&sys_timer.lk);
    sys_timer.ticks++;
    spinlock_release(&sys_timer.lk);
}

// 返回系统时钟ticks
uint64 timer_get_ticks()
{
    uint64 t;
    spinlock_acquire(&sys_timer.lk);
    t = sys_timer.ticks;
    spinlock_release(&sys_timer.lk);
    return t;
}
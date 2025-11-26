#include "lib/print.h"
#include "dev/timer.h"
#include "dev/uart.h"
#include "dev/plic.h"
#include "trap/trap.h"
#include "proc/cpu.h"
#include "memlayout.h"
#include "riscv.h"

// 中断信息
static char* interrupt_info[16] = {
    "U-mode software interrupt",      // 0
    "S-mode software interrupt",      // 1
    "reserved-1",                     // 2
    "M-mode software interrupt",      // 3
    "U-mode timer interrupt",         // 4
    "S-mode timer interrupt",         // 5
    "reserved-2",                     // 6
    "M-mode timer interrupt",         // 7
    "U-mode external interrupt",      // 8
    "S-mode external interrupt",      // 9
    "reserved-3",                     // 10
    "M-mode external interrupt",      // 11
    "reserved-4",                     // 12
    "reserved-5",                     // 13
    "reserved-6",                     // 14
    "reserved-7",                     // 15
};

// 异常信息
static char* exception_info[16] = {
    "Instruction address misaligned", // 0
    "Instruction access fault",       // 1
    "Illegal instruction",            // 2
    "Breakpoint",                     // 3
    "Load address misaligned",        // 4
    "Load access fault",              // 5
    "Store/AMO address misaligned",   // 6
    "Store/AMO access fault",         // 7
    "Environment call from U-mode",   // 8
    "Environment call from S-mode",   // 9
    "reserved-1",                     // 10
    "Environment call from M-mode",   // 11
    "Instruction page fault",         // 12
    "Load page fault",                // 13
    "reserved-2",                     // 14
    "Store/AMO page fault",           // 15
};

// in trap.S
// 内核中断处理流程
extern void kernel_vector();

// 初始化trap中全局共享的东西
void trap_kernel_init()
{
    // 设置 S-mode trap 向量入口地址到 kernel_vector (trap.S)
    w_stvec((uint64)kernel_vector);

    // 初始化 PLIC 全局优先级
    plic_init();
}

// 各个核心trap初始化
void trap_kernel_inithart()
{
    // 使能 PLIC 对该 hart 的 IRQ 接收
    plic_inithart();

    // 开启 S-mode 中断，使得该 hart 可以响应 SIP/SIE
    intr_on();
}

// 外设中断处理 (基于PLIC)
void external_interrupt_handler()
{
    // 获取当前挂起的最高优先级外部中断 ID
    int irq = plic_claim();

    if (irq == UART_IRQ) {
        // 调用 UART 中断处理函数
        uart_intr();
    } else if (irq) {
        // 如果不是 UART，简单提示一下
        printf("unexpected external irq=%d", irq);
    }

    // 完成中断处理，通知 PLIC
    if (irq)
        plic_complete(irq);
}

// 时钟中断处理 (基于CLINT)
void timer_interrupt_handler()
{
    // 更新唯一的全局系统 tick
    timer_update();
    uint64 t = timer_get_ticks();

    // 测试输出一个滴答字符
    // 可以注释掉这行防止刷屏
    //uart_putc_sync('T');
    if (r_tp() == 0 && t % 10 == 0) { // 每隔10个tick输出一次
        printf("tick=%lu\n", t);
    }
}

// 在kernel_vector()里面调用
// 内核态trap处理的核心逻辑
void trap_kernel_handler()
{
    uint64 sepc = r_sepc();          // 记录了发生异常时的pc值
    uint64 sstatus = r_sstatus();    // 与特权模式和中断相关的状态信息
    uint64 scause = r_scause();      // 引发trap的原因
    uint64 stval = r_stval();        // 发生trap时保存的附加信息(不同trap不一样)

    // 确认trap来自S-mode且此时trap处于关闭状态
    assert(sstatus & SSTATUS_SPP, "trap_kernel_handler: not from s-mode");
    assert(intr_get() == 0, "trap_kernel_handler: interreput enabled");

    int is_interrupt = (scause >> 63) & 1;
    int trap_id = scause & 0xf; 

    // 中断异常处理核心逻辑
    if (is_interrupt) {
        // 根据中断 ID 分类处理
        switch (trap_id) {
        case 1: // S-mode软件中断 (由 timer_vector 投递的)
            timer_interrupt_handler();
            // 清除软件中断挂起位 SSIP
            w_sip(r_sip() & ~2);
            break;
        case 9: // S-mode外部中断 (PLIC管理)
            external_interrupt_handler();
            break;
        default:
            printf("unexpected interrupt: %s",(trap_id < 16) ? interrupt_info[trap_id] : "unknown");
            panic("trap_kernel_handler: unexpected interrupt");
        }
    } else {
        // 异常处理（这里按要求直接报错卡死）
        printf("kernel exception: %s",(trap_id < 16) ? exception_info[trap_id] : "unknown");
        printf("sepc=0x%lx stval=0x%lx", sepc, stval);
        panic("trap_kernel_handler: unexpected exception");
    }

    // trap 返回后继续执行原来的指令
    w_sepc(sepc);
}
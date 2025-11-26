#include "lib/print.h"
#include "trap/trap.h"
#include "proc/cpu.h"
#include "mem/vmem.h"
#include "memlayout.h"
#include "riscv.h"
#include "sys.h"

// in trampoline.S
extern char trampoline[];      // 内核和用户切换的代码
extern char user_vector[];     // 用户触发trap进入内核
extern char user_return[]; // trap处理完毕返回用户

// in trap.S
extern char kernel_vector[];   // 内核态trap处理流程

// in trap_kernel.c
extern char* interrupt_info[16]; // 中断错误信息
extern char* exception_info[16]; // 异常错误信息

// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
void trap_user_handler()
{
    uint64 sepc    = r_sepc();       // 异常发生的用户PC
    uint64 sstatus = r_sstatus();    // SPP等状态位
    uint64 scause  = r_scause();     // trap原因
    uint64 stval   = r_stval();      // 额外信息

    proc_t* p = myproc();

    // 1. 确认来自 U 模式
    assert((sstatus & SSTATUS_SPP) == 0, "trap_user_handler: not from u-mode");

    // 2. 切换 STVEC 到内核 trap 向量，以便后续内核中断用
    w_stvec((uint64)kernel_vector);

    // 3. 保存用户PC到trapframe
    p->tf->epc = sepc;

    // 4. 调试打印
    printf("[DEBUG] trap_user_handler entered: scause=%lu, pid=%d", scause, p->pid);

    // 5. 系统调用处理
    if (scause == 8) {  // Environment call from U-mode
        p->tf->epc += 4; // 跳过 ecall
        intr_on();
        printf("get a syscall from proc %d", p->pid);

        // 保留你的写法：执行用户态 syscall(SYS_print)
        syscall(SYS_print);

    } else if (scause == 0x8000000000000009L) {
        // 外部中断
        external_interrupt_handler();
    } else if (scause == 0x8000000000000005L) {
        // 时钟中断
        timer_interrupt_handler();
    } else {
        // 未知trap
        printf("unexpected scause=0x%lx sepc=0x%lx stval=0x%lx", scause, sepc, stval);
    }

    // 6. 返回用户态
    trap_user_return();
}

void trap_user_return()
{
    proc_t *p = myproc();

    // 关中断，防止过程中被打断
    intr_off();

    // 设置 STVEC 为 trampoline.S 的 user_vector（虚拟地址）
    uint64 trampoline_uservec = TRAMPOLINE + (user_vector - trampoline);
    w_stvec(trampoline_uservec);

    // trapframe 中填充返回用户态时需要的值
    p->tf->kernel_satp   = MAKE_SATP(kernel_pgtbl); // 内核页表
    p->tf->kernel_sp     = p->kstack + PGSIZE;      // 内核栈顶
    p->tf->kernel_trap   = (uint64)trap_user_handler;
    p->tf->kernel_hartid = r_tp();                  // 当前 hartid

    w_satp(MAKE_SATP(p->pgtbl));

    // 设置 SSTATUS，返回到 U 模式
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP;  // SPP=0 → 返回用户模式
    x |= SSTATUS_SPIE;  // 开启用户模式中断
    w_sstatus(x);

    // SEPC 指向用户态的运行地址
    w_sepc(p->tf->epc);

    // 跳到 trampoline.S 的 user_return，真正切到U态
    uint64 trampoline_userret = TRAMPOLINE + (user_return - trampoline);
    ((void (*)(uint64,uint64))trampoline_userret)(p->tf->sp, MAKE_SATP(p->pgtbl));
}

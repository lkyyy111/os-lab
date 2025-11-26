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
void trap_user_handler() {
    uint64 sepc = r_sepc();          // 异常发生时的 PC
    uint64 sstatus = r_sstatus();    // 与特权模式/中断相关的状态
    uint64 scause = r_scause();      // trap 原因
    uint64 stval = r_stval();        // trap 附加信息

    // 确认 trap 来自 U-mode（SSTATUS_SPP == 0）
    if ((sstatus & SSTATUS_SPP) != 0) {
        panic("trap_user_handler: not from u-mode");
    }

    // 切换 stvec 到内核模式 trap 入口
    w_stvec((uint64)kernel_vector);

    proc_t* p = myproc();

    // 保存用户 PC
    p->tf->epc = sepc;
    printf("[DEBUG] trap_user_handler entered, scause=%lu, pid=%d", scause, p->pid);

    if (scause == 8) {  // system call
        p->tf->epc += 4; // 跳过 ecall 指令
        intr_on();
        printf("get a syscall from proc %d", p->pid);
        syscall(SYS_print);

    } else if (scause == 0x8000000000000009L) {
        // external interrupt (SEI)
        external_interrupt_handler();

    } else if (scause == 0x8000000000000005L) {
        // timer interrupt (STI)
        timer_interrupt_handler();

    } else {
        // 未知 trap
        printf("unexpected scause=0x%lx sepc=0x%lx stval=0x%lx", scause, sepc, stval);
    }

    // 处理完 trap，返回用户态
    trap_user_return();
}

void prepare_return(void) {
    proc_t *p = myproc();

    // 正准备切换 stvec 到用户态入口时，要关中断
    intr_off();

    // stvec 指向 trampoline.S 的 user_vector（虚拟地址）
    uint64 trampoline_uservec = TRAMPOLINE + (user_vector - trampoline);
    w_stvec(trampoline_uservec);

    // trapframe 填充返回用户态时需要的值
    p->tf->kernel_satp  = r_satp();              // 内核页表
    p->tf->kernel_sp    = p->kstack + PGSIZE;    // 内核栈顶
    p->tf->kernel_trap  = (uint64)trap_user_handler;
    p->tf->kernel_hartid = r_tp();               // 当前 hartid

    // 设置 SSTATUS 寄存器，返回 U-mode
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP;  // SPP 置 0 → 返回用户模式
    x |= SSTATUS_SPIE;  // 开启用户模式中断
    w_sstatus(x);

    // SEPC → 用户态的返回地址（用户 PC）
    w_sepc(p->tf->epc);
}


// 调用user_return()
// 内核态返回用户态
void trap_user_return() {
    proc_t *p = myproc();

    prepare_return();

    uint64 satp = MAKE_SATP(p->pgtbl); // 用户页表的 satp
    // 计算 user_return 在 TRAMPOLINE 虚拟页的绝对虚拟地址
    uint64 trampoline_userret = TRAMPOLINE + (user_return - trampoline);

    // 调用这个地址（实际上是跳转到 trampoline.S 的 user_return）
    ((void (*)(uint64))trampoline_userret)(satp);
}

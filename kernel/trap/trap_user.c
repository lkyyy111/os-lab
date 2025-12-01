#include "lib/print.h"
#include "trap/trap.h"
#include "proc/cpu.h"
#include "mem/vmem.h"
#include "memlayout.h"
#include "riscv.h"

// in trampoline.S
extern char trampoline[];      // 内核和用户切换的代码
extern char user_vector[];     // 用户触发trap进入内核
extern char user_return[];     // trap处理完毕返回用户

// in trap.S
extern char kernel_vector[];   // 内核态trap处理流程

extern void external_interrupt_handler(); // 引用内核的中断处理函数
// in trap_kernel.c
//extern char* interrupt_info[16]; // 中断错误信息
//extern char* exception_info[16]; // 异常错误信息

// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
void trap_user_handler()
{
    uint64 sepc = r_sepc();          // 记录了发生异常时的pc值
    uint64 sstatus = r_sstatus();    // 与特权模式和中断相关的状态信息
    uint64 scause = r_scause();      // 引发trap的原因
    uint64 stval = r_stval();        // 发生trap时保存的附加信息(不同trap不一样)
    proc_t* p = myproc();

    // 确认trap来自U-mode
    assert((sstatus & SSTATUS_SPP) == 0, "trap_user_handler: not from u-mode");
    w_stvec((uint64)kernel_vector);

    p->tf->epc = sepc;
    int trap_id = scause & 0xf;
    int is_interrupt = (scause >> 63) & 1;
    if (is_interrupt) {
        // 处理中断
        // 这里的逻辑可以参考 trap_kernel_handler，但通常用户态的时钟中断意味着要 yield
        // 暂时只需处理 timer 相关的清除操作，防止死循环
        switch (trap_id) {
            case 1: // S-mode software interrupt (Timer)
                // 清除 pending 位
                w_sip(r_sip() & ~2);
                // 可以在这里调用 yield() 进行进程调度
                break;
            case 9: // S-mode external interrupt
                // 处理外部中断 (PLIC)
                // 注意：通常外部中断由 plic_claim 处理，具体依赖你的外部中断实现
                // 既然 trap_kernel.c 里有 external_interrupt_handler，这里也可以调用它
                // 或者简单地打印一下
                external_interrupt_handler(); 
                break;
            default:
                printf("unexpected interrupt in user mode: %d", trap_id);
                break;
        }
    } else {
        // 处理异常
        if (trap_id == 8) { 
            // Environment call from U-mode (系统调用)
            printf("get a syscall from proc %d", p->pid);

            // ！！！关键：跳过 ecall 指令！！！
            p->tf->epc += 4;

            // 开启中断（系统调用通常允许中断）
            intr_on();
        } else {
            // 其他异常（如缺页、非法指令等）
            printf("usertrap: unexpected exception code %d", trap_id);
            printf("sepc=%p stval=%p pid=%d", sepc, stval, p->pid);
            panic("trap_user_handler: unexpected exception");
        }
    }

    // 4. 返回用户态
    trap_user_return();

}

// 调用user_return()
// 内核态返回用户态
void trap_user_return()
{
    proc_t* p = myproc();

    // 1. 关闭中断
    // 下面的操作涉及 CSR 修改和页表切换，必须原子
    intr_off();

    w_sie(r_sie() & ~(1L << 9)); 

    // 2. 设置 stvec 指向 trampoline 中的 user_vector
    // 这样下一次用户态 trap 就会跳到 trampoline 执行
    // 计算 user_vector 在 trampoline 页面的虚拟地址
    uint64 trampoline_user_vec = TRAMPOLINE + (user_vector - trampoline);
    w_stvec(trampoline_user_vec);

    // 3. 填充 trapframe 中的 kernel_* 信息
    // 让 trampoline.S 知道下次怎么进内核
    p->tf->kernel_satp = r_satp();
    p->tf->kernel_sp = p->kstack + PGSIZE; // 指向内核栈顶
    p->tf->kernel_trap = (uint64)trap_user_handler;
    p->tf->kernel_hartid = r_tp();

    // 4. 设置 sstatus
    // SSTATUS_SPP = 0 (返回 User Mode)
    // SSTATUS_SPIE = 1 (用户态中断开启)
    uint64 x = r_sstatus();
    x &= ~SSTATUS_SPP; 
    x |= SSTATUS_SPIE; 
    w_sstatus(x);

    // 5. 设置 sepc 为用户程序继续执行的地址
    w_sepc(p->tf->epc);

    // 6. 计算用户页表的 satp 值
    // Sv39模式 + 页表物理地址
    uint64 satp = MAKE_SATP(p->pgtbl);

    // 7. 跳转到 trampoline 中的 user_return
    // 计算 user_return 在 trampoline 页面的虚拟地址
    uint64 trampoline_user_ret = TRAMPOLINE + (user_return - trampoline);
    
    // 强制转换并调用
    // 参数 a0: trapframe 虚拟地址 (TRAPFRAME)
    // 参数 a1: 用户页表 satp
    ((void (*)(uint64, uint64))trampoline_user_ret)(TRAPFRAME, satp);
}
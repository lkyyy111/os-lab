#include "dev/uart.h"
#include "lib/print.h"
#include "trap/trap.h"
#include "dev/timer.h"
#include "riscv.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "lib/str.h"
#include "common.h"
#include "proc/proc.h" // 必须包含这个头文件以调用 proc_make_first

// 多核同步标志
volatile static int started = 0;

int main()
{
    int cpuid = r_tp();

    if (cpuid == 0) {
        // 1. 基础硬件初始化
        uart_init();
        print_init();
        printf("[CPU%d] System booting...\n", cpuid);

        // 2. 内存初始化
        pmem_init();
        kvm_init();       // 初始化内核页表
        kvm_inithart();   // 开启分页 (satp)

        // 3. 中断与 Trap 初始化
        trap_kernel_init();     // 设置 stvec 指向 kernel_vector
        trap_kernel_inithart(); // 开启 S-mode 中断 (SIE)

        // 4. 时钟初始化
        timer_create();         // 开启时钟中断

        // 5. 进程系统初始化 (如果有 proc_init() 请在这里调用，没有则忽略)
        // proc_init(); 

        printf("[INFO] Kernel initialized. Starting first user process...\n");
        __sync_synchronize();
        started = 1;

        // 6. 创建并运行第一个用户进程
        // 注意：这个函数通常不会返回，因为通过 trap_user_return 进用户态了
        proc_make_fisrt(); 
        
        // 如果 proc_make_first 设计为会返回（例如使用了调度器），
        // 这里的代码才会执行。但在简单实验中，通常直接跳走了。
        panic("main: execution should not reach here after proc_make_first");

    } else {
        // 次核逻辑 (暂时不需要运行用户进程)
        while (started == 0);
        __sync_synchronize();
        
        kvm_inithart();         // 开启分页
        trap_kernel_inithart(); // 开启中断

        printf("[CPU%d] Second core ready (idling).\n", cpuid);
        
        // 次核进入死循环，响应时钟中断即可
        while (1) {
            asm volatile("wfi");
        }
    }

    return 0;
}

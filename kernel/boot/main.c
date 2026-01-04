#include "dev/uart.h"
#include "lib/print.h"
#include "trap/trap.h"
#include "dev/timer.h"
#include "riscv.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "mem/mmap.h"
#include "lib/str.h"
#include "common.h"
#include "proc/proc.h"
#include "dev/plic.h"

// [新增] 包含文件系统相关的头文件
#include "dev/vio.h"   // virtio_disk_init
#include "fs/buf.h"    // binit (缓冲区初始化)
#include "fs/fs.h"     // iinit (inode初始化)
#include "fs/file.h"   // file_init (文件表初始化)

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
        mmap_init();
        kvm_init();       
        kvm_inithart();   

        // 3. 进程锁初始化 (必须取消注释！)
        // 初始化 pid_lock 和所有进程槽位的锁
        proc_init();      

        // 4. 中断与 Trap 初始化
        trap_kernel_init();     
        
        // [关键新增] 5. 文件系统子系统初始化
        printf("[CPU%d] Initializing File System...\n", cpuid);
        
        plic_init();        // 初始化 PLIC (virtio 需要)
        plic_inithart();    // 开启当前核的 PLIC 响应

        // 启用当前 hart 的 trap/中断处理，确保 virtio 中断能被处理
        trap_kernel_inithart();

        virtio_disk_init(); // 初始化磁盘驱动
        fs_init();           // 初始化文件系统
        file_init();        // 初始化全局文件打开表

        // 6. 时钟初始化
        timer_create();         

        printf("[INFO] Kernel initialized. Starting first user process...\n");
        __sync_synchronize();
        started = 1;

        // 7. 创建并运行第一个用户进程
        // 注意函数名拼写修正：proc_make_fisrt -> proc_make_first
        // 请确保 proc.c 和 proc.h 中也修正了这个拼写
        proc_make_fisrt(); 

        // 8. 进入调度器循环
        proc_scheduler();
        
        panic("main: execution should not reach here");

    } else {
        // 次核初始化
        while (started == 0);
        __sync_synchronize();
        
        printf("[CPU%d] Second core starting...\n", cpuid);
        
        kvm_inithart();         
        trap_kernel_inithart(); 
        plic_inithart(); // 次核也需要响应外部中断

        // 次核也应该参与进程调度，而不是空转
        // 这样可以利用多核优势运行进程
        proc_scheduler(); 
    }

    return 0;
}

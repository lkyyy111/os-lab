#include "dev/uart.h"
#include "lib/print.h"
#include "trap/trap.h"
#include "dev/timer.h"
#include "riscv.h"
#include "proc/proc.h"       // 声明 proc_make_fisrt()
#include "trap/trap.h" // 声明 trap_user_return()

volatile static int started = 0;

int main()
{
    int cpuid = r_tp();

    if (cpuid == 0) {
        uart_init();
        print_init();
        trap_kernel_init();
        timer_create();
        trap_kernel_inithart();
        printf("[CPU%d] System booting...\n", cpuid);
        printf("[TEST] Timer interrupt (tick) and UART interrupt (echo) ready.\n");
        printf("[INFO] Type characters to see UART interrupt echo.\n");

        // 创建单个用户进程
        
        proc_make_fisrt();
        printf("222222\n");
        __sync_synchronize();
        started = 1;
    } else {
        while (started == 0);
        __sync_synchronize();
        trap_kernel_inithart();
        printf("[CPU%d] Second core ready.\n", cpuid);
    }

    return 0;
}

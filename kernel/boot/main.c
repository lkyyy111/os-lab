#include "dev/uart.h"
#include "lib/print.h"
#include "trap/trap.h"
#include "dev/timer.h"
#include "riscv.h"

// 多核同步标志
volatile static int started = 0;

int main()
{
    int cpuid = r_tp();

    if (cpuid == 0) {
        // 初始化 UART 与 printf
        uart_init();
        print_init();

        // 初始化 Trap 系统 (设置S-mode trap向量 + 初始化PLIC优先级)
        trap_kernel_init();

        // 初始化 S-mode 时钟 (ticks=0)
        timer_create();

        // 当前CPU开启PLIC使能和SIE位
        trap_kernel_inithart();

        printf("[CPU%d] System booting...\n", cpuid);
        printf("[TEST] Timer interrupt (tick) and UART interrupt (echo) ready.\n");
        printf("[INFO] Type characters to see UART interrupt echo.\n");

        __sync_synchronize();
        started = 1;

    } else {
        // 等待主核完成初始化
        while (started == 0);
        __sync_synchronize();

        // 次核也开启PLIC使能和SIE位
        trap_kernel_inithart();
        //timer_create();

        printf("[CPU%d] Second core ready.\n", cpuid);
    }

    // 主循环: 可以在这里查看tick变化或进行交互测试
    while (1) {
        // 观察tick输出
        //printf("tick=%lu", timer_get_ticks());
    }
}

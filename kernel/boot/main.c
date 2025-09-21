// main.c
#include "dev/uart.h"
#include "lib/print.h"
#include "riscv.h"
#include "proc/proc.h"

volatile int started = 0;

int main(void) {
    if(mycpuid() == 0){
        uart_init();
        print_init();
        printf("\n");
        printf("xv6 kernel is booting");
        printf("\n");
        printf("hart %d starting", mycpuid());  // 新增：显示主核启动
        printf("\n");

        __sync_synchronize();
        started = 1; // 唤醒其他 hart
    } else {
        while(started == 0)
            ;  // 等主核完成初始化（busy wait）
        __sync_synchronize();
        printf("hart %d starting\n", mycpuid());
    }

    while(1);
}

//qemu-system-riscv64 -machine virt -nographic -bios none -kernel kernel-qemu -smp 3

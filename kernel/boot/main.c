#include "dev/uart.h"
#include "lib/print.h"
#include "riscv.h"
#include "mem/pmem.h"
#include "lib/str.h"
#include "common.h"

// 多核同步变量
volatile static int started = 0;
volatile static int over_1 = 0, over_2 = 0;

// 保存分配的物理页指针
static int* mem[1024];

int main(void) {
    int cpuid = r_tp(); // 获取当前 CPU id

    if(cpuid == 0) {
        uart_init();
        print_init();
        pmem_init();

        printf("cpu %d is booting!\n", cpuid);

        __sync_synchronize();
        started = 1; // 唤醒其他核

        // ===== CPU0 分配 =====
        int alloc_count_0 = 0;
        for(int i = 0; i < 512; i++) {
            mem[i] = pmem_alloc(true);
            if(!mem[i]) break; // 分配失败直接停止
            memset(mem[i], 1, PGSIZE);
            printf("mem = %p, data = %d\n", mem[i], mem[i][0]);
            alloc_count_0++;
        }
        printf("cpu %d alloc over\n", cpuid);
        over_1 = 1;

        while(over_1 == 0 || over_2 == 0);

        // ===== CPU0 释放 =====
        for(int i = 0; i < alloc_count_0; i++) {
            pmem_free((uint64)mem[i], true);
        }
        printf("cpu %d free over\n", cpuid);

    } else {
        while(started == 0);
        __sync_synchronize();

        printf("cpu %d is booting!\n", cpuid);

        // ===== CPU1 分配 =====
        int alloc_count_1 = 0;
        for(int i = 512; i < 1024; i++) {
            mem[i] = pmem_alloc(true);
            if(!mem[i]) break;
            memset(mem[i], 1, PGSIZE);
            printf("mem = %p, data = %d\n", mem[i], mem[i][0]);
            alloc_count_1++;
        }
        printf("cpu %d alloc over\n", cpuid);
        over_2 = 1;

        while(over_1 == 0 || over_2 == 0);

        // ===== CPU1 释放 =====
        for(int i = 512; i < 512 + alloc_count_1; i++) {
            pmem_free((uint64)mem[i], true);
        }
        printf("cpu %d free over\n", cpuid);
    }

    while(1);
}
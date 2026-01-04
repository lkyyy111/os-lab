#include "common.h"
#include "fs/file.h"
#include "dev/uart.h"
#include "mem/vmem.h" // 需要 vm_getpte 的声明
#include "proc/proc.h"
#include "proc/cpu.h"
#include "riscv.h"
#include "memlayout.h"

// 辅助函数：从用户虚拟地址 src 读取一个字符
// 返回值：读取到的字符，或者 -1 表示失败
static int get_user_char(uint64 src) {
    struct proc *p = myproc();
    uint64 va = PGROUNDDOWN(src);
    uint64 offset = src - va;

    // 1. 查页表
    pte_t *pte = vm_getpte(p->pgtbl, va, false);
    
    // 2. 检查有效性 (存在 | 有效 | 用户权限)
    if (pte == NULL || !(*pte & PTE_V) || !(*pte & PTE_U)) {
        return -1;
    }

    // 3. 计算物理地址
    uint64 pa = PTE_TO_PA(*pte) + offset;
    
    // 4. 读取数据 (注意：假设内核直接映射所有物理内存)
    return *(unsigned char *)pa;
}

// --------------------------------------------------

uint32 console_write(uint32 len, uint64 src, bool user_src)
{
    int i;
    int c_int;

    for(i = 0; i < len; i++){
        if (user_src) {
            // 用户态来源：手动查页表读取
            c_int = get_user_char(src + i);
            if (c_int == -1) break; // 读取失败，停止
        } else {
            // 内核态来源：直接读取
            c_int = ((char*)src)[i];
        }

        // 发送字符
        uart_putc_sync(c_int);
    }
    
    return i;
}

// 保持不变
uint32 console_read(uint32 len, uint64 dst, bool user_dst)
{
    uint32 i = 0;
    while(i < len){
        int c = uart_getc_sync();
        if(c == -1) break; // 无数据可读，结束本次读取

        if(user_dst){
            // 将字符拷贝到用户空间
            if(uvm_copyout(myproc()->pgtbl, dst + i, (uint64)&c, 1) < 0)
                break;
        } else {
            // 直接写入内核缓冲区
            ((char*)dst)[i] = (char)c;
        }
        i++;
    }
    return i; 
}

#include "proc/cpu.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "mem/mmap.h"
#include "lib/str.h"
#include "lib/print.h"
#include "syscall/sysfunc.h"
#include "syscall/syscall.h"
#include "riscv.h"
#include "proc/proc.h"
#include "dev/timer.h"

// 堆伸缩
// uint64 new_heap_top 新的堆顶 (如果是0代表查询, 返回旧的堆顶)
// 成功返回新的堆顶 失败返回-1
uint64 sys_brk()
{
    uint64 addr;
    arg_uint64(0, &addr);
    
    proc_t* p = myproc();
    uint64 oldsz = p->heap_top; // 注意这里改成了 heap_top

    if (addr == 0) return oldsz;

    if (addr > oldsz) {
        if (uvm_heap_grow(p->pgtbl, oldsz, addr - oldsz) == 0) {
            return -1;
        }
        p->heap_top = addr; // 更新 heap_top
    } else if (addr < oldsz) {
        uvm_heap_ungrow(p->pgtbl, oldsz, oldsz - addr);
        p->heap_top = addr; // 更新 heap_top
    }

    return p->heap_top;
}

// 内存映射
// uint64 start 起始地址 (如果为0则由内核自主选择一个合适的起点, 通常是顺序扫描找到一个够大的空闲空间)
// uint32 len   范围(字节, 检查是否是page-aligned)
// 成功返回映射空间的起始地址, 失败返回-1
uint64 sys_mmap()
{
    uint64 start;
    uint32 len;

    arg_uint64(0, &start);
    arg_uint32(1, &len);

    if (len == 0) return -1;
    if (start % PGSIZE != 0) return -1;

    uint32 npages = (len + PGSIZE - 1) / PGSIZE;
    
    // 权限设置
    // 注意：initcode.c 里可能没有传 prot 参数，所以这里默认 RW
    // 同时必须加上 PTE_U 否则用户态不可访问
    int perm = PTE_R | PTE_W | PTE_U;

    // 直接调用 uvm_mmap
    // uvm_mmap 内部已经做了 pmem_alloc 和 vm_mappages
    uvm_mmap(start, npages, perm); 

    // ！！！删掉后面所有的 for 循环分配代码！！！
    
    // 加上 TLB 刷新以防万一
    asm volatile("sfence.vma");

    return start;
}

// 取消内存映射
// uint64 start 起始地址
// uint32 len   范围(字节, 检查是否是page-aligned)
// 成功返回0 失败返回-1
uint64 sys_munmap()
{
    uint64 start;
    uint32 len;

    arg_uint64(0, &start);
    arg_uint32(1, &len);

    if (len == 0) return -1;
    if (start % PGSIZE != 0) return -1;

    // 计算页数
    uint32 npages = (len + PGSIZE - 1) / PGSIZE;

    // 调用你的 uvm_munmap
    uvm_munmap(start, npages);

    return 0;
}

// 辅助函数：封装 uvm_copyin_str
static int fetch_str(uint64 addr, char *buf, int max) {
    struct proc *p = myproc();
    
    // 为了安全，先清空缓冲区，防止 uvm_copyin_str 失败导致打印乱码
    memset(buf, 0, max);
    
    // 使用你在 uvm.c 中定义的 uvm_copyin_str
    // 注意：uvm_copyin_str 返回 void，如果拷贝过程中遇到非法地址会提前返回
    // 此时 buf 可能是部分拷贝的或者是空的，但因为上面 memset 了，所以是安全的
    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, max);
    
    return strlen(buf);
}


// 打印字符
// uint64 addr
uint64 sys_print()
{
    uint64 addr;
    char buf[512]; // 缓冲区

    // 获取第0个参数
    arg_uint64(0, &addr);

    // 从用户空间拷贝字符串到内核空间
    if(fetch_str(addr, buf, sizeof(buf)) < 0)
        return -1;

    // 打印
    printf("%s", buf);
    return 0;
}

// 进程复制
uint64 sys_fork()
{
    return proc_fork();
}

// 进程等待
// uint64 addr  子进程退出时的exit_state需要放到这里 
uint64 sys_wait()
{
    uint64 addr;
    arg_uint64(0, &addr);
    return proc_wait(addr);
}

// 进程退出
// int exit_state
uint64 sys_exit()
{
    // [修复] 使用 uint64 类型的临时变量来匹配 arg_uint64 的参数要求
    uint64 tmp_state; 
    arg_uint64(0, &tmp_state);
    
    // 转换回 int 类型
    int exit_state = (int)tmp_state;
    
    proc_exit(exit_state);
    return 0; // 不可达
}

extern timer_t sys_timer;

// 进程睡眠一段时间
// uint32 second 睡眠时间
// 成功返回0, 失败返回-1
uint64 sys_sleep()
{
    uint32 n;
    uint64 ticks0;

    arg_uint32(0, &n);

    // 1. 获取定时器锁
    spinlock_acquire(&sys_timer.lk);
    
    ticks0 = sys_timer.ticks;
    
    // 2. 循环等待直到时间流逝足够
    while(sys_timer.ticks - ticks0 < n){
        // 注意：由于你的 proc 结构体没有 killed 字段，这里删除了 killed 检查
        // 如果被外部终止，该进程必须睡够时间才能退出
        
        // 3. 睡眠
        // proc_sleep 会释放锁、修改状态、调度，唤醒后重新获取锁
        proc_sleep(&sys_timer, &sys_timer.lk);
    }
    
    // 4. 释放锁
    spinlock_release(&sys_timer.lk);
    
    return 0;
}
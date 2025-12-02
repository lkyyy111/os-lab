#include "proc/cpu.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "mem/mmap.h"
#include "lib/str.h"
#include "lib/print.h"
#include "syscall/sysfunc.h"
#include "syscall/syscall.h"
#include "riscv.h"

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

    // 简单的参数检查
    if (len == 0) return -1;

    // 对齐检查：mmap 要求按页操作
    // 你的 uvm_mmap 里面有 assert(begin % PGSIZE == 0)，所以这里最好先检查
    if (start % PGSIZE != 0) {
        // 如果实验要求宽松，可以手动帮忙对齐：start = PGROUNDDOWN(start);
        // 这里按照严格模式返回错误
        return -1; 
    }

    // 计算页数 (向上取整)
    uint32 npages = (len + PGSIZE - 1) / PGSIZE;

    // 权限设置：默认给予读写权限 (PTE_R | PTE_W | PTE_U)
    // 如果你的实验需要支持 PROT 参数，请从 arg 2 读取并转换
    int perm = PTE_R | PTE_W | PTE_U; // 加上 PTE_U 确保用户可访问

    // 调用你的 uvm_mmap
    // 注意：uvm_mmap 返回 void，若失败内部 panic (根据你提供的代码)
    // 为了防止 panic 导致内核崩溃，你可能需要在 uvm_mmap 里把 panic 改成返回错误码
    // 但目前按你提供的代码直接调用：
    uvm_mmap(start, npages, perm);

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

// copyin 测试 (int 数组)
// uint64 addr
// uint32 len
// 返回 0
uint64 sys_copyin()
{
    proc_t* p = myproc();
    uint64 addr;
    uint32 len;

    arg_uint64(0, &addr);
    arg_uint32(1, &len);

    int tmp;
    for(int i = 0; i < len; i++) {
        uvm_copyin(p->pgtbl, (uint64)&tmp, addr + i * sizeof(int), sizeof(int));
        printf("get a number from user: %d\n", tmp);
    }

    return 0;
}

// copyout 测试 (int 数组)
// uint64 addr
// 返回数组元素数量
uint64 sys_copyout()
{
    int L[5] = {1, 2, 3, 4, 5};
    proc_t* p = myproc();
    uint64 addr;

    arg_uint64(0, &addr);
    uvm_copyout(p->pgtbl, addr, (uint64)L, sizeof(int) * 5);

    return 5;
}

// copyinstr测试
// uint64 addr
// 成功返回0
uint64 sys_copyinstr()
{
    char s[64];

    arg_str(0, s, 64);
    printf("get str from user: %s\n", s);

    return 0;
}

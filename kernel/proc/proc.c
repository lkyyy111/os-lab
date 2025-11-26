#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "proc/initcode.h"
#include "memlayout.h"
#include "trap/trap.h"


// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap_user.c
extern void trap_user_return();


// 第一个进程
static proc_t proczero;

// 获得一个初始化过的用户页表
// 完成了trapframe 和 trampoline 的映射
pgtbl_t proc_pgtbl_init(uint64 trapframe_va)
{
   // 用户页表分配在 user_region
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(false);
    if (!pgtbl) {
        printf("proc_pgtbl_init: alloc pagetable failed");
        return NULL;
    }
    memset(pgtbl, 0, PGSIZE);

    // 映射 trapframe（用户可读写 — 用物理地址映射）
    vm_mappages(pgtbl,
                trapframe_va,
                (uint64)proczero.tf, // trapframe的物理地址
                PGSIZE,
                PTE_R | PTE_W | PTE_U);

    // 映射 trampoline（用户可执行，只读）
    vm_mappages(pgtbl,
                TRAMPOLINE,
                (uint64)trampoline,  // trampolines在内核段
                PGSIZE,
                PTE_R | PTE_X | PTE_U);

    return pgtbl;
}

/*
    第一个用户态进程的创建
    它的代码和数据位于initcode.h的initcode数组

    第一个进程的用户地址空间布局:
    trapoline   (1 page)
    trapframe   (1 page)
    ustack      (1 page)
    .......
                        <--heap_top
    code + data (1 page)
    empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问
*/
void proc_make_fisrt()
{
    // 1. 设置 PID
    proczero.pid = 0;
    printf("asdf\n");
    // 2. 分配 trapframe（内核区分配，存内核↔用户切换现场）
    proczero.tf = (trapframe_t*)pmem_alloc(true);
    memset(proczero.tf, 0, PGSIZE);
    // 3. 创建用户页表（后续ustack/data等映射会继续加）
    proczero.pgtbl = proc_pgtbl_init(TRAPFRAME);

    // 4. 分配用户栈（用户区分配）
    uint64 ustack_va = USTACK_TOP - PGSIZE;
    uint64 ustack_pa = (uint64)pmem_alloc(false);
    memset((void*)ustack_pa, 0, PGSIZE);
    vm_mappages(proczero.pgtbl,
                ustack_va,
                ustack_pa,
                PGSIZE,
                PTE_R | PTE_W | PTE_U);
    proczero.ustack_pages = 1;
    // 5. 拷贝 initcode[] 到用户代码段（UTEXT）
    uint64 code_va = UTEXT;
    uint64 code_pa = (uint64)pmem_alloc(false);
    memset((void*)code_pa, 0, PGSIZE);
    memcpy((void*)code_pa, initcode, initcode_len);
    vm_mappages(proczero.pgtbl,
                code_va,
                code_pa,
                PGSIZE,
                PTE_R | PTE_X | PTE_U);
    assert(initcode_len <= PGSIZE, "proc_make_first: initcode too big");

    // 6. 初始化 heap_top（代码段后一页）
    proczero.heap_top = code_va + PGSIZE;

    // 7. 填 trapframe 内核态相关字段
    proczero.tf->epc = code_va;               // 返回U态的起始PC（代码入口）
    proczero.tf->sp = USTACK_TOP;             // 用户栈顶地址
    proczero.tf->kernel_satp = MAKE_SATP(kernel_pgtbl);
    proczero.tf->kernel_sp = (uint64)pmem_alloc(true) + PGSIZE; // 新的内核栈顶
    proczero.tf->kernel_trap = (uint64)trap_user_handler;
    proczero.tf->kernel_hartid = mycpuid();

    // 保存内核栈地址到proc_t（以后可能用到）
    proczero.kstack = proczero.tf->kernel_sp - PGSIZE;

    // 8. 设置进程的内核上下文（用于 swtch）：
    // ra = trap_user_return，这样 swtch 到它会立刻进入trap_user_return，并最终切到U态
    proczero.ctx.sp = proczero.tf->kernel_sp;
    proczero.ctx.ra = (uint64)trap_user_return;

    // 9. 绑定 proczero 到当前CPU
    cpu_t* c = mycpu();
    c->proc = &proczero;

    // 10. 切换上下文（CPU上下文 -> proczero上下文）
    printf("[DEBUG] switching to proczero (initcode)\n");
    swtch(&c->ctx, &proczero.ctx);
    printf("[DEBUG] returned from proczero");

}
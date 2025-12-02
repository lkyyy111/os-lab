#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "proc/initcode.h"
#include "memlayout.h"

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap_user.c
extern void trap_user_return();

extern pgtbl_t kernel_pgtbl;

// 第一个进程
static proc_t proczero;

// 获得一个初始化过的用户页表
// 完成了trapframe 和 trampoline 的映射
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    // 1. 分配页表根节点
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    if(pgtbl == NULL) return NULL;
    memset(pgtbl, 0, PGSIZE);

    // 2. 映射 Trampoline (RX)
    // 所有进程共享同一个 trampoline 物理页
    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 3. 映射 Trapframe (RW)
    // 映射到固定的虚拟地址 TRAPFRAME
    // 注意：这里没有设置 PTE_U，因为用户程序不应该直接读写它
    vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);

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
    proc_t *p = &proczero;
    p->mmap = NULL;
    
    // 1. 设置 PID
    p->pid = 1;

    // 2. 分配并映射内核栈 (Kernel Stack)
    // 分配物理页
    void *kstack_pa = pmem_alloc(true);
    if(kstack_pa == NULL) panic("proc_make_first: kstack alloc");
    
    // 在内核页表中建立映射
    // proczero 是第0个进程，使用 KSTACK(0) 计算虚拟地址
    p->kstack = KSTACK(0);
    vm_mappages(kernel_pgtbl, p->kstack, (uint64)kstack_pa, PGSIZE, PTE_R | PTE_W);

    // 3. 准备 Trapframe
    // 分配物理页
    p->tf = (trapframe_t*)pmem_alloc(true);
    if(p->tf == NULL) panic("proc_make_first: tf alloc");
    memset(p->tf, 0, PGSIZE);

    // 4. 初始化用户页表
    // 传入 tf 的物理地址用于映射
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);
    if(p->pgtbl == NULL) panic("proc_make_first: pgtbl init");

    // 5. 映射用户代码 (Initcode)
    // 位置：UTEXT (0x1000)
    void *code_pa = pmem_alloc(false);
    if(code_pa == NULL) panic("proc_make_first: code alloc");
    memset(code_pa, 0, PGSIZE);
    
    // 拷贝代码
    memmove(code_pa, initcode, initcode_len);
    
    // 映射：需要 PTE_U 权限用户才能执行
    vm_mappages(p->pgtbl, UTEXT, (uint64)code_pa, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);

    // 6. 映射用户栈 (User Stack)
    // 位置：USTACK_TOP (TRAPFRAME - PGSIZE)
    void *ustack_pa = pmem_alloc(false);
    if(ustack_pa == NULL) panic("proc_make_first: ustack alloc");
    memset(ustack_pa, 0, PGSIZE);
    
    vm_mappages(p->pgtbl, USTACK_TOP, (uint64)ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    p->ustack_pages = 1;

    // 设置堆顶 (紧接着代码段之后)
    p->heap_top = UTEXT + PGSIZE;

    // 7. 设置 Trapframe 上下文 (为了 sret 返回用户态)
    // EPC: 返回用户态后执行的第一条指令地址
    p->tf->epc = UTEXT; 
    
    // SP: 用户栈顶。
    // 注意：USTACK_TOP 是栈页的起始地址(低地址)
    // 栈是向下生长的，所以 SP 应该是页面的结束地址(高地址)
    p->tf->sp = USTACK_TOP + PGSIZE; 

    // 8. 设置内核上下文 (为了 swtch)
    // RA: swtch 返回后跳转的地址 -> trap_user_return
    p->ctx.ra = (uint64)trap_user_return;
    
    // SP: 进程在内核中运行时的栈顶
    p->ctx.sp = p->kstack + PGSIZE;

    // 9. 切换 CPU 状态并执行
    printf("proc_make_first: switch to proczero");
    
    cpu_t *c = mycpu();
    c->proc = p;
    
    // 切换上下文：从当前 CPU 启动栈 -> proczero 内核栈
    // 实际上会跳转到 trap_user_return
    swtch(&c->ctx, &p->ctx);
}
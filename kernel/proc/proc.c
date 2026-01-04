#include "lib/print.h"
#include "lib/str.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "mem/mmap.h"
#include "proc/cpu.h"
#include "proc/proc.h"
#include "proc/initcode.h"
#include "memlayout.h"
#include "riscv.h"
#include "fs/file.h"
#include "fs/dir.h"
#include "fs/inode.h"

#define NPROC 64

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap_user.c
extern void trap_user_return();

extern pgtbl_t kernel_pgtbl;

//进程数组
static proc_t procs[NPROC];

// 第一个进程
static proc_t proczero;

//全局的pid和保护它的锁
static int global_pid = 2;
static spinlock_t lk_pid;

// 申请一个pid(锁保护)
static int alloc_pid()
{
    int tmp = 0;
    spinlock_acquire(&lk_pid);
    assert(global_pid >= 0, "alloc_pid: overflow");
    tmp = global_pid++;
    spinlock_release(&lk_pid);
    return tmp;
}

// 释放锁 + 调用 trap_user_return
static void fork_return()
{
    // 由于调度器中上了锁，所以这里需要解锁
    proc_t* p = myproc();
    spinlock_release(&p->lk);
    trap_user_return();
}

// 返回一个未使用的进程空间
// 设置pid + 设置上下文中的ra和sp
// 申请tf和pgtbl使用的物理页
proc_t* proc_alloc()
{
    struct proc *p;

    // 1. 寻找空闲进程槽位
    for(p = procs; p < &procs[NPROC]; p++) {
        spinlock_acquire(&p->lk);
        if(p->state == UNUSED) {
            goto found;
        } else {
            spinlock_release(&p->lk);
        }
    }
    return NULL; // 没有空闲进程

found:
    // 2. 设置 PID
    p->pid = alloc_pid();
    p->state = UNUSED; // 暂时保持 UNUSED，直到分配完资源防止被调度器看到

    // 3. 分配 Trapframe
    if((p->tf = (trapframe_t*)pmem_alloc(true)) == NULL){
        proc_free(p);
        spinlock_release(&p->lk); // proc_free 不释放锁，需要手动释放
        return NULL;
    }
    memset(p->tf, 0, PGSIZE);

    // 4. 分配内核栈 (Kernel Stack)
    // 分配物理页
    char *pa = pmem_alloc(true);
    if(pa == NULL){
        proc_free(p);
        spinlock_release(&p->lk);
        return NULL;
    }
    // 计算该进程对应的内核栈虚拟地址
    uint64 va = KSTACK((int)(p - procs) + 1);
    // 映射到内核页表
    if(vm_mappages(kernel_pgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W) != 0){
        pmem_free((uint64)pa, true);
        proc_free(p);
        spinlock_release(&p->lk);
        return NULL;
    }
    p->kstack = va;

    // 5. 初始化用户页表
    // 这里只创建一个包含 trampoline 和 trapframe 的基础页表
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);
    if(p->pgtbl == NULL){
        proc_free(p);
        spinlock_release(&p->lk);
        return NULL;
    }

    // 6. 初始化其他字段
    p->mmap = NULL;
    p->heap_top = 0;
    p->ustack_pages = 0;
    p->parent = NULL; 
    p->exit_state = 0;
    p->sleep_space = NULL;

    memset(p->files, 0, sizeof(p->files));

    // 7. 设置内核上下文 context
    memset(&p->ctx, 0, sizeof(p->ctx));
    p->ctx.ra = (uint64)fork_return; // 调度后跳转到 fork_return
    p->ctx.sp = p->kstack + PGSIZE;  // 栈顶 (栈向下生长)

    return p; // 返回时持有 p->lk，由调用者释放或继续操作
}

// 释放一个进程空间
// 释放pgtbl的整个地址空间
// 释放mmap_region到仓库
// 设置其余各个字段为合适初始值
// tips: 调用者需持有p->lk
void proc_free(proc_t* p)
{
    // 1. 释放 Trapframe
    if(p->tf)
        pmem_free((uint64)p->tf, true);
    p->tf = NULL;

    // 2. 释放内核栈
    if(p->kstack) {
        // 解除内核页表映射并释放物理页
        // vm_unmappages(kernel_pgtbl, p->kstack, PGSIZE, true); 
        // 或者手动操作以确保安全：
        pte_t* pte = vm_getpte(kernel_pgtbl, p->kstack, false);
        if(pte && (*pte & PTE_V)){
            uint64 pa = PTE_TO_PA(*pte);
            pmem_free(pa, true);
            *pte = 0;
        }
        // 记得刷 TLB，或者依赖后续切换页表自动刷新
    }
    p->kstack = 0;

    // 3. 释放用户页表及内存
    if(p->pgtbl) {
        // uvm_destroy_pgtbl 会释放所有用户数据页、页表页，并unmap trapframe/trampoline
        // 注意：它不会释放 tf 本身(上面步骤1已做)
        uvm_destroy_pgtbl(p->pgtbl, 2);
    }
    p->pgtbl = NULL;

    // 4. 释放 mmap 链表
    mmap_region_t *m = p->mmap;
    while(m){
        mmap_region_t *next = m->next;
        mmap_region_free(m);
        m = next;
    }
    p->mmap = NULL;

    // 5. 重置其他字段
    p->pid = 0;
    p->parent = NULL;
    p->heap_top = 0;
    p->ustack_pages = 0;
    p->sleep_space = NULL;
    p->exit_state = 0;
    p->state = UNUSED;
}

// 进程模块初始化
void proc_init()
{
    // 1. 初始化 PID 锁
    spinlock_init(&lk_pid, "pid_lock");

    // 2. 遍历进程数组进行初始化
    // procs 数组大小为 NPROC
    for(struct proc *p = procs; p < &procs[NPROC]; p++) {
        // 初始化进程锁
        spinlock_init(&p->lk, "proc_lock");
        
        // 预设内核栈的虚拟地址 (固定映射策略)
        // 假设 procs 数组索引 i 对应 KSTACK(i)
        // 这里只是计算虚拟地址，物理内存和映射在 alloc 时做
        // 如果 KSTACK 宏依赖索引，可以通过 p - procs 计算
        // p->kstack = KSTACK((int)(p - procs)); 
        
        // 调用 free 确保状态干净 (此时不需要持锁，因为还没有多线程运行)
        // 但 proc_free 内部可能有些断言，简单起见手动置 UNUSED
        p->state = UNUSED;
        p->kstack = 0;
        p->pid = 0;
        p->pgtbl = NULL;
        p->tf = NULL;
        p->mmap = NULL;
        p->parent = NULL;
    }
    
    // 初始化第一个进程结构体 proczero (它不在 procs 数组里)
    spinlock_init(&proczero.lk, "proczero");
    proczero.state = UNUSED;
    
    // 初始化 mmap 仓库
    mmap_init();
    
    printf("proc_init: done\n");
}



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

    // 2. 分配并映射内核栈
    void *kstack_pa = pmem_alloc(true);
    if(kstack_pa == NULL) panic("proc_make_first: kstack alloc");
    p->kstack = KSTACK(0);
    vm_mappages(kernel_pgtbl, p->kstack, (uint64)kstack_pa, PGSIZE, PTE_R | PTE_W);

    // 3. 准备 Trapframe
    p->tf = (trapframe_t*)pmem_alloc(true);
    if(p->tf == NULL) panic("proc_make_first: tf alloc");
    memset(p->tf, 0, PGSIZE);

    // 4. 初始化用户页表
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);
    if(p->pgtbl == NULL) panic("proc_make_first: pgtbl init");

    // 5. 映射用户代码 (Initcode)
    void *code_pa = pmem_alloc(false);
    if(code_pa == NULL) panic("proc_make_first: code alloc");
    memmove(code_pa, initcode, initcode_len);
    vm_mappages(p->pgtbl, UTEXT, (uint64)code_pa, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);

    // 6. 映射用户栈
    void *ustack_pa = pmem_alloc(false);
    if(ustack_pa == NULL) panic("proc_make_first: ustack alloc");
    vm_mappages(p->pgtbl, USTACK_TOP, (uint64)ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    p->ustack_pages = 1;

    // 设置堆顶
    p->heap_top = UTEXT + PGSIZE;

    // 7. 设置 Trapframe 上下文
    p->tf->epc = UTEXT; 
    p->tf->sp = USTACK_TOP + PGSIZE; 

    // 8. 设置内核上下文 (swtch 将跳到 fork_return)
    // 注意：这里改为 fork_return，因为我们不再手动 swtch，
    // 而是让调度器像对待普通进程一样调度它，调度器会跳到 ctx.ra
    p->ctx.ra = (uint64)fork_return; 
    p->ctx.sp = p->kstack + PGSIZE;

    p->cwd = inode_alloc(INODE_ROOT); 
    
    if(p->cwd == 0)
        panic("proc_make_first: inode_alloc root failed");

    // 绑定控制台到标准输出/输入文件描述符
    // 注意：用户态宏定义为 STD_OUT=0, STD_IN=1
    // 这里按照该约定设置 files[0] 可写，files[1] 可读
    {
        file_t *f_out = file_create_dev("console", DEV_CONSOLE, 0);
        if(f_out == NULL) panic("proc_make_first: create console out");
        f_out->readable = false;
        f_out->writable = true;
        p->files[0] = f_out; // STD_OUT

        file_t *f_in = file_create_dev("console", DEV_CONSOLE, 0);
        if(f_in == NULL) panic("proc_make_first: create console in");
        f_in->readable = true;
        f_in->writable = false;
        p->files[1] = f_in; // STD_IN
    }

    // 9. [修改] 设置状态为 RUNNABLE 并直接返回
    // 此时不进行 swtch，等待 cpu_scheduler 选中它
    spinlock_acquire(&p->lk);
    p->state = RUNNABLE;
    spinlock_release(&p->lk);

    printf("proc_make_first: init created (waiting for scheduler)");
}

// 进程复制
// UNUSED -> RUNNABLE
int proc_fork()
{
    int pid;
    struct proc *np;
    struct proc *p = myproc();

    // 1. 调用 proc_alloc 分配一个新的进程槽位
    // 成功返回时持有 np->lk
    if((np = proc_alloc()) == NULL){
        return -1;
    }

    // 2. 复制父进程的用户内存空间
    uvm_copy_pgtbl(p->pgtbl, np->pgtbl, p->heap_top, p->ustack_pages, p->mmap);

    // 复制基础元数据
    np->heap_top = p->heap_top;
    np->ustack_pages = p->ustack_pages;
    mmap_region_t *node = p->mmap;
    // 使用二级指针追踪链表尾部，保持原有顺序
    mmap_region_t **pp_new = &np->mmap; 

    while(node) {
        mmap_region_t *new_node = mmap_region_alloc();
        if(new_node == NULL) {
            goto bad;
        }
        // 复制区域信息 (begin, npages等)
        *new_node = *node;
        new_node->next = NULL;

        // 挂载到子进程链表
        *pp_new = new_node;
        pp_new = &new_node->next;

        node = node->next;
    }

    *(np->tf) = *(p->tf);

    for(int i = 0; i < FILE_PER_PROC; i++){
        if(p->files[i]){
            np->files[i] = file_dup(p->files[i]); // 增加引用计数
        }
    }

    if(p->cwd == 0) panic("fork: current process has no cwd");
    np->cwd = inode_dup(p->cwd);

    np->tf->a0 = 0;
    np->parent = p;
    pid = np->pid;
    
    np->state = RUNNABLE;

    // 释放新进程的锁，允许它被调度器调度
    spinlock_release(&np->lk);

    // 父进程返回子进程的 pid
    return pid;

bad:
    // 错误处理路径
    // proc_free 会清理 pgtbl, kstack, mmap链表等
    // proc_free 假定持有锁
    proc_free(np); 
    spinlock_release(&np->lk);
    return -1;
}


// 进程放弃CPU的控制权
// RUNNING -> RUNNABLE
void proc_yield()
{
    struct proc *p = myproc();
    
    // 必须持有锁才能修改状态和调用 sched
    spinlock_acquire(&p->lk);
    
    p->state = RUNNABLE;
    
    // 切换到调度器线程
    proc_sched();
    
    // 调度器返回后（即进程再次被调度运行时），释放锁
    spinlock_release(&p->lk);
}

// 等待一个子进程进入 ZOMBIE 状态
// 将退出的子进程的exit_state放入用户给的地址 addr
// 成功返回子进程pid，失败返回-1
int proc_wait(uint64 addr)
{
    struct proc *np;
    int havekids, pid;
    struct proc *p = myproc();

    // 持有锁以保证 sleep 的原子性
    spinlock_acquire(&p->lk);

    while(1){
        // 扫描进程表是否有子进程
        havekids = 0;
        for(np = procs; np < &procs[NPROC]; np++){
            if(np->parent != p)
                continue;
            
            // 找到子进程，获取它的锁查看状态
            spinlock_acquire(&np->lk);

            if(np->state == ZOMBIE){
                // [情况1] 找到一个已经退出的子进程 -> 回收
                pid = np->pid;
                
                // 拷贝退出码给父进程用户态
                if(addr != 0 && p->pgtbl != 0){
                    // 使用 uvm.c 提供的 copyout
                    uvm_copyout(p->pgtbl, addr, (uint64)&np->exit_state, sizeof(np->exit_state));
                }
                
                // 释放子进程的所有资源
                // 注意：proc_free 会清空 np 里的内容，所以必须先读出 pid
                proc_free(np);
                
                spinlock_release(&np->lk);
                spinlock_release(&p->lk);
                return pid;
            }

            // [情况2] 子进程还活着
            havekids = 1;
            spinlock_release(&np->lk);
        }

        // [情况3] 没有子进程了，或者当前进程被杀死了
        if(!havekids){
            spinlock_release(&p->lk);
            return -1;
        }

        // [情况4] 有子进程但都在运行 -> 睡眠
        // 释放 p->lk 并让出 CPU，等待被唤醒（通常是子进程 exit 时唤醒）
        // 睡眠频道使用 p 自己的地址
        proc_sleep(p, &p->lk); 
    }
}

// 父进程退出，子进程认proczero做父，因为它永不退出
static void proc_reparent(proc_t* parent)
{
    struct proc *np;
    
    // 遍历所有进程寻找“parent”的孩子
    for(np = procs; np < &procs[NPROC]; np++){
        // 这里为了简化没有加全局锁，实际OS中可能需要保护
        if(np->parent == parent){
            // 1. 认贼作父 (划掉)，认祖归宗
            np->parent = &proczero;
            
            // 2. 如果孩子已经是僵尸，新父亲(proczero)需要知道去收尸
            spinlock_acquire(&np->lk);
            if(np->state == ZOMBIE){
                // proczero 睡眠在它自己的地址上
                proc_wakeup(&proczero);
            }
            spinlock_release(&np->lk);
        }
    }
}
// 唤醒一个进程
/*
static void proc_wakeup_one(proc_t* p)
{
    assert(spinlock_holding(&p->lk), "proc_wakeup_one: lock");
    if(p->state == SLEEPING && p->sleep_space == p) {
        p->state = RUNNABLE;
    }
}
*/
// 进程退出
void proc_exit(int exit_state)
{
    struct proc *p = myproc();

    if(p == &proczero)
        panic("init exiting");

    // 1. 关闭文件等资源 (根据你的要求，此处省略)
    for(int fd = 0; fd < FILE_PER_PROC; fd++){
        if(p->files[fd]){
            file_close(p->files[fd]);
            p->files[fd] = NULL;
        }
    }

    // 2. 将所有子进程过继给 proczero
    proc_reparent(p);

    // 3. 唤醒父进程
    // 获取锁，准备修改自身状态
    spinlock_acquire(&p->lk);

    p->exit_state = exit_state;
    p->state = ZOMBIE;

    // 父进程通常睡眠在它自己的结构体地址上
    if(p->parent)
        proc_wakeup(p->parent);

    // 4. 切换到调度器
    // 调度器会释放 p->lk，且该函数永远不会返回
    proc_sched();
    
    panic("zombie exit");
}

// 进程切换到调度器
// ps: 调用者保证持有当前进程的锁
void proc_sched()
{
    int intena;
    struct proc *p = myproc();
    struct cpu *c = mycpu();

    // 1. 安全检查
    if(!spinlock_holding(&p->lk)) 
        panic("proc_sched: p->lk");
    if(p->state == RUNNING) 
        panic("proc_sched: running");
    if(intr_get()) 
        panic("proc_sched: interruptible");

    // 2. 保存中断使能状态
    intena = c->intena;
    
    // 3. 上下文切换：进程 -> 调度器
    // c->ctx 是当前 CPU 处于 scheduler 循环时的上下文
    swtch(&p->ctx, &c->ctx);
    
    // 4. 当进程再次被调度回来时，从这里继续执行
    // 恢复中断使能状态
    c->intena = intena;
}

// 调度器
void proc_scheduler()
{
    struct proc *p;
    struct cpu *c = mycpu();
    
    c->proc = NULL; // 确保当前没有运行进程

    for(;;){
        // 开启中断。
        // 这是为了防止如果所有进程都不可运行，循环空转导致中断被永久屏蔽。
        // 开启中断允许时钟中断发生，从而可能唤醒 SLEEPING 的进程。
        intr_on();

        // 1. 遍历普通进程数组
        for(p = procs; p < &procs[NPROC]; p++) {
            spinlock_acquire(&p->lk);
            if(p->state == RUNNABLE) {
                // 切换到运行态
                p->state = RUNNING;
                c->proc = p;
                
                // 上下文切换：调度器 -> 进程
                // 此时 CPU 栈切换到进程内核栈
                swtch(&c->ctx, &p->ctx);

                // 进程让出 CPU 后（调用 yield/sleep/exit），swtch 返回到这里
                c->proc = NULL;
            }
            spinlock_release(&p->lk);
        }

        // 2. 特殊检查 proczero (因为它不在数组里)
        // 必须检查，否则 init 进程 yield 后再也无法运行
        struct proc *pz = &proczero;
        spinlock_acquire(&pz->lk);
        if(pz->state == RUNNABLE) {
            pz->state = RUNNING;
            c->proc = pz;
            swtch(&c->ctx, &pz->ctx);
            c->proc = NULL;
        }
        spinlock_release(&pz->lk);
    }
}


// 进程睡眠在sleep_space
void proc_sleep(void* sleep_space, spinlock_t* lk)
{
    struct proc *p = myproc();
    
    // 1. 获取进程锁 p->lk
    // 必须持有 p->lk 才能修改 p->state 和调用 sched
    // 如果传入的 lk 不是 p->lk，我们需要先拿 p->lk，再释放 lk
    // 这样保证了我们在整个睡眠过程中至少持有一个锁，唤醒者无法在间隙插入 wakeup
    if(lk != &p->lk){
        spinlock_acquire(&p->lk);
        spinlock_release(lk);
    }

    // 2. 修改状态
    p->sleep_space = sleep_space;
    p->state = SLEEPING;

    // 3. 调度（让出 CPU）
    proc_sched();

    // 4. 醒来后清理
    // 到达这里说明被 wakeup 设置为 RUNNABLE 并被 scheduler 选中了
    p->sleep_space = NULL;

    // 5. 恢复原来的锁
    p->state = RUNNING; // 实际上 scheduler 已经设为 RUNNING，这里显式确认
    
    if(lk != &p->lk){
        spinlock_release(&p->lk);
        spinlock_acquire(lk);
    }
}

// 唤醒所有在sleep_space沉睡的进程
void proc_wakeup(void* sleep_space)
{
    struct proc *p;

    // 1. 遍历进程数组
    for(p = procs; p < &procs[NPROC]; p++) {
        // 只能唤醒别人，不能唤醒自己（虽然自己也不可能在 sleep）
        if(p != myproc()){
            spinlock_acquire(&p->lk);
            if(p->state == SLEEPING && p->sleep_space == sleep_space) {
                p->state = RUNNABLE;
            }
            spinlock_release(&p->lk);
        }
    }
    
    // 2. 检查 proczero
    struct proc *pz = &proczero;
    if(pz != myproc()) {
        spinlock_acquire(&pz->lk);
        if(pz->state == SLEEPING && pz->sleep_space == sleep_space) {
            pz->state = RUNNABLE;
        }
        spinlock_release(&pz->lk);
    }
}
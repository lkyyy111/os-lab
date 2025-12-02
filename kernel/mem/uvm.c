#include "mem/mmap.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "lib/str.h"
#include "memlayout.h"

// 连续虚拟空间的复制(在uvm_copy_pgtbl中使用)
static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    uint64 va, pa, page;
    int flags;
    pte_t* pte;

    for(va = begin; va < end; va += PGSIZE)
    {
        pte = vm_getpte(old, va, false);
        if(pte == NULL || !(*pte & PTE_V)) continue;
        
        pa = (uint64)PTE_TO_PA(*pte);
        flags = (int)PTE_FLAGS(*pte);

        page = (uint64)pmem_alloc(false);
        if(page == 0) panic("uvm copy_range: out of memory");

        memmove((char*)page, (const char*)pa, PGSIZE);
        vm_mappages(new, va, page, PGSIZE, flags);
    }
}

// 两个 mmap_region 区域合并
// 保留一个 释放一个 不操作 next 指针
// 在uvm_munmap里使用
// static void mmap_merge(mmap_region_t* mmap_1, mmap_region_t* mmap_2, bool keep_mmap_1)
// {
//     // 确保有效和紧临
//     assert(mmap_1 != NULL && mmap_2 != NULL, "mmap_merge: NULL");
//     assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin, "mmap_merge: check fail");
    
//     // merge
//     if(keep_mmap_1) {
//         mmap_1->npages += mmap_2->npages;
//         mmap_region_free(mmap_2);
//     } else {
//         mmap_2->begin -= mmap_1->npages * PGSIZE;
//         mmap_2->npages += mmap_1->npages;
//         mmap_region_free(mmap_1);
//     }
// }

// 打印以 mmap 为首的 mmap 链
// for debug
void uvm_show_mmaplist(mmap_region_t* mmap)
{
    mmap_region_t* tmp = mmap;
    printf("\nmmap allocable area:\n");
    if(tmp == NULL)
        printf("NULL\n");
    while(tmp != NULL) {
        printf("allocable region: %p ~ %p\n", tmp->begin, tmp->begin + tmp->npages * PGSIZE);
        tmp = tmp->next;
    }
}

// 递归释放 页表占用的物理页 和 页表管理的物理页
// ps: 顶级页表level = 3, level = 0 说明是页表管理的物理页
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    // 遍历当前页表的 512 个 PTE
    for(int i = 0; i < 512; i++){
        pte_t pte = pgtbl[i];
        if(pte & PTE_V){
            uint64 pa = PTE_TO_PA(pte);
            
            if(level > 0){
                // 递归释放下一级页表
                destroy_pgtbl((pgtbl_t)pa, level - 1);
            } else {
                // level == 0, 这是叶子节点，指向用户数据物理页
                // 释放该物理页 (归还给用户池)
                pmem_free(pa, false);
            }
            pgtbl[i] = 0;
        }
    }
    // 释放当前页表页本身 (页表页是从内核池分配的)
    pmem_free((uint64)pgtbl, true);
}

// 页表销毁：trapframe 和 trampoline 单独处理
void uvm_destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    // 1. 解除 TRAMPOLINE 和 TRAPFRAME 的映射
    // 它们不属于普通的用户数据页，不能被 destroy_pgtbl 递归释放掉物理内存
    vm_unmappages(pgtbl, TRAMPOLINE, PGSIZE, false);
    vm_unmappages(pgtbl, TRAPFRAME, PGSIZE, false);

    // 2. 递归销毁剩余的用户空间
    // 从 level 2 开始 (Sv39 根页表)
    destroy_pgtbl(pgtbl, 2);
}

// 拷贝页表 (拷贝并不包括trapframe 和 trampoline)
void uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint32 ustack_pages, mmap_region_t* mmap)
{
    /* step-1: USER_BASE ~ heap_top */
    // 拷贝代码段、数据段、堆
    // 假设用户基址为 0
    copy_range(old, new, 0, heap_top);

    /* step-2: ustack */
    // 拷贝用户栈
    // 栈顶通常在 TRAPFRAME 之下
    uint64 ustack_top = TRAPFRAME;
    uint64 ustack_bottom = ustack_top - ustack_pages * PGSIZE;
    copy_range(old, new, ustack_bottom, ustack_top);

    /* step-3: mmap_region */
    // 拷贝 mmap 动态映射区
    mmap_region_t* node = mmap;
    while(node != NULL){
        copy_range(old, new, node->begin, node->begin + node->npages * PGSIZE);
        node = node->next;
    }
}

// 在用户页表和进程mmap链里 新增mmap区域 [begin, begin + npages * PGSIZE)
// 页面权限为perm
void uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    if(npages == 0) return;
    assert(begin % PGSIZE == 0, "uvm_mmap: begin not aligned");
    
    struct proc* p = myproc();

    // 修改 mmap 链 (分情况的链式操作)
    // 为了简化实现，这里直接创建一个新节点插入链表头部
    // 如果需要更高级的实现，应该按地址排序插入并尝试合并相邻节点
    mmap_region_t* node = mmap_region_alloc();
    node->begin = begin;
    node->npages = npages;
    
    // 头插法
    node->next = p->mmap;
    p->mmap = node;

    // 修改页表 (物理页申请 + 页表映射)
    uint64 addr = begin;
    for(int i = 0; i < npages; i++) {
        void* page = pmem_alloc(false);
        if(page == NULL) panic("uvm_mmap: out of memory");
        
        memset(page, 0, PGSIZE);
        // 添加 PTE_U 权限确保用户可访问
        if(vm_mappages(p->pgtbl, addr, (uint64)page, PGSIZE, perm | PTE_U) != 0)
            panic("uvm_mmap: mappages failed");
            
        addr += PGSIZE;
    }
}

// 在用户页表和进程mmap链里释放mmap区域 [begin, begin + npages * PGSIZE)
void uvm_munmap(uint64 begin, uint32 npages)
{
    if(npages == 0) return;
    assert(begin % PGSIZE == 0, "uvm_munmap: begin not aligned");

    struct proc* p = myproc();
    uint64 end = begin + npages * PGSIZE;
    mmap_region_t** ptr = &p->mmap;

    // 遍历链表处理 mmap_region 的分割/删除/收缩
    while (*ptr != NULL) {
        mmap_region_t* node = *ptr;
        uint64 node_end = node->begin + node->npages * PGSIZE;

        // 情况1: 完全不相交
        if (node_end <= begin || node->begin >= end) {
            ptr = &node->next;
            continue;
        }

        // 情况2: munmap 区域覆盖了整个 node -> 删除 node
        if (begin <= node->begin && end >= node_end) {
            *ptr = node->next; // 移除节点
            mmap_region_free(node);
            // 指针不后移，继续检查新的当前节点
            continue;
        }

        // new mmap_region 的产生
        // 情况3: 中间打洞 (Split) -> node 分裂成两个
        if (begin > node->begin && end < node_end) {
            mmap_region_t* new_node = mmap_region_alloc();
            // 新节点负责右半部分
            new_node->begin = end;
            new_node->npages = (node_end - end) / PGSIZE;
            new_node->next = node->next;
            
            // 原节点负责左半部分
            node->npages = (begin - node->begin) / PGSIZE;
            node->next = new_node;
            
            ptr = &new_node->next;
            continue;
        }

        // 情况4: 切掉头部 (Shrink Head)
        if (begin <= node->begin && end < node_end) {
            uint64 cut_len = end - node->begin;
            node->begin = end;
            node->npages -= (cut_len / PGSIZE);
            ptr = &node->next;
            continue;
        }

        // 情况5: 切掉尾部 (Shrink Tail)
        if (begin > node->begin && end >= node_end) {
            uint64 cut_len = node_end - begin;
            node->npages -= (cut_len / PGSIZE);
            ptr = &node->next;
            continue;
        }
    }

}

// 用户堆空间增加, 返回新的堆顶地址 (注意栈顶最大值限制)
// 在这里无需修正 p->heap_top
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    uint64 new_heap_top = heap_top + len;
    // 简单检查是否越界 (假设栈底在 TRAPFRAME - 2页)
    // 实际上应该检查与 ustack 的碰撞
    if(new_heap_top >= TRAPFRAME - 2*PGSIZE) {
        printf("uvm_heap_grow: heap overlap with stack");
        return 0;
    }

    uint64 a = PGROUNDUP(heap_top);
    for(; a < new_heap_top; a += PGSIZE){
        void* mem = pmem_alloc(false);
        if(mem == NULL) return 0; // 内存不足
        memset(mem, 0, PGSIZE);
        if(vm_mappages(pgtbl, a, (uint64)mem, PGSIZE, PTE_R|PTE_W|PTE_U) != 0){
            pmem_free((uint64)mem, false);
            return 0;
        }
    }

    return new_heap_top;
}

// 用户堆空间减少, 返回新的堆顶地址
// 在这里无需修正 p->heap_top
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    uint64 new_heap_top = heap_top - len;
    // 计算需要释放的页对齐范围
    uint64 old_limit = PGROUNDUP(heap_top);
    uint64 new_limit = PGROUNDUP(new_heap_top);
    
    if(new_limit < old_limit){
        vm_unmappages(pgtbl, new_limit, old_limit - new_limit, true);
    }

    return new_heap_top;
}

// 用户态地址空间[src, src+len) 拷贝至 内核态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 n, va0, pa0;

    while(len > 0){
        va0 = PGROUNDDOWN(src);
        pte_t* pte = vm_getpte(pgtbl, va0, false);
        if(pte == NULL || !(*pte & PTE_V))
            return; // 访问了未映射的内存
        
        pa0 = PTE_TO_PA(*pte);
        // 计算当前页剩余可拷贝字节数
        n = PGSIZE - (src - va0);
        if(n > len) n = len;

        memmove((void*)dst, (void*)(pa0 + (src - va0)), n);

        len -= n;
        src += n;
        dst += n;
    }
}

// 内核态地址空间[src, src+len） 拷贝至 用户态地址空间[dst, dst+len)
void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 n, va0, pa0;

    while(len > 0){
        va0 = PGROUNDDOWN(dst);
        pte_t* pte = vm_getpte(pgtbl, va0, false);
        if(pte == NULL || !(*pte & PTE_V) || !(*pte & PTE_U))
            return; // 目标地址无效或无权限
        
        pa0 = PTE_TO_PA(*pte);
        n = PGSIZE - (dst - va0);
        if(n > len) n = len;

        memmove((void*)(pa0 + (dst - va0)), (void*)src, n);

        len -= n;
        src += n;
        dst += n;
    }
}

// 用户态字符串拷贝到内核态
// 最多拷贝maxlen字节, 中途遇到'\0'则终止
// 注意: src dst 不一定是 page-aligned
void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    uint64 n, va0, pa0;
    int got_null = 0;

    while(got_null == 0 && maxlen > 0){
        va0 = PGROUNDDOWN(src);
        pte_t* pte = vm_getpte(pgtbl, va0, false);
        if(pte == NULL || !(*pte & PTE_V))
            return;
            
        pa0 = PTE_TO_PA(*pte);
        n = PGSIZE - (src - va0);
        if(n > maxlen) n = maxlen;

        char* p = (char*)(pa0 + (src - va0));
        char* d = (char*)dst;
        
        while(n > 0){
            if((*d++ = *p++) == '\0'){
                got_null = 1;
                break;
            }
            n--;
            maxlen--;
            src++; // src 也要递增，虽然循环内没用到，但逻辑上是在移动
        }
    }
}
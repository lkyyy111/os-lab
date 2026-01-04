#include "mem/mmap.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "lib/str.h"
#include "memlayout.h"
#include "riscv.h"
#include "common.h"

// -------------------------------------------------------------------
// 核心修复：鲁棒的 copy_range
// -------------------------------------------------------------------
static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    uint64 va, pa, new_pa;
    int flags;
    pte_t *pte, *new_pte;

    begin = PGROUNDDOWN(begin);

    for(va = begin; va < end; va += PGSIZE)
    {
        // 1. 获取源页表项
        pte = vm_getpte(old, va, false); 
        if(pte == NULL || !(*pte & PTE_V)) continue;
        
        pa = (uint64)PTE_TO_PA(*pte);
        flags = (int)PTE_FLAGS(*pte);

        // 2. 尝试获取目标页表项
        new_pte = vm_getpte(new, va, false);
        
        // Case A: 目标已经显式映射了
        if (new_pte != NULL && (*new_pte & PTE_V)) {
            new_pa = PTE_TO_PA(*new_pte);
        } 
        else {
            // Case B: 目标看起来未映射，尝试分配并映射
            void* mem = pmem_alloc(false);
            if(mem == 0) panic("uvm copy_range: out of memory");
            new_pa = (uint64)mem;
            
            // 尝试映射
            int ret = vm_mappages(new, va, new_pa, PGSIZE, flags);
            
            if (ret != 0) {
                // 【关键修复】如果映射失败 (remap fail)，说明底层发现其实已经有映射了
                // (可能是 mmap 列表里重复包含栈区域)
                // 此时我们释放刚才申请的内存，改为使用已存在的页面
                pmem_free(new_pa, false);

                new_pte = vm_getpte(new, va, false);
                if (new_pte != NULL && (*new_pte & PTE_V)) {
                    // 确认已存在，使用它
                    new_pa = PTE_TO_PA(*new_pte);
                } else {
                    // 如果既映射失败，又查不到 PTE，那就是真的严重错误
                    printf("uvm copy_range: panic at va=%p", va);
                    panic("uvm copy_range: vm_mappages failed strangely");
                }
            }
        }

        // 3. 执行内存拷贝 (覆盖旧数据)
        memmove((char*)new_pa, (const char*)pa, PGSIZE);
    }
}

// -------------------------------------------------------------------
// 辅助函数
// -------------------------------------------------------------------

void uvm_show_mmaplist(mmap_region_t* mmap)
{
    mmap_region_t* tmp = mmap;
    printf("mmap allocable area:");
    if(tmp == NULL) printf("NULL");
    while(tmp != NULL) {
        printf("allocable region: %p ~ %p", tmp->begin, tmp->begin + tmp->npages * PGSIZE);
        tmp = tmp->next;
    }
}

static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    for(int i = 0; i < 512; i++){
        pte_t pte = pgtbl[i];
        if(pte & PTE_V){
            uint64 pa = PTE_TO_PA(pte);
            if(level > 0){
                destroy_pgtbl((pgtbl_t)pa, level - 1);
            } else {
                pmem_free(pa, false);
            }
            pgtbl[i] = 0;
        }
    }
    pmem_free((uint64)pgtbl, true);
}

void uvm_destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    vm_unmappages(pgtbl, TRAMPOLINE, PGSIZE, false);
    vm_unmappages(pgtbl, TRAPFRAME, PGSIZE, false);
    destroy_pgtbl(pgtbl, 2);
}

void uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint32 ustack_pages, mmap_region_t* mmap)
{
    // 1. Copy Code/Data/Heap
    copy_range(old, new, 0, heap_top);

    // 2. Copy User Stack
    uint64 ustack_top = TRAPFRAME;
    uint64 ustack_bottom = ustack_top - ustack_pages * PGSIZE;
    copy_range(old, new, ustack_bottom, ustack_top);

    // 3. Copy mmap regions
    // 如果 mmap 链表里包含了 Stack 区域，copy_range 会再次被调用
    // 上面的 copy_range 修复逻辑会自动处理这种情况（复用物理页，不报错）
    mmap_region_t* node = mmap;
    while(node != NULL){
        copy_range(old, new, node->begin, node->begin + node->npages * PGSIZE);
        node = node->next;
    }
}

// -------------------------------------------------------------------
// 核心修复：更安全的 mmap
// -------------------------------------------------------------------
void uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    if(npages == 0) return;
    if(begin % PGSIZE != 0) panic("uvm_mmap: begin not aligned");
    
    struct proc* p = myproc();

    // 1. 链表操作
    mmap_region_t* node = mmap_region_alloc();
    if(node == NULL) panic("uvm_mmap: no free mmap node");
    node->begin = begin;
    node->npages = npages;
    node->next = p->mmap;
    p->mmap = node;

    // 2. 页表操作
    uint64 addr = begin;
    for(int i = 0; i < npages; i++) {
        // 【关键策略】不再依赖 vm_getpte 检查，直接强制 Unmap
        // 这避免了 "vm_getpte 说没有，vm_mappages 说有" 的情况
        vm_unmappages(p->pgtbl, addr, PGSIZE, true);

        void* page = pmem_alloc(false);
        if(page == NULL) panic("uvm_mmap: out of memory");
        memset(page, 0, PGSIZE); // 确保 clean

        if(vm_mappages(p->pgtbl, addr, (uint64)page, PGSIZE, perm | PTE_U) != 0) {
            pmem_free((uint64)page, false);
            panic("uvm_mmap: mappages failed even after unmap");
        }
            
        addr += PGSIZE;
    }
}

void uvm_munmap(uint64 begin, uint32 npages)
{
    if(npages == 0) return;
    assert(begin % PGSIZE == 0, "uvm_munmap: begin not aligned");

    struct proc* p = myproc();
    uint64 end = begin + npages * PGSIZE;
    mmap_region_t** ptr = &p->mmap;

    while (*ptr != NULL) {
        mmap_region_t* node = *ptr;
        uint64 node_end = node->begin + node->npages * PGSIZE;

        // 1. No overlap
        if (node_end <= begin || node->begin >= end) {
            ptr = &node->next;
            continue;
        }

        // 2. Full remove
        if (begin <= node->begin && end >= node_end) {
            *ptr = node->next; 
            vm_unmappages(p->pgtbl, node->begin, node->npages * PGSIZE, true);
            mmap_region_free(node);
            continue;
        }

        // 3. Split
        if (begin > node->begin && end < node_end) {
            vm_unmappages(p->pgtbl, begin, npages * PGSIZE, true); // unmap hole

            mmap_region_t* new_node = mmap_region_alloc();
            new_node->begin = end;
            new_node->npages = (node_end - end) / PGSIZE;
            new_node->next = node->next;
            
            node->npages = (begin - node->begin) / PGSIZE;
            node->next = new_node;
            
            ptr = &new_node->next;
            continue;
        }

        // 4. Shrink Head
        if (begin <= node->begin && end < node_end) {
            uint64 cut_len = end - node->begin;
            vm_unmappages(p->pgtbl, node->begin, cut_len, true);
            node->begin = end;
            node->npages -= (cut_len / PGSIZE);
            ptr = &node->next;
            continue;
        }

        // 5. Shrink Tail
        if (begin > node->begin && end >= node_end) {
            uint64 cut_len = node_end - begin;
            vm_unmappages(p->pgtbl, begin, cut_len, true);
            node->npages -= (cut_len / PGSIZE);
            ptr = &node->next;
            continue;
        }
    }
}

uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    uint64 new_heap_top = heap_top + len;
    if(new_heap_top >= TRAPFRAME - 2*PGSIZE) {
        printf("uvm_heap_grow: heap overlap with stack");
        return 0;
    }

    uint64 a = PGROUNDUP(heap_top);
    for(; a < new_heap_top; a += PGSIZE){
        // 同样，直接强制 unmap 避免潜在冲突
        vm_unmappages(pgtbl, a, PGSIZE, true);

        void* mem = pmem_alloc(false);
        if(mem == NULL) return 0; 
        memset(mem, 0, PGSIZE);
        if(vm_mappages(pgtbl, a, (uint64)mem, PGSIZE, PTE_R|PTE_W|PTE_U) != 0){
            pmem_free((uint64)mem, false);
            return 0;
        }
    }
    return new_heap_top;
}

uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 heap_top, uint32 len)
{
    uint64 new_heap_top = heap_top - len;
    uint64 old_limit = PGROUNDUP(heap_top);
    uint64 new_limit = PGROUNDUP(new_heap_top);
    
    if(new_limit < old_limit){
        vm_unmappages(pgtbl, new_limit, old_limit - new_limit, true);
    }
    return new_heap_top;
}

int uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 n, va0, pa0;
    while(len > 0){
        va0 = PGROUNDDOWN(src);
        pte_t* pte = vm_getpte(pgtbl, va0, false);
        if(pte == NULL || !(*pte & PTE_V)) return -1; 
        
        pa0 = PTE_TO_PA(*pte);
        n = PGSIZE - (src - va0);
        if(n > len) n = len;
        memmove((void*)dst, (void*)(pa0 + (src - va0)), n);
        len -= n; src += n; dst += n;
    }
    return 0;
}

int uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 n, va0, pa0;
    while(len > 0){
        va0 = PGROUNDDOWN(dst);
        pte_t* pte = vm_getpte(pgtbl, va0, false);
        if(pte == NULL || !(*pte & PTE_V) || !(*pte & PTE_U)) return -1; 
        
        pa0 = PTE_TO_PA(*pte);
        n = PGSIZE - (dst - va0);
        if(n > len) n = len;
        memmove((void*)(pa0 + (dst - va0)), (void*)src, n);
        len -= n; src += n; dst += n;
    }
    return 0;
}

void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    uint64 n, va0, pa0;
    int got_null = 0;
    while(got_null == 0 && maxlen > 0){
        va0 = PGROUNDDOWN(src);
        pte_t* pte = vm_getpte(pgtbl, va0, false);
        if(pte == NULL || !(*pte & PTE_V)) return;
            
        pa0 = PTE_TO_PA(*pte);
        n = PGSIZE - (src - va0);
        if(n > maxlen) n = maxlen;
        char* p = (char*)(pa0 + (src - va0));
        char* d = (char*)dst;
        while(n > 0){
            if((*d++ = *p++) == '\0'){ got_null = 1; break; }
            n--; maxlen--; src++; 
        }
    }
}

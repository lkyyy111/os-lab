#include "mem/vmem.h"
#include "riscv.h"
#include "mem/pmem.h"    // 物理内存分配器
#include "lib/print.h"   // 调试输出
#include "lib/str.h"
#include "memlayout.h"

extern char trampoline[];

// 内核页表全局变量
pgtbl_t kernel_pgtbl = NULL;

// 获取 PTE 指针：如果 alloc=true 且中间页表不存在，则分配
pte_t* vm_getpte(pgtbl_t pagetable, uint64 va, bool alloc) {
    if (va >= VA_MAX) {
        printf("vm_getpte: invalid va 0x%lx\n", va);
        return NULL;
    }

    pgtbl_t pt = pagetable;
    for (int level = 2; level > 0; level--) {
        int idx = VA_TO_VP_N(va, level);
        pte_t pte = pt[idx];
        if (pte & PTE_V) {
            if (PTE_CHECK(pte)) { 
                // 中间页表
                pt = (pgtbl_t)PTE_TO_PA(pte);
            } else {
                // 已是叶子映射
                return NULL;
            }
        } else {
            if (!alloc) 
                return NULL;
            // 分配一个新的页表页
            void* new_pt = pmem_alloc(true);
            if (new_pt == NULL)
                return NULL;
            // 清空新页表
            memset(new_pt, 0, PGSIZE);
            // 当前 PTE 填入新页表物理地址
            pt[idx] = PA_TO_PTE(new_pt) | PTE_V;
            pt = (pgtbl_t)new_pt;
        }
    }
    // 最低层索引
    return &pt[VA_TO_VP_N(va, 0)];
}

// 映射一段内存
void vm_mappages(pgtbl_t pagetable, uint64 va, uint64 pa, uint64 len, int perm) {
    if (len == 0) 
        return;

    uint64 a = va;
    uint64 end = va + len;
    while (a < end) {
        pte_t* pte = vm_getpte(pagetable, a, true);
        if (pte == NULL) {
            printf("vm_mappages: unable to allocate PTE for va=0x%lx\n", a);
            return;
        }
        if (*pte & PTE_V) {
            printf("vm_mappages: remap at va=0x%lx\n", a);
        }
        *pte = PA_TO_PTE(pa) | perm | PTE_V;
        a += PGSIZE;
        pa += PGSIZE;
    }
}

// 解除映射
void vm_unmappages(pgtbl_t pagetable, uint64 va, uint64 len, bool freeit) {
    if (len == 0) return;

    uint64 a = va;
    uint64 end = va + len;
    while (a < end) {
        pte_t* pte = vm_getpte(pagetable, a, false);
        if (pte != NULL && (*pte & PTE_V)) {
            if (freeit) {
                uint64 pa = PTE_TO_PA(*pte);
                pmem_free(pa, true); // 释放物理页
            }
            *pte = 0;
        }
        a += PGSIZE;
    }
}

// 递归打印页表结构
static void vm_print_level(pgtbl_t pagetable, int level, uint64 prefix_va) {
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if (pte & PTE_V) {
            uint64 va = prefix_va | ((uint64)i << VA_SHIFT(level));
            if (PTE_CHECK(pte)) {
                // 中间页表
                printf("L%d[%d] VA=0x%lx -> next level\n", level, i, va);
                vm_print_level((pgtbl_t)PTE_TO_PA(pte), level-1, va);
            } else {
                uint64 pa = PTE_TO_PA(pte);
                printf("L%d[%d] VA=0x%lx -> PA=0x%lx flags=0x%lx\n",level, i, va, pa, PTE_FLAGS(pte));
            }
        }
    }
}

void vm_print(pgtbl_t pgtbl) {
    vm_print_level(pgtbl, 2, 0);
}

// 初始化内核页表
void kvm_init() {
    kernel_pgtbl = (pgtbl_t) pmem_alloc(true); // 分配顶级页表
    memset(kernel_pgtbl, 0, PGSIZE);

    // 1. 硬件寄存器区恒等映射 (例如 UART)
    uint64 uart_va = 0x10000000;
    vm_mappages(kernel_pgtbl, uart_va, uart_va, PGSIZE, PTE_R | PTE_W);

    uint64 plic_base = 0x0c000000;
    uint64 plic_size = 0x400000; // 4MB
    vm_mappages(kernel_pgtbl, plic_base, plic_base, plic_size, PTE_R | PTE_W);

    uint64 clint_base = 0x02000000;
    uint64 clint_size = 0x10000; // 64KB
    vm_mappages(kernel_pgtbl, clint_base, clint_base, clint_size, PTE_R | PTE_W);

    // 2. 内存区恒等映射 (例如 QEMU 默认内存 128MB 从 0x80000000)
    uint64 dram_base = 0x80000000;
    uint64 dram_size = 128 * 1024 * 1024;
    vm_mappages(kernel_pgtbl, dram_base, dram_base, dram_size, PTE_R | PTE_W | PTE_X);

    vm_mappages(kernel_pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// 启用分页模式
void kvm_inithart() {
    uint64 satp_val = MAKE_SATP(kernel_pgtbl);
    w_satp(satp_val);
    sfence_vma();  // 刷 TLB
}

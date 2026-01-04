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
    if (va >= VA_MAX) return NULL;
    if (pagetable == NULL) {
        pagetable = kernel_pgtbl;
    }

    pgtbl_t pt = pagetable;
    for (int level = 2; level > 0; level--) {
        int idx = VA_TO_VP_N(va, level);
        pte_t *pte = &pt[idx];
        
        if (*pte & PTE_V) {
            // 检查是否是大页映射 (叶子节点出现在中间层)
            if ((*pte & (PTE_R | PTE_W | PTE_X)) == 0) {
                // 是指向下一级页表的目录项
                pt = (pgtbl_t)PTE_TO_PA(*pte);
            } else {
                // 是大页映射，无法继续下钻
                return NULL;
            }
        } else {
            if (!alloc) return NULL;
            // 分配一个新的页表页
            void* new_pt = pmem_alloc(true);
            if (new_pt == NULL) return NULL;
            memset(new_pt, 0, PGSIZE);
            // 建立目录连接
            *pte = PA_TO_PTE(new_pt) | PTE_V;
            pt = (pgtbl_t)new_pt;
        }
    }
    return &pt[VA_TO_VP_N(va, 0)];
}

int vm_mappages(pgtbl_t pagetable, uint64 va, uint64 pa, uint64 len, int perm) {
    uint64 a, end;
    pte_t *pte;

    if(len == 0) return 0;

    a = PGROUNDDOWN(va);
    end = PGROUNDDOWN(va + len - 1);

    for (;;) {
        pte = vm_getpte(pagetable, a, true);
        if (pte == NULL) return -1;
        
        // 【核心修复】检查 Remap
        if (*pte & PTE_V) {
            // 检查是否是相同的映射（物理地址和权限都一样），如果是，则允许（幂等性）
            if (PTE_TO_PA(*pte) == pa && (PTE_FLAGS(*pte) & 0x3FF) == perm) {
                // pass
            } else {
                // 致命错误：试图覆盖已存在的有效映射
                // 如果覆盖，会导致原本的数据（如栈数据）丢失，变成全0，引发 syscall 0 错误
                printf("vm_mappages: remap fail at va=0x%lx (old pa=0x%lx, new pa=0x%lx)", a, PTE_TO_PA(*pte), pa);
                return -1; // 返回错误，而不是覆盖！
            }
        } else {
            *pte = PA_TO_PTE(pa) | perm | PTE_V;
        }
        
        
        
        if (a == end) break;
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

// 解除映射
void vm_unmappages(pgtbl_t pagetable, uint64 va, uint64 len, bool freeit) {
    if (len == 0) return;

    uint64 a = PGROUNDDOWN(va);
    uint64 end = PGROUNDDOWN(va + len - 1);

    for (;;) {
        pte_t* pte = vm_getpte(pagetable, a, false);
        if (pte != NULL && (*pte & PTE_V)) {
            uint64 pa = PTE_TO_PA(*pte);
            if (freeit) {
                pmem_free(pa, true); // 根据你的实现，这里可能需要区分内核/用户池，或者统一处理
            }
            *pte = 0;
        }
        if (a == end) break;
        a += PGSIZE;
    }
}

// 递归打印页表结构
static void vm_print_level(pgtbl_t pagetable, int level, uint64 prefix_va) {
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if (pte & PTE_V) {
            uint64 va = prefix_va | ((uint64)i << VA_SHIFT(level));
            if ((pte & (PTE_R|PTE_W|PTE_X)) == 0) {
                vm_print_level((pgtbl_t)PTE_TO_PA(pte), level-1, va);
            } else {
                uint64 pa = PTE_TO_PA(pte);
                printf(" .. L%d[%d] VA=0x%lx -> PA=0x%lx", level, i, va, pa);
            }
        }
    }
}

void vm_print(pgtbl_t pgtbl) {
    printf("page table %p", pgtbl);
    vm_print_level(pgtbl, 2, 0);
}

// 初始化内核页表
void kvm_init() {
    kernel_pgtbl = (pgtbl_t) pmem_alloc(true); // 分配顶级页表
    memset(kernel_pgtbl, 0, PGSIZE);

    // 1. 硬件寄存器区恒等映射 (例如 UART)
    uint64 uart_va = 0x10000000;
    vm_mappages(kernel_pgtbl, uart_va, uart_va, PGSIZE, PTE_R | PTE_W);

    uint64 virtio_base = 0x10001000;
    vm_mappages(kernel_pgtbl, virtio_base, virtio_base, PGSIZE, PTE_R | PTE_W);

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

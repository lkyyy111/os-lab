#include "common.h"
#include "lib/lock.h"
#include "mem/pmem.h"
#include "lib/print.h"


typedef struct page_node { 
    struct page_node* next;
} page_node_t; 

typedef struct alloc_region { 
    uint64 begin;          // 起始物理地址
    uint64 end;            // 终止物理地址
    spinlock_t lk;         // 自旋锁保护下面的变量
    uint32 allocable;      // 可分配页面数
    page_node_t list_head; // 链表头
} alloc_region_t; 

// 内核和用户独立的内存区域
static alloc_region_t kern_region, user_region;

// 初始化链表区域
static void
region_init(alloc_region_t* r, uint64 begin, uint64 end)
{
    r->begin = begin;
    r->end = end;
    spinlock_init(&r->lk, "alloc_region");
    r->allocable = (end - begin) / PGSIZE;

    r->list_head.next = NULL;

    // 构建链表: 在每个页面起始写入 next 指针
    uint64 addr = begin;
    page_node_t* prev = &r->list_head;
    for (uint32 i = 0; i < r->allocable; i++) {
        page_node_t* node = (page_node_t*)addr;
        node->next = NULL;
        prev->next = node;
        prev = node;
        addr += PGSIZE;
    }
}

// 初始化物理内存分为内核区域和用户区域
void pmem_init()
{
    uint64 kern_begin = (uint64)ALLOC_BEGIN;
    uint64 kern_end   = kern_begin + (uint64)KERNEL_PAGES * PGSIZE;
    uint64 user_begin = kern_end;
    uint64 user_end   = (uint64)ALLOC_END;


    region_init(&kern_region, kern_begin, kern_end);
    region_init(&user_region, user_begin, user_end);
}

// 申请物理页
void* pmem_alloc(bool in_kernel)
{
    alloc_region_t* r = in_kernel ? &kern_region : &user_region;
    spinlock_acquire(&r->lk);

    page_node_t* node = r->list_head.next;
    if (node) {
        r->list_head.next = node->next; // 从链表移除头节点
        r->allocable--;
    }

    spinlock_release(&r->lk);

    return node ? (void*)node : NULL;
}

// 释放物理页
void pmem_free(uint64 addr, bool in_kernel)
{
    alloc_region_t* r = in_kernel ? &kern_region : &user_region;

    // 确保释放地址合法
    if (addr < r->begin || addr >= r->end) {
        printf("pmem_free: invalid addr %p\n", (void*)addr);
        return;
    }

    spinlock_acquire(&r->lk);

    page_node_t* node = (page_node_t*)addr;
    // 头插法加入到链表
    node->next = r->list_head.next;
    r->list_head.next = node;
    r->allocable++;

    spinlock_release(&r->lk);
}

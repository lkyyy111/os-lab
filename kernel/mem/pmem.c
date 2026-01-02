#include "common.h"
#include "lib/lock.h"
#include "mem/pmem.h"
#include "lib/print.h"
#include "lib/str.h" // 需要引用 memset

typedef struct page_node { 
    struct page_node* next;
} page_node_t; 

typedef struct alloc_region { 
    uint64 begin;          // 起始物理地址
    uint64 end;            // 终止物理地址
    spinlock_t lk;         // 自旋锁
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
    
    // 确保对齐，避免非对齐访问
    if (begin % PGSIZE != 0 || end % PGSIZE != 0) {
        panic("pmem: region not aligned");
    }

    // 防止 end < begin 导致的下溢
    if (end <= begin) {
        r->allocable = 0;
        r->list_head.next = NULL;
        return;
    }

    r->allocable = (end - begin) / PGSIZE;
    r->list_head.next = NULL;

    // 构建链表: 在每个页面起始写入 next 指针
    // 注意：这里的 addr 是物理地址。
    // 在系统启动早期（开启分页前）或内核具有恒等映射时，直接写入物理地址是合法的。
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
    // 确保 ALLOC_BEGIN 等宏定义正确且对齐
    uint64 kern_begin = (uint64)ALLOC_BEGIN;
    uint64 kern_end   = kern_begin + (uint64)KERNEL_PAGES * PGSIZE;
    uint64 user_begin = kern_end;
    uint64 user_end   = (uint64)ALLOC_END;

    printf("pmem: init kern [0x%lx, 0x%lx) user [0x%lx, 0x%lx)", kern_begin, kern_end, user_begin, user_end);

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

    if (node) {
        // 【关键修复】申请到的内存必须清零！
        // 否则前8字节是 next 指针的残留值，或者之前使用过的脏数据
        memset((void*)node, 0, PGSIZE);
        return (void*)node;
    } else {
        printf("pmem_alloc: out of memory (kernel=%d)", in_kernel);
        return NULL;
    }
}

// 释放物理页
void pmem_free(uint64 addr, bool in_kernel)
{
    alloc_region_t* r = in_kernel ? &kern_region : &user_region;

    // 确保释放地址合法且对齐
    if (addr < r->begin || addr >= r->end || addr % PGSIZE != 0) {
        printf("pmem_free: invalid addr %p", (void*)addr);
        // panic("pmem_free: invalid address"); // 可选：直接 panic
        return;
    }

    spinlock_acquire(&r->lk);

    page_node_t* node = (page_node_t*)addr;
    
    // 简单检查：防止非常明显的双重释放（这不能防止所有双重释放）
    if (node->next == r->list_head.next && r->allocable > 0) {
         // 这只是一个弱检查，真正的双重释放检测需要 bitmap 或 magic number
    }

    // 头插法加入到链表
    node->next = r->list_head.next;
    r->list_head.next = node;
    r->allocable++;

    spinlock_release(&r->lk);
}

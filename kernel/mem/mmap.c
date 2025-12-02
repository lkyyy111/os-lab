#include "lib/print.h"
#include "lib/str.h"
#include "lib/lock.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "mem/mmap.h"

// 包装 mmap_region_t 用于仓库组织
typedef struct mmap_region_node {
    mmap_region_t mmap;
    struct mmap_region_node* next;
} mmap_region_node_t;

#define N_MMAP 256

// mmap_region_node_t 仓库(单向链表) + 指向链表头节点的指针 + 保护仓库的锁
static mmap_region_node_t list_mmap_region_node[N_MMAP];
static mmap_region_node_t* list_head;
static spinlock_t list_lk;

// 初始化上述三个数据结构
void mmap_init()
{
    spinlock_init(&list_lk, "mmap_list");

    // 将数组中的节点串联成链表
    for (int i = 0; i < N_MMAP - 1; i++) {
        list_mmap_region_node[i].next = &list_mmap_region_node[i + 1];
    }
    // 最后一个节点的 next 指向 NULL
    list_mmap_region_node[N_MMAP - 1].next = NULL;

    // list_head 指向数组的第一个元素，即链表头
    list_head = &list_mmap_region_node[0];
}

// 从仓库申请一个 mmap_region_t
// 若申请失败则 panic
// 注意: list_head 保留, 不会被申请出去
mmap_region_t* mmap_region_alloc()
{
    spinlock_acquire(&list_lk);

    if (list_head == NULL) {
        spinlock_release(&list_lk);
        panic("mmap_region_alloc: out of memory");
    }

    // 取出头节点
    mmap_region_node_t* node = list_head;
    // 头指针后移
    list_head = node->next;

    spinlock_release(&list_lk);

    // 为了安全，清空该节点的 mmap 数据部分
    memset(&node->mmap, 0, sizeof(mmap_region_t));

    // 返回内部的 mmap 结构体指针
    // 因为 mmap 是 struct 的第一个成员，所以地址相同，但为了类型安全，取地址
    return &node->mmap;
}

// 向仓库归还一个 mmap_region_t
void mmap_region_free(mmap_region_t* mmap)
{
    if (mmap == NULL) return;

    // 通过 mmap 指针还原出 node 指针
    // 这是一个技巧：因为 mmap 是 node 的第一个成员，所以它们的地址是一样的
    // 但如果结构体定义变了，这里需要用 container_of 宏，这里简单强转即可
    mmap_region_node_t* node = (mmap_region_node_t*)mmap;

    spinlock_acquire(&list_lk);

    // 头插法归还到链表
    node->next = list_head;
    list_head = node;

    spinlock_release(&list_lk);
}

// 输出仓库里可用的 mmap_region_node_t
// for debug
void mmap_show_mmaplist()
{
    spinlock_acquire(&list_lk);
    
    mmap_region_node_t* tmp = list_head;
    int node = 1;
    long index = 0; // 使用 long 匹配指针差值类型
    while (tmp)
    {
        // 计算当前节点在数组中的索引
        // 注意：这里应该减去数组基地址，而不是减去当前的 list_head
        index = tmp - list_mmap_region_node; 
        printf("Node %d: Array Index = %ld, Addr = %p", node++, index, tmp);
        tmp = tmp->next;
    }

    spinlock_release(&list_lk);
}
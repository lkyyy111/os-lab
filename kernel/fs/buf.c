#include "fs/buf.h"
#include "dev/vio.h"
#include "lib/lock.h"
#include "lib/print.h"
#include "lib/str.h"

#define N_BLOCK_BUF 64
#define BLOCK_NUM_UNUSED 0xFFFFFFFF

// 将buf包装成双向循环链表的node
typedef struct buf_node {
    buf_t buf;
    struct buf_node* next;
    struct buf_node* prev;
} buf_node_t;

// buf cache
static buf_node_t buf_cache[N_BLOCK_BUF];
static buf_node_t head_buf; // ->next 已分配 ->prev 可分配
static spinlock_t lk_buf_cache; // 这个锁负责保护 链式结构 + buf_ref + block_num

// 链表操作
static void insert_head(buf_node_t* buf_node, bool head_next)
{
    // 离开
    if(buf_node->next && buf_node->prev) {
        buf_node->next->prev = buf_node->prev;
        buf_node->prev->next = buf_node->next;
    }

    // 插入
    if(head_next) { // 插入 head->next
        buf_node->prev = &head_buf;
        buf_node->next = head_buf.next;
        head_buf.next->prev = buf_node;
        head_buf.next = buf_node;        
    } else { // 插入 head->prev
        buf_node->next = &head_buf;
        buf_node->prev = head_buf.prev;
        head_buf.prev->next = buf_node;
        head_buf.prev = buf_node;
    }
}

// 初始化
void buf_init()
{
    spinlock_init(&lk_buf_cache, "buf_cache");

    // 初始化链表头
    head_buf.next = &head_buf;
    head_buf.prev = &head_buf;
    head_buf.buf.block_num = BLOCK_NUM_UNUSED;
    head_buf.buf.buf_ref = 0;
    head_buf.buf.disk = false;
    sleeplock_init(&head_buf.buf.slk, "buf_head");
    printf("debug: head_buf.slk=%p\n", &head_buf.buf.slk);

    for(int i = 0; i < N_BLOCK_BUF; i++) {
        buf_node_t *node = &buf_cache[i];

        // 初始化节点字段
        node->next = node->prev = NULL;
        node->buf.block_num = BLOCK_NUM_UNUSED;
        node->buf.buf_ref = 0;
        node->buf.disk = false;
        // 初始化睡眠锁（关键）
        sleeplock_init(&node->buf.slk, "block_buf");
        printf("debug: buf[%d]=%p slk=%p\n", i, node, &node->buf.slk);

        // 插入到链表尾端（LRU 空闲端）
        insert_head(node, false);
    }
}


/*
    首先假设这个block_num对应的block在内存中有备份, 找到它并上锁返回
    如果找不到, 尝试申请一个无人使用的buf, 去磁盘读取对应block并上锁返回
    如果没有空闲buf, panic报错
    (建议合并xv6的bget())
*/
buf_t* buf_read(uint32 block_num)
{
    buf_node_t *b;

    spinlock_acquire(&lk_buf_cache);

    // 1. 缓存命中查找 (Cache Hit)
    // 从头向后遍历 (通常经常使用的在前面)
    for(b = head_buf.next; b != &head_buf; b = b->next) {
        if(b->buf.block_num == block_num) {
            b->buf.buf_ref++;
            spinlock_release(&lk_buf_cache);
            
            // 获取睡眠锁，独占访问该 buffer
            sleeplock_acquire(&b->buf.slk);
            return &b->buf;
        }
    }

    // 2. 缓存未命中 (Cache Miss)，需要分配新块
    // 从后向前遍历 (head.prev 是 LRU 端)，寻找 ref == 0 的块进行驱逐
    for(b = head_buf.prev; b != &head_buf; b = b->prev) {
        if(b->buf.buf_ref == 0) {
            // 找到牺牲块
            b->buf.block_num = block_num; // 更新为新的块号
            b->buf.buf_ref = 1;           // 标记为正在使用
            
            // 注意：这里我们还没有真正读数据，但已经占住了坑位
            spinlock_release(&lk_buf_cache);

            // 获取睡眠锁
            sleeplock_acquire(&b->buf.slk);

            // 调用磁盘驱动读取数据
            // 参数 false 表示读取 (write = false)
            virtio_disk_rw(&b->buf, false);

            return &b->buf;
        }
    }

    // 3. 缓存耗尽
    spinlock_release(&lk_buf_cache);
    panic("buf_read: no free buffers");
    return NULL; 
}

// 写函数 (强制磁盘和内存保持一致)
void buf_write(buf_t* buf)
{
    if (buf == NULL) return;
    virtio_disk_rw(buf, true);
}

// buf 释放
void buf_release(buf_t* buf)
{
    if(buf == NULL) return;

    // 释放 buffer 的使用锁
    sleeplock_release(&buf->slk);

    spinlock_acquire(&lk_buf_cache);

    buf->buf_ref--;

    if (buf->buf_ref == 0) {
        // 如果引用归零，说明该 buffer 暂时空闲了
        // 将其移动到链表头部 (head.next)
        // 这样 LRU 算法会优先从链表尾部 (head.prev) 驱逐那些很久没用的块
        // 而刚刚释放的这个块 (可能包含热数据) 会在链表中保留更长时间
        buf_node_t* node = (buf_node_t*)buf; // buf_t 是 buf_node_t 的第一个成员，强转安全
        insert_head(node, true);
    }

    spinlock_release(&lk_buf_cache);
}

// 输出buf_cache的情况
void buf_print()
{
    printf("\nbuf_cache:\n");
    buf_node_t* buf = head_buf.next;
    spinlock_acquire(&lk_buf_cache);
    while(buf != &head_buf)
    {
        buf_t* b = &buf->buf;
        printf("buf %d: ref = %d, block_num = %d\n", (int)(buf-buf_cache), b->buf_ref, b->block_num);
        for(int i = 0; i < 8; i++)
            printf("%d ",b->data[i]);
        printf("\n");
        buf = buf->next;
    }
    spinlock_release(&lk_buf_cache);
}
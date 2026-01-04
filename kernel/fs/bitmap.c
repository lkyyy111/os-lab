#include "fs/buf.h"
#include "fs/fs.h"
#include "fs/bitmap.h"
#include "lib/print.h"

extern super_block_t sb;

// search and set bit
static uint32 bitmap_search_and_set(uint32 bitmap_block)
{
    buf_t* buf = buf_read(bitmap_block);
    uint32 bit_idx = 0;
    uint32 byte_idx = 0;
    
    // 遍历整个 Block 中的每一个位
    // BLOCK_SIZE * 8 是一个 block 能管理的资源总数 (通常是 1024*8 = 8192)
    for(uint32 i = 0; i < BLOCK_SIZE * 8; i++) {
        byte_idx = i / 8;
        bit_idx  = i % 8;
        
        // 检查该位是否为 0 (空闲)
        // 使用 1 << bit_idx 生成掩码
        if ((buf->data[byte_idx] & (1 << bit_idx)) == 0) {
            // 找到空闲位，将其置 1 (占用)
            buf->data[byte_idx] |= (1 << bit_idx);
            
            // 立即写回磁盘，确保分配持久化
            buf_write(buf);
            
            // 释放 buffer
            buf_release(buf);
            
            return i; // 返回找到的序号
        }
    }

    // 如果遍历完都没找到空闲位
    buf_release(buf);
    panic("bitmap_search_and_set: out of resources");
    return 0; // 不会运行到这里
}

// unset bit
static void bitmap_unset(uint32 bitmap_block, uint32 num)
{
    buf_t* buf = buf_read(bitmap_block);
    
    uint32 byte_idx = num / 8;
    uint32 bit_idx  = num % 8;

    // 简单越界检查 (可选)
    if(byte_idx >= BLOCK_SIZE) {
        buf_release(buf);
        panic("bitmap_unset: index out of range");
    }

    // 检查是否已经是 0 了 (防止重复释放)
    // assert(buf->data[byte_idx] & (1 << bit_idx), "bitmap_unset: already free");

    // 将该位置 0
    buf->data[byte_idx] &= ~(1 << bit_idx);

    // 写回磁盘
    buf_write(buf);
    buf_release(buf);
}

uint32 bitmap_alloc_block()
{
    // 1. 在 data bitmap 中找到一个空闲位
    uint32 offset = bitmap_search_and_set(sb.data_bitmap_start);
    
    // 2. 将相对偏移量转换为物理块号
    // Data Area 的物理起始位置 + 相对偏移
    return sb.data_start + offset;
}

void bitmap_free_block(uint32 block_num)
{
    // 1. 计算相对偏移量
    // 物理块号 - Data Area 起始位置
    uint32 offset = block_num - sb.data_start;
    
    // 2. 在 bitmap 中清除对应位
    bitmap_unset(sb.data_bitmap_start, offset);
}

uint16 bitmap_alloc_inode()
{
    // Inode 编号本质上就是它在 Inode Table 中的索引
    // 所以直接返回 bitmap 的搜索结果即可
    return (uint16)bitmap_search_and_set(sb.inode_bitmap_start);
}

void bitmap_free_inode(uint16 inode_num)
{
    bitmap_unset(sb.inode_bitmap_start, (uint32)inode_num);
}

// 打印所有已经分配出去的bit序号(序号从0开始)
// for debug
void bitmap_print(uint32 bitmap_block_num)
{
    uint8 bit_cmp;
    uint32 byte, shift;

    printf("\nbitmap:\n");

    buf_t* buf = buf_read(bitmap_block_num);
    for(byte = 0; byte < BLOCK_SIZE; byte++) {
        bit_cmp = 1;
        for(shift = 0; shift <= 7; shift++) {
            if(bit_cmp & buf->data[byte])
               printf("bit %d is alloced\n", byte * 8 + shift);
            bit_cmp = bit_cmp << 1;
        }
    }
    printf("over\n");
    buf_release(buf);
}
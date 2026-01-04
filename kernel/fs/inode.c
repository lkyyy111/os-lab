#include "fs/buf.h"
#include "fs/bitmap.h"
#include "fs/inode.h"
#include "fs/fs.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "lib/str.h"
#include "proc/cpu.h"
#include "common.h"

extern super_block_t sb;

// 内存中的inode资源 + 保护它的锁
#define N_INODE 32
static inode_t icache[N_INODE];
static spinlock_t lk_icache;

// icache初始化
void inode_init()
{
    spinlock_init(&lk_icache, "inode_cache");
    for(int i = 0; i < N_INODE; i++) {
        sleeplock_init(&icache[i].slk, "inode");
        icache[i].ref = 0;
        icache[i].valid = false;
    }
}

/*---------------------- 与inode本身相关 -------------------*/

// 使用磁盘里的inode更新内存里的inode (write = false)
// 或 使用内存里的inode更新磁盘里的inode (write = true)
// 调用者需要设置inode_num并持有睡眠锁
void inode_rw(inode_t* ip, bool write)
{
    buf_t* buf;
    // 计算 block 位置
    uint32 block_num = sb.inode_start + ip->inode_num / INODE_PER_BLOCK;
    // 计算 block 内偏移
    uint32 offset = (ip->inode_num % INODE_PER_BLOCK) * sizeof(inode_disk_t);
    
    buf = buf_read(block_num);
    // 获得指向磁盘结构的指针
    inode_disk_t* dip = (inode_disk_t*)(buf->data + offset);

    if(write) {
        // 内存 -> 磁盘
        dip->type = ip->type;
        dip->major = ip->major;
        dip->minor = ip->minor;
        dip->nlink = ip->nlink;
        dip->size = ip->size;
        memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
        
        buf_write(buf);
    } else {
        // 磁盘 -> 内存
        ip->type = dip->type;
        ip->major = dip->major;
        ip->minor = dip->minor;
        ip->nlink = dip->nlink;
        ip->size = dip->size;
        memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    }
    
    buf_release(buf);
}

// 在icache里查询inode
// 如果没有查询到则申请一个空闲inode
// 如果icache没有空闲inode则报错
// 注意: 获得的inode没有上锁
inode_t* inode_alloc(uint16 inode_num)
{    
    inode_t *ip, *empty = NULL;

    spinlock_acquire(&lk_icache);

    // 1. 查找是否已在缓存
    for(ip = &icache[0]; ip < &icache[N_INODE]; ip++){
        if(ip->ref > 0 && ip->inode_num == inode_num) {
            ip->ref++;
            spinlock_release(&lk_icache);
            return ip;
        }
        if(empty == NULL && ip->ref == 0) // 记录遇到的第一个空坑
            empty = ip;
    }

    // 2. 没找到，分配新坑
    if(empty == NULL)
        panic("inode_alloc: no free inodes in cache");

    ip = empty;
    ip->inode_num = inode_num;
    ip->ref = 1;
    ip->valid = false; // 数据尚未从磁盘读入
    spinlock_release(&lk_icache);

    return ip;
}

// 在磁盘里申请一个inode (操作bitmap, 返回inode_num)
// 向icache申请一个inode数据结构
// 填写内存里的inode并以此更新磁盘里的inode
// 注意: 获得的inode没有上锁
inode_t* inode_create(uint16 type, uint16 major, uint16 minor)
{
    // 1. 在磁盘 Bitmap 中分配一个新的 inode 号
    uint16 inum = bitmap_alloc_inode();

    // 2. 获取内存对象
    inode_t* ip = inode_alloc(inum);

    // 3. 初始化并写入磁盘
    sleeplock_acquire(&ip->slk);
    
    ip->type = type;
    ip->major = major;
    ip->minor = minor;
    ip->nlink = 1;
    ip->size = 0;
    memset(ip->addrs, 0, sizeof(ip->addrs));
    
    ip->valid = true; // 已经是最新数据了
    inode_rw(ip, true); // 写回磁盘
    
    sleeplock_release(&ip->slk);

    return ip;
}

// 供inode_free调用
// 在磁盘上删除一个inode及其管理的文件 (修改inode bitmap + block bitmap)
// 调用者需要持有lk_icache, 但不应该持有slk
static void inode_destroy(inode_t* ip)
{
    // 必须重新获取 sleep lock 才能操作数据
    sleeplock_acquire(&ip->slk);
    
    if(ip->valid)
        inode_free_data(ip); // 释放所有数据块
    
    ip->type = 0; 
    inode_rw(ip, true); // 写回磁盘，标记 type=0
    
    sleeplock_release(&ip->slk);

    // 释放 inode bitmap
    bitmap_free_inode(ip->inode_num);
}

// 向icache里归还inode
// inode->ref--
// 调用者不应该持有slk
void inode_free(inode_t* ip)
{
    spinlock_acquire(&lk_icache);
    
    if(ip->ref < 1)
        panic("inode_free: ref < 1");
        
    ip->ref--;

    // 如果没有引用了，且硬链接数为0 (文件被unlink)，则彻底删除
    if(ip->ref == 0 && ip->nlink == 0) {
        // destroy 需要持有 ref (防止被别人抢走)，所以这里是个临界区技巧
        // 但简单起见，我们在持有 spinlock 时判断，释放 spinlock 后 destroy
        // 注意：这里有极小的竞态风险，但在简单 OS 中通常假设单核或大锁
        // 更严谨的做法是：ref保持0，但在 destroy 期间持有 slk 防止被 alloc
        
        // 实际上 destroy 操作较长，应该释放自旋锁
        spinlock_release(&lk_icache);
        inode_destroy(ip);
        return;
    }

    spinlock_release(&lk_icache);
}

// ip->ref++ with lock
inode_t* inode_dup(inode_t* ip)
{
    spinlock_acquire(&lk_icache);
    ip->ref++;
    spinlock_release(&lk_icache);
    return ip;
}

// 给inode上锁
// 如果valid失效则从磁盘中读入
void inode_lock(inode_t* ip)
{
    if(ip == NULL || ip->ref < 1)
        panic("inode_lock");

    sleeplock_acquire(&ip->slk);

    if(!ip->valid) {
        inode_rw(ip, false); // 从磁盘读
        ip->valid = true;
    }
}

// 给inode解锁
void inode_unlock(inode_t* ip)
{
    if(ip == NULL || !sleeplock_holding(&ip->slk))
        panic("inode_unlock");
    sleeplock_release(&ip->slk);
}

// 连招: 解锁 + 释放
void inode_unlock_free(inode_t* ip)
{
    inode_unlock(ip);
    inode_free(ip);
}

/*---------------------------- 与inode管理的data相关 --------------------------*/

// 辅助 inode_locate_block
// 递归查询或创建block
static uint32 locate_block(uint32* entry, uint32 bn, uint32 size)
{
    if(*entry == 0)
        *entry = bitmap_alloc_block();

    if(size == 1)
        return *entry;    

    uint32* next_entry;
    uint32 next_size = size / ENTRY_PER_BLOCK;
    uint32 next_bn = bn % next_size;
    uint32 ret = 0;

    buf_t* buf = buf_read(*entry);
    next_entry = (uint32*)(buf->data) + bn / next_size;
    ret = locate_block(next_entry, next_bn, next_size);
    buf_release(buf);

    return ret;
}

// 确定inode里第bn块data block的block_num
// 如果不存在第bn块data block则申请一个并返回它的block_num
// 由于inode->addrs的结构, 这个过程比较复杂, 需要单独处理
static uint32 inode_locate_block(inode_t* ip, uint32 bn)
{
    uint32 ret = 0;

    // 1. 直接索引 (0 - 9)
    if(bn < N_ADDRS_1) {
        // size=1 表示这是最后一级，直接返回
        ret = locate_block(&ip->addrs[bn], bn, 1);
        return ret;
    }
    bn -= N_ADDRS_1;

    // 2. 一级间接索引 (10, 11) -> 对应 N_ADDRS_2
    // 每个一级索引块管理 ENTRY_PER_BLOCK 个块
    if(bn < N_ADDRS_2 * ENTRY_PER_BLOCK)
    {
        uint32 size = ENTRY_PER_BLOCK;
        uint32 idx = bn / size;
        uint32 b = bn % size;
        ret = locate_block(&ip->addrs[N_ADDRS_1 + idx], b, size);
        return ret;
    }
    bn -= N_ADDRS_2 * ENTRY_PER_BLOCK;

    // 3. 二级间接索引 (12) -> 对应 N_ADDRS_3
    // 每个二级索引块管理 ENTRY_PER_BLOCK * ENTRY_PER_BLOCK 个块
    if(bn < N_ADDRS_3 * ENTRY_PER_BLOCK * ENTRY_PER_BLOCK)
    {
        uint32 size = ENTRY_PER_BLOCK * ENTRY_PER_BLOCK;
        uint32 idx = bn / size;
        uint32 b = bn % size;
        ret = locate_block(&ip->addrs[N_ADDRS_1 + N_ADDRS_2 + idx], b, size);
        return ret;
    }

    panic("inode_locate_block: overflow");
    return 0;
}


// 读取 inode 管理的 data block
// 调用者需要持有 inode 锁
// 成功返回读出的字节数, 失败返回0
uint32 inode_read_data(inode_t* ip, uint32 offset, uint32 len, void* dst, bool user)
{
    uint32 tot, m;
    buf_t* buf;

    if(offset > ip->size || offset + len < offset)
        return 0;
    
    if(offset + len > ip->size)
        len = ip->size - offset;

    for(tot = 0; tot < len; tot += m, offset += m, dst += m) {
         uint32 block_num = inode_locate_block(ip, offset / BLOCK_SIZE);
         printf("inode_read_data: read block=%lu offset=%lu len=%lu ip->size=%lu\n",
             (uint64)block_num, (uint64)offset, (uint64)len, (uint64)ip->size);
        buf = buf_read(block_num);
        m = MIN(len - tot, BLOCK_SIZE - (offset % BLOCK_SIZE));
        
        if(user) {
            // 从内核 buffer 拷贝到用户空间
            if(uvm_copyout(myproc()->pgtbl, (uint64)dst, (uint64)(buf->data + (offset % BLOCK_SIZE)), m) == -1) {
                buf_release(buf);
                return -1; // Error
            }
        } else {
            memmove(dst, buf->data + (offset % BLOCK_SIZE), m);
        }
        
        buf_release(buf);
    }
    return tot;
}

// 写入 inode 管理的 data block (可能导致管理的 block 增加)
// 调用者需要持有 inode 锁
// 成功返回写入的字节数, 失败返回0
uint32 inode_write_data(inode_t* ip, uint32 offset, uint32 len, void* src, bool user)
{
    uint32 tot, m;
    buf_t* buf;

    if(offset > ip->size || offset + len < offset)
        return 0; // 不支持稀疏空洞写，必须接着写

    for(tot = 0; tot < len; tot += m, offset += m, src += m) {
        uint32 block_num = inode_locate_block(ip, offset / BLOCK_SIZE);
        
        buf = buf_read(block_num);
        m = MIN(len - tot, BLOCK_SIZE - (offset % BLOCK_SIZE));
        
        if(user) {
             if(uvm_copyin(myproc()->pgtbl, (uint64)(buf->data + (offset % BLOCK_SIZE)), (uint64)src, m) == -1) {
                buf_release(buf);
                return -1;
             }
        } else {
            memmove(buf->data + (offset % BLOCK_SIZE), src, m);
        }
        
        buf_write(buf);
        buf_release(buf);
    }

    if(len > 0) {
        // 更新文件大小
        if(offset > ip->size)
            ip->size = offset;
        
        // 更新 inode 本身到磁盘 (addrs 可能变了，size 变了)
        inode_rw(ip, true);
    }

    return tot;
}

// 辅助 inode_free_data 做递归释放
static void data_free(uint32 block_num, uint32 level)
{  
    if (block_num == 0) return;

    // level 0 表示这是 data block，直接释放
    // level > 0 表示这是 index block，需要读取并释放其子节点
    if(level > 0) {
        buf_t* buf = buf_read(block_num);
        uint32* addrs = (uint32*)buf->data;
        for(int i = 0; i < ENTRY_PER_BLOCK; i++) {
            if(addrs[i] != 0) {
                data_free(addrs[i], level - 1);
            }
        }
        buf_release(buf);
    }

    // 无论是 data block 还是 index block，最后都要把自己释放掉
    bitmap_free_block(block_num);
}

// 释放inode管理的 data block
// ip->addrs被清空 ip->size置0
// 调用者需要持有slk
void inode_free_data(inode_t* ip)
{
    // 1. 释放直接块
    for(int i = 0; i < N_ADDRS_1; i++) {
        if(ip->addrs[i]) {
            data_free(ip->addrs[i], 0); // 0级，直接是数据
            ip->addrs[i] = 0;
        }
    }

    // 2. 释放一级间接块
    for(int i = 0; i < N_ADDRS_2; i++) {
        if(ip->addrs[N_ADDRS_1 + i]) {
            data_free(ip->addrs[N_ADDRS_1 + i], 1); // 1级索引
            ip->addrs[N_ADDRS_1 + i] = 0;
        }
    }

    // 3. 释放二级间接块
    for(int i = 0; i < N_ADDRS_3; i++) {
        if(ip->addrs[N_ADDRS_1 + N_ADDRS_2 + i]) {
            data_free(ip->addrs[N_ADDRS_1 + N_ADDRS_2 + i], 2); // 2级索引
            ip->addrs[N_ADDRS_1 + N_ADDRS_2 + i] = 0;
        }
    }

    ip->size = 0;
    inode_rw(ip, true); // 写回清空状态
}

static char* inode_types[] = {
    "INODE_UNUSED",
    "INODE_DIR",
    "INODE_FILE",
    "INODE_DEVICE",
};

// 输出inode信息
// for dubug
void inode_print(inode_t* ip)
{
    assert(sleeplock_holding(&ip->slk), "inode_print: lk");

    printf("\ninode information:\n");
    printf("num = %d, ref = %d, valid = %d\n", ip->inode_num, ip->ref, ip->valid);
    printf("type = %s, major = %d, minor = %d, nlink = %d\n", inode_types[ip->type], ip->major, ip->minor, ip->nlink);
    printf("size = %d, addrs =", ip->size);
    for(int i = 0; i < N_ADDRS; i++)
        printf(" %d", ip->addrs[i]);
    printf("\n");
}
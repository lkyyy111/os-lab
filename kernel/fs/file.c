#include "fs/fs.h"
#include "fs/buf.h"
#include "fs/dir.h"
#include "fs/bitmap.h"
#include "fs/inode.h"
#include "fs/file.h"
#include "mem/vmem.h"
#include "proc/cpu.h"
#include "lib/print.h"
#include "lib/lock.h"
#include "lib/str.h"
#include "common.h"


extern uint32 console_read(uint32 len, uint64 dst, bool user_dst);
extern uint32 console_write(uint32 len, uint64 src, bool user_src);

// 设备列表(读写接口)
dev_t devlist[N_DEV];

// ftable + 保护它的锁
#define N_FILE 32
file_t ftable[N_FILE];
spinlock_t lk_ftable;

// ftable初始化 + devlist初始化
void file_init()
{
    spinlock_init(&lk_ftable, "ftable");

    // 初始化设备表
    // DEV_CONSOLE 通常定义为 1
    devlist[DEV_CONSOLE].read = console_read;
    devlist[DEV_CONSOLE].write = console_write;
}


// alloc file_t in ftable
// 失败则panic
file_t* file_alloc()
{
    spinlock_acquire(&lk_ftable);
    for(int i = 0; i < N_FILE; i++){
        if(ftable[i].ref == 0){
            ftable[i].ref = 1;
            spinlock_release(&lk_ftable);
            return &ftable[i];
        }
    }
    spinlock_release(&lk_ftable);
    panic("file_alloc: no free file struct");
    return NULL; 
}

// 创建设备文件(供proczero创建console)
file_t* file_create_dev(char* path, uint16 major, uint16 minor)
{
    file_t* f = file_alloc();
    f->type = FD_DEVICE;
    f->readable = true;
    f->writable = true;
    f->major = major;
    f->ip = NULL; // 设备文件(字符设备)在这个简易 OS 中通常不绑定 inode，或者直接操作
    return f;
}

// 打开一个文件
file_t* file_open(char *path, uint32 open_mode)
{
    inode_t *ip;
    
    // 修正: O_CREATE -> MODE_CREATE
    if(open_mode & MODE_CREATE){
        // path_create_inode 返回的是已上锁的 inode
        ip = path_create_inode(path, FT_FILE, 0, 0);
        if(ip == NULL) return NULL;
    } else {
        if((ip = path_to_inode(path)) == NULL){
            return NULL;
        }
        inode_lock(ip);
        
        // 修正: O_RDONLY check -> 检查是否只读
        // 如果是目录 且 想要写入 (open_mode 包含 MODE_WRITE) -> 报错
        if(ip->type == FT_DIR && (open_mode & MODE_WRITE)){
            inode_unlock_free(ip);
            return NULL;
        }
    }

    file_t *f = file_alloc();
    if(f == NULL){
        inode_unlock_free(ip);
        return NULL;
    }

    // 设置文件类型
    if(ip->type == FT_DEVICE){
        f->type = FD_DEVICE;
        f->major = ip->major;
    } else if(ip->type == FT_DIR) {
        f->type = FD_DIR;    // 适配你的 FD_DIR
        f->offset = 0;          
    } else {
        f->type = FD_FILE;   // 适配你的 FD_FILE
        f->offset = 0;       // file.h 里是 offset
    }
    
    f->ip = ip;
    
    // 修正: 使用 MODE_READ / MODE_WRITE
    f->readable = (open_mode & MODE_READ) ? true : false;
    f->writable = (open_mode & MODE_WRITE) ? true : false;

    inode_unlock(ip);
    
    return f;
}


// 释放一个file
void file_close(file_t* file)
{
    spinlock_acquire(&lk_ftable);
    
    if(file->ref < 1) 
        panic("file_close: ref < 1");
        
    file->ref--;
    
    if(file->ref > 0){
        spinlock_release(&lk_ftable);
        return;
    }

    // ref == 0, 真正的回收资源
    uint16 type = file->type;
    inode_t* ip = file->ip;
    
    file->type = FD_UNUSED;
    file->ip = NULL;
    file->offset = 0;
    
    spinlock_release(&lk_ftable);

    // 如果关联了 inode，需要释放 inode 的引用
    if(type == FD_FILE || type == FD_DIR){
        if(ip) inode_free(ip); // iput 会处理 ref count 和释放内存
    }
}

// 文件内容读取
// 返回读取到的字节数
uint32 file_read(file_t* file, uint32 len, uint64 dst, bool user)
{
    if(!file->readable) return -1;

    if(file->type == FD_DEVICE){
        // 调用设备驱动的读函数
        if(file->major < 0 || file->major >= N_DEV || !devlist[file->major].read)
            return -1;
        return devlist[file->major].read(len, dst, user);
    } 
    else if(file->type == FD_FILE || file->type == FD_DIR){
        // 读取普通文件或目录
        inode_lock(file->ip);
        // inode_read_data 通常定义为: inode_read_data(inode, offset, len, dst_addr, user_dst)
        int n = inode_read_data(file->ip, file->offset, len, (void*)dst, user);
        if(n > 0)
            file->offset += n;
        inode_unlock(file->ip);
        return n;
    }
    return -1;
}

// 文件内容写入
// 返回写入的字节数
uint32 file_write(file_t* file, uint32 len, uint64 src, bool user)
{
    if(!file->writable) return -1;

    if(file->type == FD_DEVICE){
        // 调用设备驱动的写函数
        if(file->major < 0 || file->major >= N_DEV || !devlist[file->major].write)
            return -1;
        return devlist[file->major].write(len, src, user);
    } 
    else if(file->type == FD_FILE){
        // 写入普通文件
        inode_lock(file->ip);
        // inode_write_data 通常定义为: inode_write_data(inode, offset, len, src_addr, user_src)
        // 并且可能需要处理写满的情况，这里简化处理
        int n = inode_write_data(file->ip, file->offset, len, (void*)src, user);
        if(n > 0)
            file->offset += n;
        inode_unlock(file->ip);
        return n;
    }
    return -1;
}


// flags 可能取值
#define LSEEK_SET 0  // file->offset = offset
#define LSEEK_ADD 1  // file->offset += offset
#define LSEEK_SUB 2  // file->offset -= offset

// 修改file->offset (只针对FD_FILE类型的文件)
uint32 file_lseek(file_t* file, uint32 offset, int flags)
{
    if(file->type != FD_FILE) return -1;

    inode_lock(file->ip);
    
    uint32 new_off = file->offset;

    switch(flags){
        case LSEEK_SET:
            new_off = offset;
            break;
        case LSEEK_ADD:
            new_off += offset;
            break;
        case LSEEK_SUB:
            new_off -= offset;
            break;
    }


    file->offset = new_off;
    inode_unlock(file->ip);
    
    return new_off;
}

// file->ref++ with lock
file_t* file_dup(file_t* file)
{
    spinlock_acquire(&lk_ftable);
    assert(file->ref > 0, "file_dup: ref");
    file->ref++;
    spinlock_release(&lk_ftable);
    return file;
}

// 获取文件状态
int file_stat(file_t* file, uint64 addr)
{
    file_state_t state;
    if(file->type == FD_FILE || file->type == FD_DIR)
    {
        inode_lock(file->ip);
        state.type = file->ip->type;
        state.inode_num = file->ip->inode_num;
        state.nlink = file->ip->nlink;
        state.size = file->ip->size;
        inode_unlock(file->ip);

        uvm_copyout(myproc()->pgtbl, addr, (uint64)&state, sizeof(file_state_t));
    }
    return -1;
}
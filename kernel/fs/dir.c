#include "fs/fs.h"
#include "fs/buf.h"
#include "fs/inode.h"
#include "fs/dir.h"
#include "fs/bitmap.h"
#include "lib/str.h"
#include "lib/print.h"
#include "proc/cpu.h"
#include "mem/vmem.h"
#include "proc/cpu.h"

static uint32 dir_get_block(inode_t* pip) {
    if (pip->addrs[0] == 0) {
        pip->addrs[0] = bitmap_alloc_block();
        // 新分配的目录块必须清零，否则会有垃圾数据被当作目录项
        buf_t* buf = buf_read(pip->addrs[0]);
        memset(buf->data, 0, BLOCK_SIZE);
        buf_write(buf);
        buf_release(buf);
        pip->size = BLOCK_SIZE; // 更新大小
        inode_rw(pip, true);    // 写回 inode
    }
    return pip->addrs[0];
}

// 对目录文件的简化性假设: 每个目录文件只包括一个block
// 也就是每个目录下最多 BLOCK_SIZE / sizeof(dirent_t) = 32 个目录项

// 查询一个目录项是否在目录里
// 成功返回这个目录项的inode_num
// 失败返回INODE_NUM_UNUSED
// ps: 调用者需持有pip的锁
uint16 dir_search_entry(inode_t *pip, char *name)
{
    // 目录必须有数据块
    if(pip->addrs[0] == 0) return INODE_NUM_UNUSED;

    buf_t* buf = buf_read(pip->addrs[0]);
    dirent_t* de;
    
    for(int offset = 0; offset < BLOCK_SIZE; offset += sizeof(dirent_t)) {
        de = (dirent_t*)(buf->data + offset);
        // 如果 inode_num 不为 0 且名字匹配
        if(de->name[0] != 0 && strncmp(name, de->name, DIR_NAME_LEN) == 0) {
            uint16 inum = de->inode_num;
            buf_release(buf);
            return inum;
        }
    }
    
    buf_release(buf);
    return INODE_NUM_UNUSED;
}

// 在pip目录下添加一个目录项
// 成功返回这个目录项的偏移量 (同时更新pip->size)
// 失败返回BLOCK_SIZE (没有空间 或 发生重名)
// ps: 调用者需持有pip的锁
uint32 dir_add_entry(inode_t *pip, uint16 inode_num, char *name)
{
    // 1. 检查重名
    if(dir_search_entry(pip, name) != INODE_NUM_UNUSED) {
        return BLOCK_SIZE;
    }

    uint32 block_num = dir_get_block(pip);
    buf_t* buf = buf_read(block_num);
    dirent_t* de;
    int offset;
    
    // 2. 寻找空位
    for(offset = 0; offset < BLOCK_SIZE; offset += sizeof(dirent_t)) {
        de = (dirent_t*)(buf->data + offset);
        if(de->name[0] == 0) { // 找到空位
            goto found;
        }
    }

    // 没找到空位
    buf_release(buf);
    return BLOCK_SIZE;

found:
    de->inode_num = inode_num;
    strncpy(de->name, name, DIR_NAME_LEN);
    buf_write(buf);
    buf_release(buf);
    
    // 大小不需要改，因为固定是一个Block，或者维护有效字节数
    // 但通常目录大小就是Block的整数倍。如果需要精确维护:
    // pip->size = max(pip->size, offset + sizeof(dirent_t));
    // inode_rw(pip, true);

    return offset;
}

// 在pip目录下删除一个目录项
// 成功返回这个目录项的inode_num
// 失败返回INODE_NUM_UNUSED
// ps: 调用者需持有pip的锁
uint16 dir_delete_entry(inode_t *pip, char *name)
{
    if(pip->addrs[0] == 0) return INODE_NUM_UNUSED;

    buf_t* buf = buf_read(pip->addrs[0]);
    dirent_t* de;
    uint16 inum = INODE_NUM_UNUSED;

    for(int offset = 0; offset < BLOCK_SIZE; offset += sizeof(dirent_t)) {
        de = (dirent_t*)(buf->data + offset);
        if(de->name[0] != 0 && strncmp(name, de->name, DIR_NAME_LEN) == 0) {
            inum = de->inode_num;
            // 清除目录项
            memset(de, 0, sizeof(dirent_t));
            buf_write(buf);
            break;
        }
    }

    buf_release(buf);
    return inum;
}

// 把目录下的有效目录项复制到dst (dst区域长度为len)
// 返回读到的字节数 (sizeof(dirent_t)*n)
// 调用者需要持有pip的锁
uint32 dir_get_entries(inode_t* pip, uint32 len, void* dst, bool user)
{
    if(pip->addrs[0] == 0) return 0;
    
    buf_t* buf = buf_read(pip->addrs[0]);
    dirent_t* de;
    uint32 copied = 0;
    uint32 dst_off = 0;

    for(int offset = 0; offset < BLOCK_SIZE; offset += sizeof(dirent_t)) {
        if(copied + sizeof(dirent_t) > len) break;

        de = (dirent_t*)(buf->data + offset);
        if(de->name[0] != 0) {
            if(user) {
                if(uvm_copyout(myproc()->pgtbl, (uint64)dst + dst_off, (uint64)de, sizeof(dirent_t)) < 0) {
                    buf_release(buf);
                    return -1;
                }
            } else {
                memmove(dst + dst_off, de, sizeof(dirent_t));
            }
            copied += sizeof(dirent_t);
            dst_off += sizeof(dirent_t);
        }
    }

    buf_release(buf);
    return copied;
}

// 改变进程里存储的当前目录
// 成功返回0 失败返回-1
uint32 dir_change(char* path)
{
    inode_t* ip = path_to_inode(path);
    if(ip == NULL) return -1;

    // inode_lock(ip);
    if(ip->type != FT_DIR) {
        inode_unlock_free(ip);
        return -1;
    }
    inode_unlock(ip);

    // 更新进程的 cwd
    // 注意: 这里需要处理原 cwd 的释放和新 cwd 的引用
    proc_t* p = myproc();
    inode_t* old = p->cwd;
    
    p->cwd = ip; // 接管 ip 的引用 (path_to_inode 已经 ref++ 过了)
    
    if(old) inode_free(old); // 释放旧的

    return 0;
}

// 输出一个目录下的所有有效目录项
// for debug
// ps: 调用者需持有pip的锁
void dir_print(inode_t *pip)
{
    assert(sleeplock_holding(&pip->slk), "dir_print: lock");

    printf("\ninode_num = %d dirents:\n", pip->inode_num);
    if (pip->addrs[0] == 0){
        printf("(empty)");
        return;
    }
    dirent_t *de;
    buf_t *buf = buf_read(pip->addrs[0]);
    for (uint32 offset = 0; offset < BLOCK_SIZE; offset += sizeof(dirent_t))
    {
        de = (dirent_t *)(buf->data + offset);
        if (de->name[0] != 0)
            printf("inum = %d dirent = %s\n", de->inode_num, de->name);
    }
    buf_release(buf);
}

/*----------------------- 路径(一串目录和文件) -------------------------*/

// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
static char *skip_element(char *path, char *name)
{
    while(*path == '/') path++;
    if(*path == 0) return 0;

    char *s = path;
    while (*path != '/' && *path != 0)
        path++;

    int len = path - s;
    if (len >= DIR_NAME_LEN) {
        memmove(name, s, DIR_NAME_LEN);
    } else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

// 查找路径path对应的inode (find_parent = false)
// 查找路径path对应的inode的父节点 (find_parent = true)
// 供两个上层函数使用
// 失败返回NULL
static inode_t* search_inode(char* path, char* name, bool find_parent)
{
    inode_t *ip, *next;

    printf("search_inode: path='%s' find_parent=%d\n", path, find_parent);
    // 1. 确定起点
    if(*path == '/') {
        ip = inode_alloc(INODE_ROOT);
    } else {
        ip = inode_dup(myproc()->cwd);
    }

    while((path = skip_element(path, name)) != 0) {
        printf("search_inode: element='%s' rest='%s'\n", name, path);
        inode_lock(ip);

        // 如果不是目录，无法继续查找
        if(ip->type != FT_DIR) {
            inode_unlock_free(ip);
            return NULL;
        }

        // 2. 如果是找父节点，且路径已结束(name里是最后一级)，则当前ip就是父节点
        if(find_parent && *path == 0) {
            // 返回上锁的父节点
            return ip; // Caller must unlock
        }

        // 3. 在目录中查找下一级
        uint16 next_inum = dir_search_entry(ip, name);
        printf("search_inode: lookup '%s' -> inum=%d\n", name, (int)next_inum);
        if(next_inum == INODE_NUM_UNUSED) {
            inode_unlock_free(ip);
            return NULL;
        }

        // 4. 获取下一级 inode
        // 关键点：Hand-over-hand locking
        // 先解锁当前，再获取下一个 (避免持锁等待IO)
        inode_unlock(ip);
        
        next = inode_alloc(next_inum);
        if(next == NULL) {
            inode_free(ip);
            return NULL;
        }
        
        inode_free(ip); // 释放对当前目录的引用
        ip = next;
    }

    // 循环结束
    if(find_parent) {
        // 如果是要找父节点，但循环走完了还没返回，说明路径为空或只有"/"
        inode_free(ip);
        return NULL;
    }

    // 找到了目标 inode，上锁并返回
    inode_lock(ip);
    printf("search_inode: found inode type=%d size=%lu\n", (int)ip->type, (uint64)ip->size);
    return ip;
}

// 找到path对应的inode
inode_t* path_to_inode(char* path)
{
    char name[DIR_NAME_LEN];
    return search_inode(path, name, false);
}

// 找到path对应的inode的父节点
// path最后的目录名放入name指向的空间
inode_t* path_to_pinode(char* path, char* name)
{
    return search_inode(path, name, true);
}

// 如果path对应的inode存在则返回inode
// 如果path对应的inode不存在则创建inode
// 失败返回NULL
inode_t* path_create_inode(char* path, uint16 type, uint16 major, uint16 minor)
{
    char name[DIR_NAME_LEN];
    inode_t *dp, *ip;

    // 1. 找到父目录
    dp = path_to_pinode(path, name);
    if(dp == NULL) return NULL;
    
    // dp 此时是上锁状态

    // 2. 检查文件是否已存在
    uint16 inum = dir_search_entry(dp, name);
    if(inum != INODE_NUM_UNUSED) {
        inode_unlock_free(dp);
        ip = inode_alloc(inum);
        inode_lock(ip);
        // 如果存在，type 应该要一致，这里简化处理，直接返回旧的
        return ip; 
    }

    // 3. 分配新 inode
    ip = inode_create(type, major, minor);
    if(ip == NULL) {
        inode_unlock_free(dp);
        return NULL;
    }
    // inode_create 返回的 ip 只有 ref，没有 lock，但在 create 内部初始化时使用了 slk。
    // 为了后续操作，我们需要锁住它 (实际上 inode_create 内部实现通常写回后就释放锁了)
    inode_lock(ip);

    // 4. 如果是创建目录，需要添加 . 和 ..
    if(type == FT_DIR) {
        // . 指向自己
        dir_add_entry(ip, ip->inode_num, "."); 
        // .. 指向父目录
        dir_add_entry(ip, dp->inode_num, "..");
    }

    // 5. 将新文件添加到父目录
    if(dir_add_entry(dp, ip->inode_num, name) == BLOCK_SIZE) {
        // 添加失败（比如满了）
        inode_unlock_free(ip); // 这里应该触发 destroy 流程，简化起见直接释放
        inode_unlock_free(dp);
        return NULL;
    }

    // 如果创建的是目录，父目录的链接数增加 (子目录的 ".." 指向父目录)
    if(type == FT_DIR) {
        dp->nlink++;
        inode_rw(dp, true);
    }

    inode_unlock_free(dp);

    // 返回上锁的新 inode
    return ip; 
}

// 文件链接(目录不能被链接)
// 本质是创建一个目录项, 这个目录项的inode_num是存在的而不用申请
// 成功返回0 失败返回-1
uint32 path_link(char* old_path, char* new_path)
{
    char name[DIR_NAME_LEN];
    inode_t *ip, *dp;

    // 1. 获取源文件 inode
    ip = path_to_inode(old_path);
    if(ip == NULL) return -1;
    
    if(ip->type == FT_DIR) {
        inode_unlock_free(ip);
        return -1; // 不允许硬链接目录
    }
    
    ip->nlink++;
    inode_rw(ip, true); // 写盘更新 nlink
    inode_unlock(ip); 

    // 2. 获取目标父目录
    dp = path_to_pinode(new_path, name);
    if(dp == NULL) {
        // 回滚 nlink
        inode_lock(ip);
        ip->nlink--;
        inode_rw(ip, true);
        inode_unlock_free(ip);
        return -1;
    }

    // 3. 添加目录项
    if(dir_add_entry(dp, ip->inode_num, name) == BLOCK_SIZE) {
        inode_unlock_free(dp);
        // 回滚 nlink
        inode_lock(ip);
        ip->nlink--;
        inode_rw(ip, true);
        inode_unlock_free(ip);
        return -1;
    }

    inode_unlock_free(dp);
    inode_free(ip); // 释放掉 path_to_inode 带来的引用

    return 0;
}

// 检查一个unlink操作是否合理
// 调用者需要持有ip的锁
// 在path_unlink()中调用
static bool check_unlink(inode_t* ip)
{
    assert(sleeplock_holding(&ip->slk), "check_unlink: slk");

    uint8 tmp[sizeof(dirent_t) * 3];
    uint32 read_len;
    
    read_len = dir_get_entries(ip, sizeof(dirent_t) * 3, tmp, false);
    
    if(read_len == sizeof(dirent_t) * 3) {
        return false;
    } else if(read_len == sizeof(dirent_t) * 2) {
        return true;
    } else {
        panic("check_unlink: read_len");
        return false;
    }
}

// 文件删除链接
uint32 path_unlink(char* path)
{
    char name[DIR_NAME_LEN];
    inode_t *dp, *ip;

    // 1. 获取父目录 (dp 已上锁)
    dp = path_to_pinode(path, name);
    if(dp == NULL) return -1;
    
    // 禁止 unlink "." and ".."
    if(strncmp(name, ".", DIR_NAME_LEN) == 0 || strncmp(name, "..", DIR_NAME_LEN) == 0) {
        inode_unlock_free(dp);
        return -1;
    }

    // 2. 查找目标文件的 inode 编号
    uint16 inum = dir_search_entry(dp, name);
    if(inum == INODE_NUM_UNUSED) {
        inode_unlock_free(dp); // 没找到文件
        return -1;
    }

    // 3. 获取目标 inode (ip) 并上锁
    // 此时我们同时持有 dp(父) 和 ip(子) 的锁
    // 这是安全的，因为文件系统层级锁顺序通常是 自上而下
    ip = inode_alloc(inum);
    inode_lock(ip);
    
    if(ip->nlink < 1) panic("path_unlink: nlink < 1");

    // 4. 【关键修正】先检查是否允许删除
    // 如果是目录，必须为空才能 unlink
    if(ip->type == FT_DIR && !check_unlink(ip)) {
        // 目录非空，禁止删除！
        // 此时我们还没有修改磁盘数据，直接解锁退出即可，非常安全
        inode_unlock_free(ip);
        inode_unlock_free(dp);
        return -1;
    }

    // 5. 检查通过，现在才开始动手修改磁盘
    // 从父目录中删除目录项
    dir_delete_entry(dp, name); // 只操作 dp 的数据块

    // 6. 更新 inode 信息
    // 如果删除的是目录，父目录的链接数也要减 (因为子目录里的 ".." 没了)
    if(ip->type == FT_DIR) {
        dp->nlink--;
        inode_rw(dp, true);
    }

    // 目标文件链接数减一
    ip->nlink--;
    inode_rw(ip, true);
    
    // 7. 释放资源
    inode_unlock_free(ip); // 如果 nlink=0，这里会自动触发 inode_free -> inode_destroy
    inode_unlock_free(dp);

    return 0;
}
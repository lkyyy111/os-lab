#include "common.h"
#include "fs/inode.h"
#include "fs/file.h"
#include "fs/dir.h"
#include "proc/proc.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "lib/str.h"
#include "lib/print.h"
#include "riscv.h"
#include "memlayout.h"
#include "proc/cpu.h"

// --------------------------------------------------
// ELF 格式定义
// --------------------------------------------------
#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ELF_MAXARGS 32  // 最大支持的命令行参数个数


// ELF 文件头
struct elfhdr {
    uint32 magic;
    uint8  elf[12];
    uint16 type;
    uint16 machine;
    uint32 version;
    uint64 entry;
    uint64 phoff;
    uint64 shoff;
    uint32 flags;
    uint16 ehsize;
    uint16 phentsize;
    uint16 phnum;
    uint16 shentsize;
    uint16 shnum;
    uint16 shstrndx;
};

// 程序段头 (Program Header)
struct proghdr {
    uint32 type;
    uint32 flags;
    uint64 off;
    uint64 vaddr;
    uint64 paddr;
    uint64 filesz;
    uint64 memsz;
    uint64 align;
};

#define ELF_PROG_LOAD 1
#define ELF_PROG_FLAG_EXEC 1
#define ELF_PROG_FLAG_WRITE 2
#define ELF_PROG_FLAG_READ 4

// 外部声明：在 proc.c 中实现的初始化页表函数
extern pgtbl_t proc_pgtbl_init(uint64 trapframe);

// --------------------------------------------------
// 辅助函数
// --------------------------------------------------

// 从 inode 加载数据到指定的虚拟地址 va
// 该 va 必须在 pgtbl 中已映射，且 iread 可以直接写入对应的物理地址
static int load_seg(pgtbl_t pgtbl, uint64 va, struct inode *ip, uint64 offset, uint32 len)
{
    uint64 i, n, pa;
    pte_t *pte;
    
    for(i = 0; i < len; i += PGSIZE){
        pte = vm_getpte(pgtbl, va + i, false);
        if(pte == 0 || !(*pte & PTE_V))
            return -1; // 地址未映射
        
        pa = PTE_TO_PA(*pte);
        n = PGSIZE;
        if(i + n > len)
            n = len - i;
            
        // 从文件读取 n 字节到物理地址 pa
        if(inode_read_data(ip, offset + i, n, (void*)pa, false) != n) {
            printf("DEBUG: load_seg read fail off=%lu len=%lu va=0x%lx pa=0x%lx\n",
                   (uint64)offset + i, (uint64)n, va + i, pa);
            return -1;
        }
    }
    return 0;
}

// --------------------------------------------------
// proc_exec 实现 (Debug Version)
// --------------------------------------------------
int proc_exec(char *path, char **argv)
{
    int i, off;
    uint64 argc, sz = 0, sp, ustack[ELF_MAXARGS+1], stackbase;
    struct elfhdr elf;
    struct inode *ip;
    struct proghdr ph;
    pgtbl_t pgtbl = 0, oldpgtbl;
    struct proc *p = myproc();
    
    printf("DEBUG: proc_exec: begin loading path='%s'\n", path);

    // 1. 打开文件
    if((ip = path_to_inode(path)) == 0){
        printf("DEBUG: proc_exec: path_to_inode failed for '%s'", path);
        // 尝试去掉 ./ 再试一次，有的简单文件系统不支持 ./
        if(path[0] == '.' && path[1] == '/') {
             if((ip = path_to_inode(path + 2)) == 0) {
                 printf("DEBUG: proc_exec: path_to_inode failed for '%s' too", path+2);
                 return -1;
             }
             printf("DEBUG: proc_exec: found file using path '%s'", path+2);
        } else {
            return -1;
        }
    }
    // 注意：path_to_inode 返回的 ip 已经上锁，不要重复上锁

    // 2. 读取并检查 ELF 头
    if(inode_read_data(ip, 0, sizeof(elf), (void*)&elf, false) != sizeof(elf)) {
        printf("DEBUG: proc_exec: read elf header failed");
        goto bad;
    }
        
    if(elf.magic != ELF_MAGIC) {
        printf("DEBUG: proc_exec: elf magic mismatch. Found %x, expect %x", elf.magic, ELF_MAGIC);
        goto bad;
    }

    printf("DEBUG: proc_exec: elf header ok, phnum=%d phoff=%lu entry=0x%lx\n", elf.phnum, (uint64)elf.phoff, elf.entry);

    // 3. 创建新页表
    pgtbl = proc_pgtbl_init((uint64)p->tf);
    if(pgtbl == 0) {
        printf("DEBUG: proc_exec: proc_pgtbl_init failed");
        goto bad;
    }

    // 4. 加载程序段
    sz = 0;
    for(i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)){
        if(inode_read_data(ip, off, sizeof(ph), (void*)&ph, false) != sizeof(ph)) {
            printf("DEBUG: proc_exec: read proghdr %d failed\n", i);
            goto bad;
        }
        
        if(ph.type != ELF_PROG_LOAD)
            continue;
        
        if(ph.memsz < ph.filesz) {
            printf("DEBUG: proc_exec: memsz < filesz (ph %d)\n", i);
            goto bad;
        }
        
        if(ph.vaddr + ph.memsz < ph.vaddr) {
            printf("DEBUG: proc_exec: vaddr overflow (ph %d)\n", i);
            goto bad;
        }

        uint64 va_start = PGROUNDDOWN(ph.vaddr);
        uint64 va_end = PGROUNDUP(ph.vaddr + ph.memsz);
        
        int perm = PTE_U;
        if(ph.flags & ELF_PROG_FLAG_READ) perm |= PTE_R;
        if(ph.flags & ELF_PROG_FLAG_WRITE) perm |= PTE_W;
        if(ph.flags & ELF_PROG_FLAG_EXEC) perm |= PTE_X;

        printf("DEBUG: proc_exec: ph %d vaddr=0x%lx filesz=%lu memsz=%lu perms=%x va_start=0x%lx va_end=0x%lx\n",
               i, ph.vaddr, (uint64)ph.filesz, (uint64)ph.memsz, perm, va_start, va_end);

        for(uint64 a = va_start; a < va_end; a += PGSIZE){
            char *mem = (char*)pmem_alloc(false);
            if(mem == 0) {
                printf("DEBUG: proc_exec: pmem_alloc failed (out of memory) ph %d a=0x%lx\n", i, a);
                goto bad;
            }
            
            // 假设恒等映射，直接转
            if(vm_mappages(pgtbl, a, (uint64)mem, PGSIZE, perm) != 0){
                printf("DEBUG: proc_exec: vm_mappages failed at va=0x%lx ph %d\n", a, i);
                pmem_free((uint64)mem, false);
                goto bad;
            }
        }

        if(load_seg(pgtbl, ph.vaddr, ip, ph.off, ph.filesz) < 0) {
            printf("DEBUG: proc_exec: load_seg failed for ph %d va=0x%lx off=%lu filesz=%lu\n",
                   i, ph.vaddr, (uint64)ph.off, (uint64)ph.filesz);
            goto bad;
        }
            
        if(va_end > sz)
            sz = va_end;
    }

    printf("DEBUG: proc_exec: all segments loaded, sz=0x%lx\n", sz);

    inode_unlock_free(ip);
    ip = 0;

    // 5. 分配并初始化用户栈
    sz = PGROUNDUP(sz);
    printf("DEBUG: proc_exec: alloc user stack at 0x%lx size=0x%x\n", USTACK_TOP - PGSIZE, PGSIZE);
    uint64 stack_va = USTACK_TOP - PGSIZE;
    char *stack_mem = (char*)pmem_alloc(false);
    if(stack_mem == 0) {
        printf("DEBUG: proc_exec: pmem_alloc for stack failed");
        goto bad;
    }
    
    if(vm_mappages(pgtbl, stack_va, (uint64)stack_mem, PGSIZE, PTE_R|PTE_W|PTE_U) != 0) {
        printf("DEBUG: proc_exec: stack mapping failed");
        goto bad;
    }

    sp = USTACK_TOP;
    stackbase = USTACK_TOP - PGSIZE;
    printf("DEBUG: proc_exec: stack mapped, sp=0x%lx stackbase=0x%lx\n", sp, stackbase);

    // 6. 参数压栈
    // 这里增加一个检查：防止 argv 指针本身是坏的
    if (argv == 0) {
        printf("DEBUG: proc_exec: argv is NULL");
        goto bad;
    }

    printf("DEBUG: proc_exec: begin argv copy\n");
    for(argc = 0; argv[argc]; argc++) {
        printf("DEBUG: processing argv[%d] = '%s'\n", argc, argv[argc]);
        uint64 len = strlen(argv[argc]) + 1;
        sp -= len;
        sp -= (sp % 16);
        
        if(sp < stackbase) {
             printf("DEBUG: proc_exec: stack overflow during args copy");
             goto bad;
        }
        
        // 注意：这里没有检查 uvm_copyout 的返回值
        uvm_copyout(pgtbl, sp, (uint64)argv[argc], len);
        ustack[argc] = sp;
    }
    ustack[argc] = 0;

    sp -= (argc + 1) * sizeof(uint64);
    sp -= (sp % 16);
    if(sp < stackbase) {
         printf("DEBUG: proc_exec: stack overflow during argv pointers copy");
         goto bad;
    }
    
    uvm_copyout(pgtbl, sp, (uint64)ustack, (argc + 1) * sizeof(uint64));

    // 成功
    printf("DEBUG: proc_exec: argv copied, final sp=0x%lx argc=%lu\n", sp, argc);

    printf("DEBUG: proc_exec: success. entry=0x%lx sp=0x%lx argc=%lu\n", elf.entry, sp, argc);

    p->tf->a0 = argc;
    p->tf->a1 = sp;
    p->tf->sp = sp;
    p->tf->epc = elf.entry;

    oldpgtbl = p->pgtbl;
    p->pgtbl = pgtbl;
    p->heap_top = sz;
    p->ustack_pages = 1;

    uvm_destroy_pgtbl(oldpgtbl, 2);
    return argc;

bad:
    printf("DEBUG: proc_exec: GOTO BAD\n");
    if(pgtbl)
        uvm_destroy_pgtbl(pgtbl, 2);
    if(ip)
        inode_unlock_free(ip);
    return -1;
}

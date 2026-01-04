#include "lib/print.h"
#include "proc/cpu.h"
#include "mem/mmap.h"
#include "mem/vmem.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "syscall/sysfunc.h"

#define NELEM(x) (sizeof(x)/sizeof((x)[0]))

// 系统调用跳转
static uint64 (*syscalls[])(void) = {
    [SYS_exec]          sys_exec,
    [SYS_brk]           sys_brk,
    [SYS_mmap]          sys_mmap,
    [SYS_munmap]        sys_munmap,
    [SYS_fork]          sys_fork,
    [SYS_wait]          sys_wait,
    [SYS_exit]          sys_exit,
    [SYS_sleep]         sys_sleep,
    [SYS_open]          sys_open,
    [SYS_close]         sys_close,
    [SYS_read]          sys_read,
    [SYS_write]         sys_write,
    [SYS_lseek]         sys_lseek,
    [SYS_dup]           sys_dup,
    [SYS_fstat]         sys_fstat,
    [SYS_getdir]        sys_getdir,
    [SYS_mkdir]         sys_mkdir,
    [SYS_chdir]         sys_chdir,
    [SYS_link]          sys_link,
    [SYS_unlink]        sys_unlink,
};

// 系统调用
void syscall()
{
    proc_t *p = myproc();
    // 从 Trapframe 中获取用户传入的系统调用号 (位于 a7 寄存器)
    uint64 num = p->tf->a7;

    // 检查系统调用号是否有效，并且对应的函数已经实现
    if (num >= 0 && num < NELEM(syscalls) && syscalls[num]) {
        // 调用对应的系统调用处理函数
        // 函数的返回值将被存入 a0 寄存器，以便返回给用户进程
        p->tf->a0 = syscalls[num]();
    } else {
        // 如果系统调用号无效或未实现，打印错误信息
        printf("%d: unknown sys call %d", p->pid, num);
        // 返回 -1 表示出错
        p->tf->a0 = -1;
    }
}

/*
    其他用于读取传入参数的函数
    参数分为两种,第一种是数据本身,第二种是指针
    第一种使用tf->ax传递
    第二种使用uvm_copyin 和 uvm_copyinstr 进行传递
*/

// 读取 n 号参数,它放在 an 寄存器中
static uint64 arg_raw(int n)
{   
    proc_t* proc = myproc();
    switch(n) {
        case 0:
            return proc->tf->a0;
        case 1:
            return proc->tf->a1;
        case 2:
            return proc->tf->a2;
        case 3:
            return proc->tf->a3;
        case 4:
            return proc->tf->a4;
        case 5:        
            return proc->tf->a5;
        default:
            panic("arg_raw: illegal arg num");
            return -1;
    }
}

// 读取 n 号参数, 作为 uint32 存储
void arg_uint32(int n, uint32* ip)
{
    *ip = arg_raw(n);
}

// 读取 n 号参数, 作为 uint64 存储
void arg_uint64(int n, uint64* ip)
{
    *ip = arg_raw(n);
}

// 读取 n 号参数指向的字符串到 buf, 字符串最大长度是 maxlen
void arg_str(int n, char* buf, int maxlen)
{
    proc_t* p = myproc();
    uint64 addr;
    arg_uint64(n, &addr);

    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, maxlen);
}
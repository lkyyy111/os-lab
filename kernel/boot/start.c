#include "riscv.h"
#include "dev/timer.h"

int main(void);

__attribute__ ((aligned (16))) uint8 CPU_stack[4096 * NCPU];

void start()
{
    unsigned long x = r_mstatus();
    x &= ~MSTATUS_MPP_MASK;
    x |= MSTATUS_MPP_S;              // MRET后进入S-mode
    w_mstatus(x);

    w_mepc((uint64)main);             // 返回地址设为 main()
    w_satp(0);                        // 关闭分页机制

    // 所有异常委托给S-mode
    w_medeleg(0xffff);
    w_mideleg(0xffff);

    // SIE打开三类中断: 外部 / 定时 / 软件
    w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

    // 允许访问所有物理内存
    w_pmpaddr0(0x3fffffffffffffull);
    w_pmpcfg0(0xf);

    int id = r_mhartid();
    w_tp(id);

    // 初始化 M-mode 定时器
    timer_init();

    asm volatile("mret");             // 跳到 main()，进入S-mode
}

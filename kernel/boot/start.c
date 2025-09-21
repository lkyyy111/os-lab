#include "riscv.h"

int main(void);

__attribute__ ((aligned (16))) uint8 CPU_stack[4096 * NCPU];

void start()
{
    unsigned long x = r_mstatus();
    x &= ~MSTATUS_MPP_MASK;
    x |= MSTATUS_MPP_S;
    w_mstatus(x);

    w_mepc((uint64)main);

    w_satp(0);

    w_medeleg(0xffff);
    w_mideleg(0xffff);
    w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

    w_pmpaddr0(0x3fffffffffffffull);
    w_pmpcfg0(0xf);

    int id = r_mhartid();
    w_tp(id);

    asm volatile("mret");
}
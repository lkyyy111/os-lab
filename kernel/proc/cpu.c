#include "proc/cpu.h"
#include "riscv.h"

static cpu_t cpus[NCPU];

cpu_t* mycpu(void)
{
    int id = mycpuid();
    struct cpu *c = &cpus[id];
    return c;
}

int mycpuid(void) 
{
    int id =r_tp();
    return id;
}

proc_t* myproc(void)
{
    return mycpu()->proc;
}

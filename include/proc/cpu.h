#ifndef __CPU_H__
#define __CPU_H__

#include "common.h"
#include "proc/proc.h"

typedef struct cpu {
    int noff;       // 关中断的深度
    int intena;     // 第一次关中断前的状态
    proc_t* proc;
    context_t ctx;
} cpu_t;

int     mycpuid(void);
cpu_t*  mycpu(void);
proc_t* myproc(void);

#endif
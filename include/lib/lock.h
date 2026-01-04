#ifndef __LOCK_H__
#define __LOCK_H__

#include "common.h"

typedef struct spinlock {
    int locked;
    char* name;
    struct cpu *cpu;
} spinlock_t;

void push_off();
void pop_off();

void spinlock_init(spinlock_t* lk, char* name);
void spinlock_acquire(spinlock_t* lk);
void spinlock_release(spinlock_t* lk);
bool spinlock_holding(spinlock_t* lk); 


typedef struct sleeplock {
    uint locked;       // 锁的状态：0=未被持有，1=被持有
    spinlock_t lk;     // 用于保护该结构体本身的自旋锁
    
    // 调试信息
    char *name;        // 锁名称
    int pid;           // 持有该锁的进程PID
} sleeplock_t;

void sleeplock_init(sleeplock_t *lk, char *name);
void sleeplock_acquire(sleeplock_t *lk);
void sleeplock_release(sleeplock_t *lk);
bool sleeplock_holding(sleeplock_t *lk);

#endif
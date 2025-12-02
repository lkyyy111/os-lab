#ifndef __COMMON_H__
#define __COMMON_H__

// 通用整数类型
typedef char                   int8;
typedef short                  int16;
typedef int                    int32;
typedef long long              int64;

typedef unsigned char          uint8; 
typedef unsigned short         uint16;
typedef unsigned int           uint32;
typedef unsigned long long     uint64;

// xv6常用的简写类型
typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

// 页表项类型（Page Directory Entry）
typedef uint64 pde_t;

typedef unsigned long long reg; 
typedef enum {false = 0, true = 1} bool;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define NCPU 2

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// 页面大小相关
#define PGSIZE 4096
#define MAXVA  (1UL << 38)
#define PGSHIFT 12

#define KERNEL_PAGES 512           // 内核保留物理页数量


#endif

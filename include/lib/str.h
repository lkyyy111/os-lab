#ifndef __STR_H__
#define __STR_H__

#include "common.h"  // 包含 uint、uchar 这些类型

// 内存操作
void* memset(void *dst, int c, uint n);
int memcmp(const void *v1, const void *v2, uint n);
void* memmove(void *dst, const void *src, uint n);
void* memcpy(void *dst, const void *src, uint n);

// 字符串操作
int strncmp(const char *p, const char *q, uint n);
char* strncpy(char *s, const char *t, int n);
char* safestrcpy(char *s, const char *t, int n);
int strlen(const char *s);

#endif // __STR_H__

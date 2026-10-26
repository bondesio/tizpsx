#ifndef TIZPSX_STRING_H
#define TIZPSX_STRING_H
#include <stddef.h>
extern void *memcpy(void *,const void *,size_t);
extern void *memset(void *,int,size_t);
extern int strcmp(const char *,const char *);
extern int strncmp(const char *,const char *,size_t);
extern size_t strlen(const char *);
extern char *strchr(const char *,int);
extern char *strrchr(const char *,int);
extern char *strstr(const char *,const char *);
#endif

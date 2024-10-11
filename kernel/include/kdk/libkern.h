/*
 * Copyright (c) 2022-2023 NetaScale Object Solutions.
 * Created in 2022.
 */

#ifndef KRX_LIBKERN_LIBKERN_H
#define KRX_LIBKERN_LIBKERN_H

#ifdef __cplusplus
#define restrict __restrict
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* sys/param.h */
#define MIN2(a, b) (((a) < (b)) ? (a) : (b))

/* ctype.h */
int isalpha(int c);
int isdigit(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);

#define isalnum(c__) (isalpha(c__) || isdigit(c__))

/* stdlib.h */
int atoi(const char *s);

/* string.h */
int memcmp(const void *str1, const void *str2, size_t count);
void *memcpy(void *restrict dstv, const void *restrict srcv, size_t len);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *b, int c, size_t len);

int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strdup(const char *src);
char *strcpy(char *restrict dst, const char *restrict src);
char *strncpy(char *restrict dst, const char *restrict src, size_t n);
size_t strlen(const char *str);
char *strchr(const char *str, int c);
char *strrchr(const char *str, int c);

char *strtok_r(char *s, const char *delim, char **last);

/* vm/copyinout.c */
int memcpy_from_user(void *dst, const void *src, size_t len);
int memcpy_to_user(void *dst, const void *src, size_t len);
size_t strllen_user(const char *s, size_t strsz);
int strlcpy_from_user(char *dst, const char *src, size_t dstsize);
size_t strldup_user(char **dst, const char *src, size_t strsz);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRX_LIBKERN_LIBKERN_H */

#ifndef MLX_LIBKERN_LIBKERN_H
#define MLX_LIBKERN_LIBKERN_H

#include <stdint.h>
#include <stddef.h>

/* ctype.h */
int isalpha(char c);
int isdigit(char c);
int isspace(char c);
int isupper(char c);
int islower(char c);
int isxdigit(char c);
int tolower(int c);

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

#endif /* MLX_LIBKERN_LIBKERN_H */

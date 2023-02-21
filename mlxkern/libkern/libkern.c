/*
 * Copyright (c) 2022-2023 The Melantix Project.
 * Created in 2022.
 */

#include "kdk/libkern.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"

/* ctype.h */
int
isalpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int
isdigit(char c)
{
	if ((c >= '0') && (c <= '9'))
		return 1;
	return 0;
}

int
isspace(char c)
{
	if ((c == ' ') || (c <= '\t') || (c == '\n') || c == '\r' ||
	    c == '\v' || c == '\f')
		return 1;
	return 0;
}

int
isupper(char c)
{
	return (c >= 'A' && c <= 'Z');
}

int
islower(char c)
{
	return (c >= 'a' && c <= 'z');
}

int
isxdigit(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
	    (c >= 'A' && c <= 'F');
}

int
tolower(int c)
{
	if (c <= 0x7F) {
		if (c >= 'A' && c <= 'Z')
			return c - 'A' + 'a';
	} else
		kdprintf("tolower unicode: unimplemented\n");
	return c;
}

/* stdlib.h */

/*
 * from muslibc
 */
int
atoi(const char *s)
{
	int n = 0, neg = 0;
	while (isspace(*s))
		s++;
	switch (*s) {
	case '-':
		neg = 1;
	case '+':
		s++;
	}
	/* Compute n as a negative number to avoid overflow on INT_MIN */
	while (isdigit(*s))
		n = 10 * n - (*s++ - '0');
	return neg ? n : -n;
}

int
memcmp(const void *str1, const void *str2, size_t count)
{
	register const unsigned char *c1, *c2;

	c1 = (const unsigned char *)str1;
	c2 = (const unsigned char *)str2;

	while (count-- > 0) {
		if (*c1++ != *c2++)
			return c1[-1] < c2[-1] ? -1 : 1;
	}
	return 0;
}

void *
memcpy(void *restrict dstv, const void *restrict srcv, size_t len)
{
	unsigned char	    *dst = (unsigned char *)dstv;
	const unsigned char *src = (const unsigned char *)srcv;
	for (size_t i = 0; i < len; i++)
		dst[i] = src[i];
	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	const char *f = src;
	char	   *t = dst;

	if (f < t) {
		f += n;
		t += n;
		while (n-- > 0)
			*--t = *--f;
	} else
		while (n-- > 0)
			*t++ = *f++;
	return dst;
}

void *
memset(void *b, int c, size_t len)
{
	char *ss = b;
	while (len-- > 0)
		*ss++ = c;
	return b;
}

int
strcmp(const char *s1, const char *s2)
{
	while (*s1 == *s2++)
		if (*s1++ == 0)
			return (0);
	return (*(const unsigned char *)s1 - *(const unsigned char *)--s2);
}

int
strncmp(const char *s1, const char *s2, size_t n)
{

	if (n == 0)
		return (0);
	do {
		if (*s1 != *s2++)
			return (*(unsigned char *)s1 - *(unsigned char *)--s2);
		if (*s1++ == 0)
			break;
	} while (--n != 0);
	return (0);
}

char *
strdup(const char *src)
{
	size_t size = strlen(src) + 1;
	char  *str = kmem_alloc(size);
	memcpy(str, src, size);
	return str;
}

char *
strcpy(char *restrict dst, const char *restrict src)
{
	while ((*dst++ = *src++))
		;
	return dst;
}

char *
strncpy(char *restrict dst, const char *restrict src, size_t n)
{
	if (n != 0) {
		register char	    *d = dst;
		register const char *s = src;

		do {
			if ((*d++ = *s++) == 0) {
				while (--n != 0)
					*d++ = 0;
				break;
			}
		} while (--n != 0);
	}
	return (dst);
}

size_t
strlen(const char *str)
{
	const char *s;

	for (s = str; *s; ++s)
		continue;
	return (s - str);
}
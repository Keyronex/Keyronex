/*
 * Copyright (c) 2022-2023 NetaScale Object Solutions.
 * Created in 2022.
 */

#include "kdk/kern.h"
#include "kdk/libkern.h"
#include "kdk/kmem.h"

/* ctype.h */
int
isalpha(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int
isdigit(int c)
{
	if ((c >= '0') && (c <= '9'))
		return 1;
	return 0;
}

int
isspace(int c)
{
	if ((c == ' ') || (c <= '\t') || (c == '\n') || c == '\r' ||
	    c == '\v' || c == '\f')
		return 1;
	return 0;
}

int
isupper(int c)
{
	return (c >= 'A' && c <= 'Z');
}

int
islower(int c)
{
	return (c >= 'a' && c <= 'z');
}

int
isxdigit(int c)
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
		kprintf("tolower unicode: unimplemented\n");
	return c;
}

int toupper(int c)
{
	return (c >= 'a' && c <= 'z' ? 'A' + (c - 'a') : c);
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
	unsigned char *dst = (unsigned char *)dstv;
	const unsigned char *src = (const unsigned char *)srcv;
	for (size_t i = 0; i < len; i++)
		dst[i] = src[i];
	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	const char *f = src;
	char *t = dst;

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
	char *str = kmem_alloc(size);
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
		register char *d = dst;
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

size_t
strnlen(const char *str, size_t maxlen)
{
	const char *cp;

	for (cp = str; maxlen != 0 && *cp != '\0'; cp++, maxlen--)
		;

	return (size_t)(cp - str);
}

char *
strchr(const char *p, int ch)
{
	do {
		if (*p == (char)ch)
			return ((char *)p);
		if (!*p)
			return ((char *)NULL);
	} while (++p);
	kfatal("strchr");
}

char *
strrchr(const char *str, int c)
{
	char *p = NULL;

	while (++str) {
		if (*str == (char)c)
			p = (char *)str;
		else if (*str == '\0')
			return p;
	}
	kfatal("strrchr");
}

int
snprintf(char *str, size_t size, const char *format, ...)
{
	int r;
	va_list ap;
	va_start(ap, format);
	r = npf_vsnprintf(str, size, format, ap);
	va_end(ap);
	return r;
}

size_t
strspn(const char *s1, const char *s2)
{
	size_t ret = 0;
	while (*s1 && strchr(s2, *s1++))
		ret++;
	return ret;
}

size_t
strcspn(const char *s1, const char *s2)
{
	size_t ret = 0;
	while (*s1)
		if (strchr(s2, *s1))
			return ret;
		else
			s1++, ret++;
	return ret;
}

char *
strtok_r(char *str, const char *delim, char **nextp)
{
	char *ret;

	if (str == NULL) {
		str = *nextp;
	}

	str += strspn(str, delim);

	if (*str == '\0') {
		return NULL;
	}

	ret = str;

	str += strcspn(str, delim);

	if (*str) {
		*str++ = '\0';
	}

	*nextp = str;

	return ret;
}

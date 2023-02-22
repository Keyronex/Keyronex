/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Wed Feb 22 2023.
 */

int __dso_handle = 0;

extern "C" int
__cxa_atexit(void (*function)(void *), void *argument, void *dso_tag)
{
	return 0;
}